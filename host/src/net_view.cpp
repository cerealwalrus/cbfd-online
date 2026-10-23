// Full-screen view for the online session (netplay milestone 3).
//
// The game's multiplayer is split screen: one camera per player (D_800BE628, 0x180 bytes each; for 2 or more
// players the camera at index `players` is laid out full screen too). Online every machine runs the whole game
// (lockstep): what the game's passes do while they run (what they cull, the random numbers they draw, ...) feeds
// the game's logic, so they have to run exactly the same on every machine. So nothing of the game's memory is
// touched while the game runs, and the passes draw as for split screen. Only the finished display list of the
// frame is rewritten, at its end, before it is drawn:
//  - this machine's own camera's passes get the full-screen camera's viewport (the viewport command points at the
//    camera's record: it points at the full-screen camera's instead), the full-screen scissor, the background fill
//    over the whole view, and their projection matrix (a copy of it, in memory past the game's) widened to the
//    full-screen shape,
//  - the other cameras' passes get a scissor with no area, so what they draw is not seen.
// CONKER_NET_VIEW=split turns this off (the game's split screen is shown).

#include <set>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <SDL.h>

#include "recomp.h"

#include "ultramodern/config.hpp"

#include "conker.hpp"

namespace {
    // The look kinds (+0x23E of the look camera) in which the screen-space sprites are moved down with the full view: the rocket launcher's
    // (0x16) and the sniper scope's (0x24: what the log showed while aiming it; CONKER_NET_SPRITE_KINDS="16,15" (hex, comma-separated) replaces the list to try others).
    const std::set<int>& sprite_kinds() {
        static const std::set<int> kinds = []() {
            std::set<int> out;
            if (const char* text = std::getenv("CONKER_NET_SPRITE_KINDS")) {
                const char* p = text;
                while (*p != 0) {
                    char* end = nullptr;
                    const long v = std::strtol(p, &end, 16);
                    if (end == p) {
                        p++;
                        continue;
                    }
                    out.insert((int)v);
                    p = end;
                }
            }
            if (out.empty()) {
                out = { 0x16, 0x24 };
            }
            return out;
        }();
        return kinds;
    }

    constexpr gpr cameras_pointer = (gpr)(int32_t)0x800BE628;
    constexpr gpr layout_variable = (gpr)(int32_t)0x80082FA0;   // D_80082FA0: players - 1
    constexpr uint32_t camera_size = 0x180;
    constexpr int max_cameras = 4;

    // The camera record's fields (floats on the game's 292 x 216 frame).
    constexpr uint32_t field_top = 0x24, field_bottom = 0x28, field_left = 0x2C, field_right = 0x30;
    constexpr uint32_t field_viewports = 0x40;

    // Matrices of ours, in RDRAM past the game's 8 MB (widescreen.cpp's and cutscene_aspect.cpp's lists are at
    // 0x00F00000 and 0x00F10000): a ring, as RT64 has long finished with one when it comes round again.
    constexpr uint32_t matrix_start = 0x00F30000;
    constexpr uint32_t matrix_slot = 0x40;
    constexpr uint32_t matrix_slots = 128;
    constexpr uint32_t viewport_start = 0x00F40000;   // copies of viewports (16 bytes), the same way
    constexpr uint32_t viewport_slots = 256;
    // (a fill of this machine's camera's rectangle, a tint over its view, is drawn as RT64's extended fill reaching past the 4:3 frame's
    // sides: from a little list of ours the fill's command calls, a ring of 64 of 32 bytes)
    constexpr uint32_t tint_start = 0x00F44000;
    constexpr uint32_t tint_slot = 0x20;
    constexpr uint32_t tint_slots = 64;
    uint32_t tint_next = 0;
    // (HUD Placement: Expand: the fills of this machine's HUD at the screen's sides, as RT64's extended fill with an origin, from
    // little lists of ours the fill's command calls, a ring of 128 of 32 bytes)
    constexpr uint32_t edge_fill_start = 0x00F45000;
    constexpr uint32_t edge_fill_slot = 0x20;
    constexpr uint32_t edge_fill_slots = 128;
    uint32_t edge_fill_next = 0;
    constexpr uint32_t list_start = 0x00F50000;       // copies of the lists the HUD calls (a ring of 32 slots of 1024 commands)
    constexpr uint32_t list_slot = 0x2000;
    constexpr uint32_t list_slots = 32;
    uint32_t list_next = 0;


    bool view_is_full() {
        static int mode = -1;
        if (mode < 0) {
            const char* value = SDL_getenv("CONKER_NET_VIEW");
            mode = (value != nullptr && std::string(value) == "split") ? 0 : 1;
        }
        return mode == 1 && conker::netsession::active();
    }

    struct Pass {
        uint32_t gfx;     // where the pass (or the camera's setup) starts writing its commands
        int camera;
        int kind;         // 1: 3D pass (func_15019464), 2: HUD pass (func_15019BB8), 0: just func_1501A490's setup
    };
    std::vector<Pass> passes;

    // The display list words a frame's rewrite changed, to be put back once the renderer has read the list.
    struct Journal {
        uint32_t start;
        std::vector<std::array<uint32_t, 3>> words;   // address, original, rewritten
    };
    std::mutex journal_mutex;
    std::deque<Journal> journals;
    uint32_t matrix_next = 0, viewport_next = 0;
    int dumps_done = 0;
    uint64_t frames_rewritten = 0;

    int players_now(uint8_t* rdram) {
        return (int32_t)MEM_W(0, layout_variable) + 1;
    }

    float get_float(uint8_t* rdram, uint32_t address, uint32_t offset) {
        const uint32_t word = (uint32_t)MEM_W(offset, (gpr)(int32_t)address);
        float value;
        std::memcpy(&value, &word, sizeof(value));
        return value;
    }

    uint32_t command_word(uint8_t* rdram, uint32_t gfx, int index, int word) {
        return (uint32_t)MEM_W(index * 8 + word * 4, (gpr)(int32_t)gfx);
    }

    void set_command_word(uint8_t* rdram, uint32_t gfx, int index, int word, uint32_t value) {
        MEM_W(index * 8 + word * 4, (gpr)(int32_t)gfx) = (int32_t)value;
    }

    void dump_cameras(uint8_t* rdram, uint32_t base, int players) {
        char line[240];
        for (int cam = 0; cam <= players; cam++) {
            const uint32_t record = base + cam * camera_size;
            std::snprintf(line, sizeof(line), "[net view] camera %d: top %g bottom %g left %g right %g, fov %g %g, vp %08X %08X %08X %08X", cam,
                get_float(rdram, record, field_top), get_float(rdram, record, field_bottom), get_float(rdram, record, field_left),
                get_float(rdram, record, field_right), get_float(rdram, record, 0x74), get_float(rdram, record, 0x78),
                (uint32_t)MEM_W(0x40, (gpr)(int32_t)record), (uint32_t)MEM_W(0x44, (gpr)(int32_t)record),
                (uint32_t)MEM_W(0x48, (gpr)(int32_t)record), (uint32_t)MEM_W(0x4C, (gpr)(int32_t)record));
            conker::netsession::log(line);
        }
    }

    uint32_t scissor_word0(float left, float top) {
        return 0xED000000u | (((uint32_t)(int32_t)(left * 4.0f) & 0xFFFu) << 12) | ((uint32_t)(int32_t)(top * 4.0f) & 0xFFFu);
    }

    uint32_t scissor_word1(float right, float bottom) {
        return (((uint32_t)(int32_t)(right * 4.0f) & 0xFFFu) << 12) | ((uint32_t)(int32_t)(bottom * 4.0f) & 0xFFFu);
    }

    // CONKER_NET_AIMFIT (0..1) overrides it for every machine. Unset: 1, the exact fit.
    float aim_fit(int local) {
        static float value = -2.0f;
        if (value < -1.5f) {
            value = -1.0f;
            const char* text = SDL_getenv("CONKER_NET_AIMFIT");
            if (text != nullptr && *text != 0) {
                value = std::min(std::max((float)std::atof(text), 0.0f), 1.0f);
            }
        }
        (void)local;
        return value >= 0.0f ? value : 1.0f;
    }

    // A copy of the matrix at `address` (N64 fixed point: 16 integer halves, then 16 fractions) with its first
    // column scaled, in our own memory; returns where the copy is.
    uint32_t scaled_matrix_copy(uint8_t* rdram, uint32_t address, float scale_x, float scale_y) {
        const uint32_t copy = 0x80000000u | (matrix_start + (matrix_next % matrix_slots) * matrix_slot);
        matrix_next++;
        for (int w = 0; w < 16; w++) {
            MEM_W(w * 4, (gpr)(int32_t)copy) = MEM_W(w * 4, (gpr)(int32_t)address);
        }
        const auto scale_value = [](int32_t value, float scale) {
            const double scaled = (double)value * (double)scale;
            return scaled > 2147483647.0 ? 2147483647 : (scaled < -2147483648.0 ? (int32_t)0x80000000 : (int32_t)scaled);
        };
        for (int row = 0; row < 4; row++) {
            uint32_t integer_word = (uint32_t)MEM_W(row * 8, (gpr)(int32_t)copy);
            uint32_t fraction_word = (uint32_t)MEM_W(32 + row * 8, (gpr)(int32_t)copy);
            // element [row][0] is the high half of both words, [row][1] the low half (16.16 together)
            const int32_t x = scale_value((int32_t)((integer_word & 0xFFFF0000u) | (fraction_word >> 16)), scale_x);
            const int32_t y = scale_value((int32_t)((integer_word << 16) | (fraction_word & 0xFFFFu)), scale_y);
            integer_word = ((uint32_t)x & 0xFFFF0000u) | ((uint32_t)y >> 16);
            fraction_word = (((uint32_t)x & 0xFFFFu) << 16) | ((uint32_t)y & 0xFFFFu);
            MEM_W(row * 8, (gpr)(int32_t)copy) = (int32_t)integer_word;
            MEM_W(32 + row * 8, (gpr)(int32_t)copy) = (int32_t)fraction_word;
        }
        return copy;
    }

    // A copy of the viewport at `address` with its x and y scale zero, so that everything drawn through it collapses
    // to a point: for the other players' cameras, whose passes stay in the frame's list but must not draw.
    uint32_t collapsed_viewport_copy(uint8_t* rdram, uint32_t address) {
        const uint32_t copy = 0x80000000u | (viewport_start + (viewport_next % viewport_slots) * 16u);
        viewport_next++;
        for (int w = 0; w < 4; w++) {
            MEM_W(w * 4, (gpr)(int32_t)copy) = MEM_W(w * 4, (gpr)(int32_t)address);
        }
        MEM_W(0, (gpr)(int32_t)copy) = 0;
        return copy;
    }

    // A copy of the viewport at `address` with its centre moved down by `shift` (in the viewport's own units, 4 to a
    // pixel), in our own memory; returns where the copy is.
    uint32_t shifted_viewport_copy(uint8_t* rdram, uint32_t address, int32_t shift, int32_t shift_x) {
        const uint32_t copy = 0x80000000u | (viewport_start + (viewport_next % viewport_slots) * 16u);
        viewport_next++;
        for (int w = 0; w < 4; w++) {
            MEM_W(w * 4, (gpr)(int32_t)copy) = MEM_W(w * 4, (gpr)(int32_t)address);
        }
        const uint32_t translation = (uint32_t)MEM_W(8, (gpr)(int32_t)copy);   // x (high half) and y (low half)
        const int32_t y = (int16_t)(translation & 0xFFFFu) + shift;
        const int32_t x = (int16_t)(translation >> 16) + shift_x;
        MEM_W(8, (gpr)(int32_t)copy) = (int32_t)(((uint32_t)x << 16) | ((uint32_t)y & 0xFFFFu));
        return copy;
    }

    // The whole frame's display list, from the first pass to its end (`end`), rewritten for this machine's view.
    void rewrite_frame(uint8_t* rdram, const std::vector<Pass>& frame, uint32_t end, uint32_t base, int players, int local, bool log_it) {
        uint32_t start = 0xFFFFFFFFu;
        for (const Pass& pass : frame) {
            if (pass.gfx < start) {
                start = pass.gfx;
            }
        }
        if ((start >> 24) != 0x80 || (end >> 24) != 0x80 || end <= start || end - start > 0x100000u || (end & 0x7FFFFFu) > 0x800000u) {
            return;
        }
        const uint32_t full = base + players * camera_size;
        const float left = get_float(rdram, full, field_left), right = get_float(rdram, full, field_right);
        const float top = get_float(rdram, full, field_top), bottom = get_float(rdram, full, field_bottom);
        // each camera's scissor as the game writes it, and this machine's camera's viewport words
        uint32_t scissor0[max_cameras], scissor1[max_cameras];
        for (int k = 0; k < players; k++) {
            const uint32_t record = base + k * camera_size;
            scissor0[k] = scissor_word0(get_float(rdram, record, field_left), get_float(rdram, record, field_top));
            scissor1[k] = scissor_word1(get_float(rdram, record, field_right), get_float(rdram, record, field_bottom));
        }
        const uint32_t own = base + local * camera_size;
        // how much wider the camera's view is than the full-screen one, as the viewports show it
        // The game puts its aim (the reticle, the turret's offsets) in pixels of the camera's own rectangle, which is
        // small. For a shot to go where the reticle is on the full screen, the world has to be drawn at the same
        // pixels per angle as that rectangle: both axes of the projection are scaled to the rectangle's.
        // CONKER_NET_AIMFIT (default 1) blends from 0, the old look (the right height, widened to the screen's shape),
        // to 1, the exact match (a wider view).
        float shape = 1.0f, scale_x = 1.0f, scale_y = 1.0f;
        {
            const float own_x = (float)(int16_t)((uint32_t)MEM_W(field_viewports, (gpr)(int32_t)own) >> 16);
            const float own_y = (float)(int16_t)((uint32_t)MEM_W(field_viewports, (gpr)(int32_t)own) & 0xFFFF);
            const float full_x = (float)(int16_t)((uint32_t)MEM_W(field_viewports, (gpr)(int32_t)full) >> 16);
            const float full_y = (float)(int16_t)((uint32_t)MEM_W(field_viewports, (gpr)(int32_t)full) & 0xFFFF);
            if (own_x > 0.0f && own_y > 0.0f && full_x > 0.0f && full_y > 0.0f) {
                shape = (full_y / full_x) / (own_y / own_x);
                const float fit = aim_fit(local);
                scale_x = std::pow(shape, 1.0f - fit) * std::pow(own_x / full_x, fit);
                scale_y = std::pow(own_y / full_y, fit);
            }
        }
        // CONKER_NET_VIEWMASK (a number, default 31) picks which rewrites are made, to find which one the game's logic
        // is sensitive to: 1 viewports, 2 scissors, 4 fills of the camera's rectangle, 8 projection matrices, 16 the HUD.
        static const unsigned mask = []() { const char* text = SDL_getenv("CONKER_NET_VIEWMASK"); return text != nullptr ? (unsigned)std::strtoul(text, nullptr, 0) : 31u; }();
        static const bool hud_layout = [&]() { const char* text = SDL_getenv("CONKER_NET_HUD"); return (text == nullptr || std::string(text) != "old") && (mask & 16u) != 0; }();
        int redirected = 0, shown = 0, hidden = 0, matrices = 0, fills = 0, logged = 0, rects_logged = 0, rects_moved = 0, rects_hidden = 0;
        // where this machine's camera's 3D passes and its HUD passes are (each ends where the next pass starts)
        std::vector<std::pair<uint32_t, uint32_t>> segments, hud_segments;
        std::vector<std::pair<uint32_t, uint32_t>> any_hud_segments;   // every camera's HUD passes
        // what the game draws after the cameras' 3D passes, for no camera (camera 255): among it each team's health bar, a
        // rectangle for each row of its bricks, at its camera's corner
        std::vector<std::pair<uint32_t, uint32_t>> bar_segments;
        for (size_t i = 0; i < frame.size(); i++) {
            if (frame[i].kind == 0) {
                if (frame[i].camera == 255) {
                    bar_segments.push_back({ frame[i].gfx, i + 1 < frame.size() ? frame[i + 1].gfx : end });
                }
                continue;
            }
            size_t next = i + 1;
            while (next < frame.size() && frame[next].kind == 0) {
                next++;
            }
            const uint32_t stop = next < frame.size() ? frame[next].gfx : end;
            if (frame[i].kind == 2) {
                any_hud_segments.push_back({ frame[i].gfx, stop });
            }
            if (frame[i].camera != local) {
                continue;
            }
            (frame[i].kind == 1 ? segments : hud_segments).push_back({ frame[i].gfx, stop });
        }
        // every pass of this machine's camera except its HUD (3D passes and the passes of what is drawn in the world: characters, effects, sprites)
        std::vector<std::pair<uint32_t, uint32_t>> local_world_passes;
        for (size_t i = 0; i < frame.size(); i++) {
            if (frame[i].camera == local && frame[i].kind != 2) {
                local_world_passes.push_back({ frame[i].gfx, i + 1 < frame.size() ? frame[i + 1].gfx : end });
            }
        }
        const auto inside = [](const std::vector<std::pair<uint32_t, uint32_t>>& list, uint32_t address) {
            for (const auto& item : list) {
                if (address >= item.first && address < item.second) {
                    return true;
                }
            }
            return false;
        };
        // the HUD is laid out for the camera's own rectangle (its middle is where the game aims): moved so that
        // the middle is the full-screen view's
        const int32_t hud_shift = (int32_t)(int16_t)((uint32_t)MEM_W(field_viewports + 8, (gpr)(int32_t)full) & 0xFFFFu) -
            (int32_t)(int16_t)((uint32_t)MEM_W(field_viewports + 8, (gpr)(int32_t)own) & 0xFFFFu);
        // (and sideways: the same, from the translation's high half; nothing with the two players' halves, which are as wide as the screen)
        const int32_t hud_shift_x = (int32_t)(int16_t)((uint32_t)MEM_W(field_viewports + 8, (gpr)(int32_t)full) >> 16) -
            (int32_t)(int16_t)((uint32_t)MEM_W(field_viewports + 8, (gpr)(int32_t)own) >> 16);
        // a one-time dump (the fourth logged frame, in play) of every command in the HUD passes, to see how the icons and
        // the health bars are drawn
        static int logged_frames = 0;
        const bool dump_hud = log_it && ++logged_frames == 4;
        if (dump_hud) {
            for (const auto& segment : any_hud_segments) {
                char line[120];
                std::snprintf(line, sizeof(line), "[net view] HUD pass %08X..%08X (camera %d is %d)", segment.first, segment.second, 0, local);
                conker::netsession::log(line);
            }
        }
        // The rocket launcher's look (kind 0x16 of this machine's player's camera): the green overlay it draws is a rectangle as large as the
        // camera's own, placed for the split screen's half and not moved with the view. It is hidden (below).
        bool rocket_look = false;
        int local_kind = 0;
        bool remote_sniper = false;   // another player is aiming the sniper (its laser's sprite is the other machine's to place: logged)
        bool sprite_look = false;   // a look whose sprites are moved with the view (see sprite_kinds)
        unsigned rocket_mask = 0;   // every player whose look is the rocket launcher's
        {
            // (the look cameras, another array than the view's: pointer at 0x800DBFF0, 0x9A0 apart, the player at +0x23D, the look's kind at +0x23E)
            const uint32_t look_base = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
            if ((look_base >> 24) == 0x80 && (look_base & 0x7FFFFFu) < 0x7F0000u) {
                for (int k = 0; k < 4; k++) {
                    const gpr record = (gpr)(int32_t)(look_base + (uint32_t)k * 0x9A0u);
                    if ((int)MEM_BU(0x23D, record) == local && MEM_BU(0x23E, record) != 0) {
                        // This machine's look (a player can have more than one record: the one with a kind is the look in use). The kind is
                        // logged when it changes, and the looks listed in sprite_kinds() have the game's screen-space sprites (the rocket's
                        // explosion, the sniper laser's star) moved with the view, as the rocket launcher's are.
                        const int kind_now = (int)MEM_BU(0x23E, record);
                        if (sprite_kinds().count(kind_now) != 0) {
                            sprite_look = true;
                        }
                        local_kind = kind_now;
                    }
                    if ((int)MEM_BU(0x23D, record) != local && (int)MEM_BU(0x23D, record) < 4 && sprite_kinds().count((int)MEM_BU(0x23E, record)) != 0 && MEM_BU(0x23E, record) != 0x16) {
                        remote_sniper = true;
                    }
                    if (MEM_BU(0x23E, record) == 0x16 && (int)MEM_BU(0x23D, record) < 4) {
                        rocket_mask |= 1u << (int)MEM_BU(0x23D, record);
                        if ((int)MEM_BU(0x23D, record) == local) {
                            rocket_look = true;
                        }
                    }
                }
            }
        }
        // (a look kind seen for the first time: this frame's passes are logged, to find the green overlay of a look that isn't the
        // rocket launcher's kind 0x16, such as the teddies' zoom)
        bool dump_look = false;
        {
            static int logged_kind = -1;
            static std::set<int> kinds_dumped;
            if (local_kind != logged_kind) {
                logged_kind = local_kind;
                char line[120];
                std::snprintf(line, sizeof(line), "[net view] this machine's look kind is now %02X", local_kind);
                conker::netsession::log(line);
                if (local_kind != 0 && kinds_dumped.size() < 12 && kinds_dumped.insert(local_kind).second) {
                    dump_look = true;
                }
            }
        }
        if (dump_look) {
            for (size_t i = 0; i < frame.size(); i++) {
                const uint32_t to = i + 1 < frame.size() ? frame[i + 1].gfx : 0;
                char line[160];
                std::snprintf(line, sizeof(line), "[net view] look %02X: pass %zu at %08X (%u bytes): camera %d kind %d%s", local_kind, i, frame[i].gfx,
                    to > frame[i].gfx ? to - frame[i].gfx : 0u, frame[i].camera, frame[i].kind, frame[i].camera == local ? " (this machine's)" : "");
                conker::netsession::log(line);
            }
        }
        {
            // (a test: the first frames of each camera's rocket launcher look, as the game made them: every command of that camera's
            // passes but its world's, and the scissor the last pass before each left set, to find the green overlay of the second camera)
            // (its 2nd, 30th and 120th frames, the first time a camera has it)
            static int rocket_dumps[4] = {};
            static int rocket_frames[4] = {};
            bool dump_now[4] = {};
            for (int ovp = 0; ovp < 4; ovp++) {
                if (((rocket_mask >> ovp) & 1u) == 0) {
                    rocket_frames[ovp] = 0;
                    continue;
                }
                rocket_frames[ovp]++;
                dump_now[ovp] = rocket_dumps[ovp] < 3 && (rocket_frames[ovp] == 2 || rocket_frames[ovp] == 30 || rocket_frames[ovp] == 120);
            }
            for (int ovp = 0; ovp < 4; ovp++) if (dump_now[ovp]) {
                rocket_dumps[ovp]++;
                uint32_t scissor_w0 = 0, scissor_w1 = 0;
                int lines = 0;
                for (size_t i = 0; i < frame.size() && lines < 1200; i++) {
                    const uint32_t from = frame[i].gfx;
                    const uint32_t to = i + 1 < frame.size() ? frame[i + 1].gfx : end;
                    if ((from >> 24) != 0x80 || to <= from || to - from > 0x8000u) {
                        continue;
                    }
                    const bool dump_it = frame[i].camera == ovp && to - from <= 0x1000u;
                    if (dump_it) {
                        char line[200];
                        std::snprintf(line, sizeof(line), "[net view] rocket dump %d/3, camera %d%s: pass %zu %08X..%08X (kind %d), the scissor before it %08X %08X",
                            rocket_dumps[ovp], ovp, ovp == local ? " (this machine's)" : "", i, from, to, frame[i].kind, scissor_w0, scissor_w1);
                        conker::netsession::log(line);
                        lines++;
                    }
                    for (uint32_t address = from; address < to; address += 8) {
                        const uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)address), w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)address);
                        if ((w0 >> 24) == 0xED) {
                            scissor_w0 = w0;
                            scissor_w1 = w1;
                        }
                        if (dump_it && lines < 1200) {
                            char line[120];
                            std::snprintf(line, sizeof(line), "[net view]   %08X: %08X %08X", address, w0, w1);
                            conker::netsession::log(line);
                            lines++;
                            if ((w0 >> 24) == 0xDE && (w1 >> 24) == 0x80 && (w1 & 0x7FFFFFu) < 0x7F0000u) {
                                // one level of the lists it calls
                                for (uint32_t inner = w1, n = 0; n < 48 && lines < 1200; inner += 8, n++) {
                                    const uint32_t i0 = (uint32_t)MEM_W(0, (gpr)(int32_t)inner), i1 = (uint32_t)MEM_W(4, (gpr)(int32_t)inner);
                                    std::snprintf(line, sizeof(line), "[net view]     > %08X: %08X %08X", inner, i0, i1);
                                    conker::netsession::log(line);
                                    lines++;
                                    if ((i0 >> 24) == 0xDF) {
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        for (int ovp = 0; ovp < 4; ovp++) if (((rocket_mask >> ovp) & 1u) != 0) {
            // With the rocket launcher's look the frame has one more pass of this machine's camera before its HUD pass (a long one, its
            // setup and the green filter, then the usual short setup pass): the long one is the overlay, drawn for the split screen's half and
            // not for the full view. Its scissors are made empty, so nothing of it is seen.
            int hud_pass = -1;
            for (size_t i = 0; i < frame.size(); i++) {
                if (frame[i].kind == 2 && frame[i].camera == ovp) {
                    hud_pass = (int)i;
                }
            }
            std::vector<int> setups;
            for (int i = hud_pass - 1; i >= 0 && frame[i].kind == 0 && frame[i].camera == ovp; i--) {
                setups.push_back(i);
            }
            {
                static unsigned logged_masks = 0;
                if (ovp == local && logged_masks < 20) {
                    logged_masks++;
                    char line[160];
                    std::snprintf(line, sizeof(line), "[net view] rocket look of camera %d: HUD pass %d, %zu setup passes before it (%s)", ovp, hud_pass, setups.size(),
                        hud_pass >= 0 && setups.size() >= 2 ? "the extra one hidden" : "nothing hidden: not the expected passes");
                    conker::netsession::log(line);
                }
            }
            if (hud_pass >= 0 && setups.size() >= 2) {
                for (size_t s = 1; s < setups.size(); s++) {
                    const uint32_t from = frame[setups[s]].gfx;
                    const uint32_t to = frame[setups[s] + 1].gfx;
                    if ((from >> 24) == 0x80 && to > from && to - from <= 0x4000u) {
                        // The overlay pass sets no scissor of its own (logged), so it is drawn with the last one set: an empty one is set at its
                        // start, in place of one of its first pipe syncs.
                        for (uint32_t address = from; address < from + 40 && address < to; address += 8) {
                            const uint32_t op = (uint32_t)MEM_W(0, (gpr)(int32_t)address) >> 24;
                            if (op == 0xE7 || op == 0xE6 || op == 0xE8 || op == 0xE9) {
                                MEM_W(0, (gpr)(int32_t)address) = (int32_t)0xED000000u;
                                MEM_W(4, (gpr)(int32_t)address) = 0;
                                break;
                            }
                        }
                        for (uint32_t address = from; address < to; address += 8) {
                            if (((uint32_t)MEM_W(0, (gpr)(int32_t)address) >> 24) == 0xED && address > from + 40) {
                                MEM_W(0, (gpr)(int32_t)address) = (int32_t)0xED000000u;
                                MEM_W(4, (gpr)(int32_t)address) = 0;
                            }
                        }
                    }
                }
            }
        }
        int dumped = 0;
        if (dump_hud) {
            for (const Pass& pass : frame) {
                char line[120];
                std::snprintf(line, sizeof(line), "[net view] pass at %08X: camera %d kind %d", pass.gfx, pass.camera, pass.kind);
                conker::netsession::log(line);
            }
            // every rectangle of the frame's own list, to find where the health bars are drawn
            int listed = 0;
            for (uint32_t address = start; address < end && listed < 400; address += 8) {
                const uint32_t r0 = (uint32_t)MEM_W(0, (gpr)(int32_t)address), r1 = (uint32_t)MEM_W(4, (gpr)(int32_t)address);
                const uint32_t rop = r0 >> 24;
                if (rop == 0xE4 || rop == 0xE5 || rop == 0xF6 || rop == 0xDE || rop == 0xBF || rop == 0x05 || rop == 0x06 || rop == 0x07) {
                    char line[120];
                    std::snprintf(line, sizeof(line), "[net view] any %08X: %08X %08X%s", address, r0, r1, inside(any_hud_segments, address) ? " hud" : "");
                    conker::netsession::log(line);
                    listed++;
                }
            }
        }
        // The lists the HUD's passes call (G_DL): the icons, the health bars and the scores are drawn by them. They are
        // the game's own static lists, so they are not changed: a copy of each is rewritten (in our own memory) and the
        // call is pointed at it.
        struct Range { uint32_t from, to; bool nested; };
        std::vector<Range> ranges;
        ranges.push_back({ start, end, false });
        {
            std::vector<std::pair<uint32_t, uint32_t>> copies;   // original, copy
            const auto copy_list = [&](uint32_t source, auto&& self, int depth) -> uint32_t {
                for (const auto& item : copies) {
                    if (item.first == source) {
                        return item.second;
                    }
                }
                uint32_t last = source;
                while (last - source < list_slot - 8u && (last & 0x7FFFFFu) < 0x7FFFF8u && ((uint32_t)MEM_W(0, (gpr)(int32_t)last) >> 24) != 0xDF) {
                    last += 8;
                }
                if (((uint32_t)MEM_W(0, (gpr)(int32_t)last) >> 24) != 0xDF || copies.size() >= 24) {
                    return 0;   // not a list that ends: left as it is
                }
                const uint32_t copy = 0x80000000u | (list_start + (list_next % list_slots) * list_slot);
                list_next++;
                copies.push_back({ source, copy });
                for (uint32_t off = 0; off <= last - source; off += 4) {
                    MEM_W(off, (gpr)(int32_t)copy) = MEM_W(off, (gpr)(int32_t)(source));
                }
                const uint32_t to = copy + (last - source) + 8;
                ranges.push_back({ copy, to, true });
                if (depth < 2) {
                    for (uint32_t a = copy; a < to; a += 8) {
                        const uint32_t c0 = (uint32_t)MEM_W(0, (gpr)(int32_t)a), c1 = (uint32_t)MEM_W(4, (gpr)(int32_t)a);
                        if ((c0 >> 24) == 0xDE && ((c0 >> 16) & 0xFFu) == 0 && (c1 >> 24) == 0x80 && (c1 & 7u) == 0 && (c1 & 0x7FFFFFu) < 0x7FF000u) {
                            const uint32_t inner = self(c1, self, depth + 1);
                            if (inner != 0) {
                                MEM_W(4, (gpr)(int32_t)a) = (int32_t)inner;
                            }
                        }
                    }
                }
                return copy;
            };
            for (const auto& segment : any_hud_segments) {
                for (uint32_t a = segment.first; a < segment.second; a += 8) {
                    const uint32_t c0 = (uint32_t)MEM_W(0, (gpr)(int32_t)a), c1 = (uint32_t)MEM_W(4, (gpr)(int32_t)a);
                    if ((c0 >> 24) == 0xDE && ((c0 >> 16) & 0xFFu) == 0 && (c1 >> 24) == 0x80 && (c1 & 7u) == 0 && (c1 & 0x7FFFFFu) < 0x7FF000u) {
                        const uint32_t copy = copy_list(c1, copy_list, 0);
                        if (copy != 0) {
                            MEM_W(4, (gpr)(int32_t)a) = (int32_t)copy;
                        }
                    }
                }
            }
        }
        static const bool score_swap = []() { const char* text = SDL_getenv("CONKER_NET_SCORESWAP"); return text == nullptr || std::atoi(text) != 0; }();
        static const bool keep_old_hidden = SDL_getenv("CONKER_NET_HIDEOLD") != nullptr;
        // With the online pause menu open (net_menu.cpp) the pause screen is the game's own, drawn as it draws it: the game's HUD
        // behind it (the bars, the radars, the weapon) is hidden, as the game's pause hides it, and nothing else is moved.
        const bool menu_open = conker::netmenu::is_open();
        const auto other_viewport = [&](uint32_t pointer) {
            for (int k = 0; k < players; k++) {
                const uint32_t record = base + k * camera_size;
                if (k != local && pointer >= record + field_viewports && pointer < record + field_viewports + 0x40) {
                    return true;
                }
            }
            return false;
        };
        // HUD Placement (the graphics settings): with Expand, this machine's HUD at the screen's sides (the radar, the weapon, the health
        // bars and the other corners' pieces) is drawn with RT64's extended rectangles, with an origin at the left or right edge, so RT64
        // places it from the window's edge (with Original that edge is the 4:3 frame's, where the view puts it now). Their coordinates for
        // the right edge are counted from the frame's right side (RT64 adds the colour image's width, D_800BE620's).
        const bool expand_hud = ultramodern::renderer::get_graphics_config().hr_option == ultramodern::renderer::HUDRatioMode::Full;
        int32_t frame_width = (int32_t)MEM_W(0, (gpr)(int32_t)0x800BE620);
        if (frame_width < 200 || frame_width > 640) {
            frame_width = 292;
        }
        uint32_t extended_on_in = 0xFFFFFFFFu;   // the range RT64's extended GBI was enabled in (at a sync before the first rectangle)
        uint32_t skip_to = 0;                    // the words of an extended rectangle written over the game's, not looked at again
        int edge_rects = 0;
        for (const Range& range : ranges)
        for (uint32_t address = range.from; address < range.to; address += 8) {
            if (address < skip_to) {
                continue;
            }
            const bool nested = range.nested;
            const uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)address);
            const uint32_t w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)address);
            const uint32_t op = w0 >> 24;
            const uint32_t chain_op = nested ? 0u : op;
            if (dump_hud && dumped < 2500 && (nested || inside(any_hud_segments, address))) {
                char line[80];
                std::snprintf(line, sizeof(line), "[net view] cmd %08X: %08X %08X", address, w0, w1);
                conker::netsession::log(line);
                dumped++;
            }
            if ((mask & 1u) != 0 && chain_op == 0xDC && (w0 & 0x00FFFFFFu) == 0x080008u && ((w1 >= own + field_viewports && w1 < own + field_viewports + 0x40) ||
                    (w1 >= full + field_viewports && w1 < full + field_viewports + 0x40))) {
                const bool own_viewport = w1 < own + field_viewports + 0x40 && w1 >= own + field_viewports;
                const uint32_t target = own_viewport ? w1 - own + full : w1;
                if (inside(hud_segments, address) && (hud_shift != 0 || hud_shift_x != 0)) {
                    MEM_W(4, (gpr)(int32_t)address) = (int32_t)shifted_viewport_copy(rdram, target, hud_shift, hud_shift_x);
                } else if (own_viewport) {
                    MEM_W(4, (gpr)(int32_t)address) = (int32_t)target;
                }
                redirected++;
            } else if ((mask & 1u) != 0 && !keep_old_hidden && chain_op == 0xDC && (w0 & 0x00FFFFFFu) == 0x080008u && other_viewport(w1)) {
                // another player's camera pass (its scissor is emptied below): its viewport collapsed too, so that it can't
                // draw over this machine's view whatever the renderer makes of an empty scissor
                MEM_W(4, (gpr)(int32_t)address) = (int32_t)collapsed_viewport_copy(rdram, w1);
                redirected++;
            } else if ((mask & 2u) != 0 && chain_op == 0xED) {
                for (int k = 0; k < players; k++) {
                    if (w0 == scissor0[k] && w1 == scissor1[k]) {
                        if (k == local) {
                            MEM_W(0, (gpr)(int32_t)address) = (int32_t)scissor_word0(left, top);
                            MEM_W(4, (gpr)(int32_t)address) = (int32_t)scissor_word1(right, bottom);
                            shown++;
                        } else {
                            MEM_W(0, (gpr)(int32_t)address) = (int32_t)0xED000000u;
                            MEM_W(4, (gpr)(int32_t)address) = 0;
                            hidden++;
                        }
                        break;
                    }
                }
            } else if ((mask & 4u) != 0 && chain_op == 0xF6) {
                // a fill of a camera's own rectangle (a tint over its view): this machine's camera's, over the full view
                if (w0 == (0xF6000000u | (scissor1[local] & 0xFFFFFFu)) && w1 == (scissor0[local] & 0xFFFFFFu)) {
                    static const bool tint_4_3 = SDL_getenv("CONKER_NET_TINT43") != nullptr;
                    if (tint_4_3) {
                        MEM_W(0, (gpr)(int32_t)address) = (int32_t)(0xF6000000u | (scissor_word1(right, bottom) & 0xFFFFFFu));
                        MEM_W(4, (gpr)(int32_t)address) = (int32_t)(scissor_word0(left, top) & 0xFFFFFFu);
                    }
                    else {
                        // the full view's height, and past both sides (the scissor stops it at the window's): the fade after the
                        // death wipe stopped at the 4:3 frame's sides, two lines with the view bright beside them
                        const uint32_t tint = 0x80000000u | (tint_start + (tint_next++ % tint_slots) * tint_slot);
                        const int32_t ulx = ((int32_t)left - 1024) * 4, uly = (int32_t)top * 4, lrx = ((int32_t)right + 1024) * 4, lry = (int32_t)bottom * 4;
                        MEM_W(0, (gpr)(int32_t)tint) = (int32_t)(0xE0000000u | 0x525464u);               // RT64's hook: enable its extended GBI
                        MEM_W(4, (gpr)(int32_t)tint) = (int32_t)((0x1u << 28) | 0x64u);
                        MEM_W(8, (gpr)(int32_t)tint) = (int32_t)((0x64u << 24) | 0x000003u);             // G_EX_FILLRECT_V1, no origin
                        MEM_W(12, (gpr)(int32_t)tint) = (int32_t)(0x800u | (0x800u << 12));
                        MEM_W(16, (gpr)(int32_t)tint) = (int32_t)(((uint32_t)ulx << 16) | ((uint32_t)uly & 0xFFFFu));
                        MEM_W(20, (gpr)(int32_t)tint) = (int32_t)(((uint32_t)lrx << 16) | ((uint32_t)lry & 0xFFFFu));
                        MEM_W(24, (gpr)(int32_t)tint) = (int32_t)0xDF000000u;                            // end of the list
                        MEM_W(28, (gpr)(int32_t)tint) = 0;
                        MEM_W(0, (gpr)(int32_t)address) = (int32_t)0xDE000000u;                          // the fill: a call to it
                        MEM_W(4, (gpr)(int32_t)address) = (int32_t)(tint & 0x00FFFFFFu);
                    }
                    fills++;
                }
            } else if ((mask & 8u) != 0 && chain_op == 0xDA && (w0 & 0x04u) != 0 && (scale_x != 1.0f || scale_y != 1.0f) && w1 >= own && w1 < own + camera_size) {
                // Every load of this machine's camera's projection, in whatever pass: the game draws its characters in passes of
                // their own after the cameras' (marked for no camera), with the same projection and the same viewport, and a
                // viewport made full-screen needs the projection widened with it or what is drawn there doesn't fit the rest.
                // the projection matrix in this machine's camera's record, loaded by its 3D pass: a copy, widened
                MEM_W(4, (gpr)(int32_t)address) = (int32_t)scaled_matrix_copy(rdram, w1, scale_x, scale_y);
                matrices++;
            }
            // The HUD's rectangles (icons, digits, bars, the aiming reticle's brackets) are placed in the game's 292 x 216
            // screen, for the split-screen halves. Moved here, by where each one is:
            //  - around the middle of this machine's camera's own rectangle: the reticle, which the game aims through,
            //    moved to the middle of the full view with the 3D view;
            //  - in the band at the screen's middle row (the timer and the scores, which sit between the halves): up
            //    to the top corner on the right;
            //  - inside this machine's camera's rectangle: at its corners, the corners of the full view;
            //  - inside another camera's rectangle: gone (another player's HUD).
            if (hud_layout && (op == 0xE4 || op == 0xE5 || op == 0xF6) && (nested || inside(any_hud_segments, address) || inside(bar_segments, address)) &&
                    !(op == 0xF6 && w0 == (0xF6000000u | (scissor1[local] & 0xFFFFFFu)) && w1 == (scissor0[local] & 0xFFFFFFu))) {
                const int32_t yl = (int32_t)(w0 & 0xFFFu), yh = (int32_t)(w1 & 0xFFFu);
                const int32_t xl = (int32_t)((w0 >> 12) & 0xFFFu), xh = (int32_t)((w1 >> 12) & 0xFFFu);
                const float cx = (float)(xl + xh) * 0.125f, cy = (float)(yl + yh) * 0.125f;
                int32_t dx = 0, dy = 0;
                bool hide = false;
                const char* kind_name = "left";
                const float own_l = get_float(rdram, own, field_left), own_r = get_float(rdram, own, field_right);
                const float own_t = get_float(rdram, own, field_top), own_b = get_float(rdram, own, field_bottom);
                const auto in_rect = [&](float l, float t, float r, float b) { return cx >= l && cx <= r && cy >= t && cy <= b; };
                // A health bar (what is drawn after the 3D passes, apart from the HUD's passes): a small rectangle near a
                // camera's left edge; anything else there (the pause screen's pictures, in strips) is left alone.
                const bool bar = !nested && !inside(any_hud_segments, address);
                const float width = (float)std::abs(xl - xh) * 0.25f, height = (float)std::abs(yl - yh) * 0.25f;
                bool near_left = false;
                for (int k = 0; k < players; k++) {
                    const float l = get_float(rdram, base + k * camera_size, field_left);
                    near_left = near_left || (cx >= l && cx < l + 40.0f);
                }
                // The radar (a grey circle, 32 x 32 in the game's screen, and its dot) at each camera's top right: with two
                // players only this machine's player's is shown, at the full view's top right.
                int radar_camera = -1;
                if (bar && players >= 2 && width <= 34.5f && height <= 34.5f) {
                    for (int k = 0; k < players; k++) {
                        const uint32_t record = base + k * camera_size;
                        const float r_right = get_float(rdram, record, field_right), r_top = get_float(rdram, record, field_top);
                        if (cx >= r_right - 72.0f && cx <= r_right && (cy >= r_top && cy <= r_top + 62.0f)) {
                            radar_camera = k;
                        }
                    }
                }
                const bool leave = bar && radar_camera < 0 && (op == 0xF6 || !(width <= 16.5f && height <= 5.5f && near_left));
                if (menu_open) {
                    if (bar && (radar_camera >= 0 || !leave)) {
                        hide = true;
                        kind_name = "menu: HUD hidden";
                    } else {
                        kind_name = "menu: left alone";
                    }
                } else if (radar_camera >= 0) {
                    if (radar_camera == local) {
                        dx = (int32_t)((right - get_float(rdram, own, field_right)) * 4.0f);
                        dy = (int32_t)((top - own_t) * 4.0f);
                        kind_name = "radar or weapon";
                    } else {
                        hide = true;
                        kind_name = "other radar or weapon";
                    }
                } else if (leave) {
                    kind_name = "left alone";
                } else if (!bar && std::fabs(cy - (own_t + own_b) * 0.5f) < 40.0f && std::fabs(cx - (own_l + own_r) * 0.5f) < 48.0f && in_rect(own_l, own_t, own_r, own_b)) {
                    dx = hud_shift_x;
                    dy = hud_shift;
                    kind_name = "aim";
                } else if (!bar && cy >= 0.5f * (top + bottom) - 10.0f && cy <= 0.5f * (top + bottom) + 8.0f) {
                    // the shared row (the timer, and with two players each one's score, an icon and its digits): the
                    // game's own one-screen layout puts it along the bottom, the scores either side of the timer. The
                    // left score is player 2's, the right one player 1's; the second machine's player gets theirs on
                    // the right as well, so each sees its own team at the right (CONKER_NET_SCORESWAP=0 keeps them
                    // where they are).
                    dy = (int32_t)((top + 0.946f * (bottom - top) - 0.5f * (top + bottom)) * 4.0f);
                    dx = 0;
                    kind_name = "shared";
                    const float middle = 0.5f * (left + right);
                    if (players == 2 && local == 1 && score_swap && (cx < middle - 36.0f || cx > middle + 36.0f)) {
                        dx = (int32_t)((cx < middle ? 120.0f : -120.0f) * 4.0f);
                        kind_name = "score";
                    }
                } else if (!bar && cx >= own_l + 30.0f && cx <= own_r - 30.0f && cy >= own_t + 8.0f && cy <= own_b - 8.0f) {
                    // inside the camera's rectangle and away from its edges (the rocket's explosion): placed by the game in the rectangle's
                    // pixels, at the view's middle with the aim, so it moves down with the aim and not out to a corner
                    dx = hud_shift_x;
                    dy = hud_shift;
                    kind_name = "world sprite";
                    static int world_logged = 0;
                    if (world_logged < 40) {
                        char line[140];
                        std::snprintf(line, sizeof(line), "[net view] world sprite %08X: %08X %08X (centre %g,%g)", address, w0, w1, cx, cy);
                        conker::netsession::log(line);
                        world_logged++;
                    }
                } else if (in_rect(own_l, own_t, own_r, own_b)) {
                    dx = (int32_t)(((cx < (own_l + own_r) * 0.5f) ? (left - own_l) : (right - own_r)) * 4.0f);
                    dy = (int32_t)(((cy < (own_t + own_b) * 0.5f) ? (top - own_t) : (bottom - own_b)) * 4.0f);
                    kind_name = "own";
                } else {
                    for (int k = 0; k < players && !hide; k++) {
                        if (k == local) {
                            continue;
                        }
                        const uint32_t record = base + k * camera_size;
                        if (in_rect(get_float(rdram, record, field_left), get_float(rdram, record, field_top), get_float(rdram, record, field_right), get_float(rdram, record, field_bottom))) {
                            hide = true;
                            kind_name = "other";
                        }
                    }
                }
                if (log_it && rects_logged < 60) {
                    char line[140];
                    std::snprintf(line, sizeof(line), "[net view]   HUD rect %08X: %08X %08X (centre %g,%g) %s", address, w0, w1, cx, cy, kind_name);
                    conker::netsession::log(line);
                    rects_logged++;
                }
                if (hide) {
                    // nothing left of it: as wide and as high as a point
                    MEM_W(0, (gpr)(int32_t)address) = (int32_t)((w0 & ~0xFFFFFFu) | (uint32_t)(((uint32_t)xh & 0xFFFu) << 12) | ((uint32_t)yh & 0xFFFu));
                    rects_hidden++;
                } else if (dx != 0 || dy != 0) {
                    const int32_t a0 = xl + dx, a1 = xh + dx, b0 = yl + dy, b1 = yh + dy;
                    // Expand: the side it goes to (1 left, 2 right)
                    int edge = 0;
                    if (expand_hud && !menu_open) {
                        if (std::strcmp(kind_name, "radar or weapon") == 0) {
                            edge = 2;
                        } else if (std::strcmp(kind_name, "own") == 0) {
                            edge = cx < (own_l + own_r) * 0.5f ? 1 : 2;
                        }
                    }
                    bool extended = false;
                    if (edge != 0) {
                        // RT64's extended GBI on, at a sync before it in this list (RT64 needs no syncs), if not already in this list
                        if (extended_on_in != range.from) {
                            for (uint32_t back = address; back > range.from && address - back < 64u * 8u;) {
                                back -= 8;
                                const uint32_t b0w = (uint32_t)MEM_W(0, (gpr)(int32_t)back), b1w = (uint32_t)MEM_W(4, (gpr)(int32_t)back);
                                if ((b0w == 0xE7000000u || b0w == 0xE6000000u || b0w == 0xE8000000u) && b1w == 0) {
                                    MEM_W(0, (gpr)(int32_t)back) = (int32_t)(0xE0000000u | 0x525464u);
                                    MEM_W(4, (gpr)(int32_t)back) = (int32_t)((0x1u << 28) | 0x64u);
                                    extended_on_in = range.from;
                                    break;
                                }
                            }
                        }
                        const uint32_t origin = edge == 2 ? 0x400u : 0x000u;   // G_EX_ORIGIN_RIGHT / _LEFT
                        const int32_t off = edge == 2 ? frame_width * 4 : 0;
                        const int32_t ulx = std::max(a1, 0) - off, uly = std::max(b1, 0), lrx = std::min(a0, 0xFFF) - off, lry = std::min(b0, 0xFFF);
                        if (extended_on_in == range.from && (op == 0xE4 || op == 0xE5) && address + 16 < range.to &&
                                ((uint32_t)MEM_W(8, (gpr)(int32_t)address) >> 24) == 0xE1 && ((uint32_t)MEM_W(16, (gpr)(int32_t)address) >> 24) == 0xF1) {
                            // G_EX_TEXRECT_V1 in the three words of the game's G_TEXRECT and its two G_RDPHALFs
                            const uint32_t st = (uint32_t)MEM_W(12, (gpr)(int32_t)address), steps = (uint32_t)MEM_W(20, (gpr)(int32_t)address);
                            const uint32_t tile = (w1 >> 24) & 0x7u;
                            MEM_W(0, (gpr)(int32_t)address) = (int32_t)((0x64u << 24) | 0x000002u);
                            MEM_W(4, (gpr)(int32_t)address) = (int32_t)(tile | (origin << 3) | (origin << 15) | (op == 0xE5 ? 0x80u : 0u));
                            MEM_W(8, (gpr)(int32_t)address) = (int32_t)(((uint32_t)ulx << 16) | ((uint32_t)uly & 0xFFFFu));
                            MEM_W(12, (gpr)(int32_t)address) = (int32_t)(((uint32_t)lrx << 16) | ((uint32_t)lry & 0xFFFFu));
                            MEM_W(16, (gpr)(int32_t)address) = (int32_t)st;
                            MEM_W(20, (gpr)(int32_t)address) = (int32_t)steps;
                            skip_to = address + 24;
                            extended = true;
                        } else if (extended_on_in == range.from && op == 0xF6) {
                            // G_EX_FILLRECT_V1, from a list of ours the fill's word calls
                            const uint32_t fill = 0x80000000u | (edge_fill_start + (edge_fill_next++ % edge_fill_slots) * edge_fill_slot);
                            MEM_W(0, (gpr)(int32_t)fill) = (int32_t)((0x64u << 24) | 0x000003u);
                            MEM_W(4, (gpr)(int32_t)fill) = (int32_t)(origin | (origin << 12));
                            MEM_W(8, (gpr)(int32_t)fill) = (int32_t)(((uint32_t)ulx << 16) | ((uint32_t)uly & 0xFFFFu));
                            MEM_W(12, (gpr)(int32_t)fill) = (int32_t)(((uint32_t)lrx << 16) | ((uint32_t)lry & 0xFFFFu));
                            MEM_W(16, (gpr)(int32_t)fill) = (int32_t)0xDF000000u;
                            MEM_W(20, (gpr)(int32_t)fill) = 0;
                            MEM_W(0, (gpr)(int32_t)address) = (int32_t)0xDE000000u;
                            MEM_W(4, (gpr)(int32_t)address) = (int32_t)(fill & 0x00FFFFFFu);
                            extended = true;
                        }
                        if (extended) {
                            rects_moved++;
                            edge_rects++;
                        }
                    }
                    if (!extended && a1 >= 0 && b1 >= 0 && a0 <= 0xFFF && b0 <= 0xFFF) {
                        MEM_W(0, (gpr)(int32_t)address) = (int32_t)((w0 & 0xFF000000u) | ((uint32_t)std::min(a0, 0xFFF) << 12) | (uint32_t)std::min(b0, 0xFFF));
                        MEM_W(4, (gpr)(int32_t)address) = (int32_t)((w1 & 0xFF000000u) | ((uint32_t)std::max(a1, 0) << 12) | (uint32_t)std::max(b1, 0));
                        rects_moved++;
                    }
                }
            }
            if (remote_sniper && op == 0x64 && (w0 & 0x00FFFFFFu) == 0x000002u && address + 16 < range.to) {
                static int sprite_lines = 0;
                if (sprite_lines < 400) {
                    // which pass of the frame the sprite is in, and where it is (10.2 fixed point): to find out what moves the other player's laser star
                    int pass = -1;
                    for (size_t i = 0; i < frame.size(); i++) {
                        if (frame[i].gfx <= address) {
                            pass = (int)i;
                        }
                    }
                    const uint32_t c0 = (uint32_t)MEM_W(8, (gpr)(int32_t)address), c1 = (uint32_t)MEM_W(12, (gpr)(int32_t)address);
                    char line[200];
                    std::snprintf(line, sizeof(line), "[net view] remote sniper: sprite %08X in pass %d (camera %d, kind %d, local %d): w1 %08X, corners x %d..%d y %d..%d (quarter pixels), local look %02X",
                        address, pass, pass >= 0 ? frame[pass].camera : -1, pass >= 0 ? frame[pass].kind : -1, local, (uint32_t)w1,
                        (int)(int16_t)(c0 >> 16), (int)(int16_t)(c1 >> 16), (int)(int16_t)(c0 & 0xFFFF), (int)(int16_t)(c1 & 0xFFFF), local_kind);
                    conker::netsession::log(line);
                    sprite_lines++;
                }
            }
            // (every one of them, whatever this machine's look: another player's rocket, its missile and its explosion, are placed by the game
            // in this camera's rectangle as well, and were left at the split screen's place; CONKER_NET_SPRITES=look moves them only in the
            // looks above, as 0.9.7 did)
            static const bool sprites_only_in_looks = [] { const char* v = std::getenv("CONKER_NET_SPRITES"); return v != nullptr && std::strcmp(v, "look") == 0; }();
            if ((rocket_look || sprite_look || remote_sniper || !sprites_only_in_looks) && hud_layout && !menu_open && !nested && op == 0x64 && (w0 & 0x00FFFFFFu) == 0x000002u && (hud_shift != 0 || hud_shift_x != 0) &&
                    inside(local_world_passes, address) && address + 16 < range.to) {
                // The game's screen-space sprites (func_15130A9C: the rocket's explosion, sparkles, bees) are emitted as RT64's extended texture
                // rectangle (widescreen.cpp): this word, then the corners (x, y, x, y: 10.2 fixed point, 16 bits each), then the texture words. The
                // game places them in its camera's own rectangle, so with the full view they are moved down with the world, as the aim is.
                const uint32_t c0 = (uint32_t)MEM_W(8, (gpr)(int32_t)address), c1 = (uint32_t)MEM_W(12, (gpr)(int32_t)address);
                const int32_t uly = (int32_t)(int16_t)(c0 & 0xFFFFu), lry = (int32_t)(int16_t)(c1 & 0xFFFFu);
                const int32_t nuly = uly + hud_shift, nlry = lry + hud_shift;
                const int32_t nulx = (int32_t)(int16_t)(c0 >> 16) + hud_shift_x, nlrx = (int32_t)(int16_t)(c1 >> 16) + hud_shift_x;
                if (nuly >= -0x8000 && nuly <= 0x7FFF && nlry >= -0x8000 && nlry <= 0x7FFF && nulx >= -0x8000 && nulx <= 0x7FFF && nlrx >= -0x8000 && nlrx <= 0x7FFF) {
                    MEM_W(8, (gpr)(int32_t)address) = (int32_t)(((uint32_t)nulx << 16) | ((uint32_t)nuly & 0xFFFFu));
                    MEM_W(12, (gpr)(int32_t)address) = (int32_t)(((uint32_t)nlrx << 16) | ((uint32_t)nlry & 0xFFFFu));
                }
            }
            if (rocket_look && (op == 0xE4 || op == 0xE5) &&
                    !(op == 0xF6 && w0 == (0xF6000000u | (scissor1[local] & 0xFFFFFFu)) && w1 == (scissor0[local] & 0xFFFFFFu))) {
                // a rectangle half as wide and as high as this machine's camera, at least: the overlay (the game's own rectangles
                // for it are of the camera's size; HUD icons, bars and sprites are far smaller). Collapsed to a point.
                const float o_l = get_float(rdram, own, field_left), o_r = get_float(rdram, own, field_right);
                const float o_t = get_float(rdram, own, field_top), o_b = get_float(rdram, own, field_bottom);
                const int32_t r_yl = (int32_t)(w0 & 0xFFFu), r_yh = (int32_t)(w1 & 0xFFFu);
                const int32_t r_xl = (int32_t)((w0 >> 12) & 0xFFFu), r_xh = (int32_t)((w1 >> 12) & 0xFFFu);
                const float r_w = (float)std::abs(r_xl - r_xh) * 0.25f, r_h = (float)std::abs(r_yl - r_yh) * 0.25f;
                if (r_w >= 0.5f * (o_r - o_l) && r_h >= 0.5f * (o_b - o_t)) {
                    MEM_W(0, (gpr)(int32_t)address) = (int32_t)((w0 & ~0xFFFFFFu) | (uint32_t)(((uint32_t)r_xh & 0xFFFu) << 12) | ((uint32_t)r_yh & 0xFFFu));
                }
            }
            if (log_it && logged < 160 && (op == 0xDC || op == 0xED || op == 0xDA || op == 0xDE || op == 0xE0)) {
                char line[120];
                std::snprintf(line, sizeof(line), "[net view]   %08X: %08X %08X", address, w0, w1);
                conker::netsession::log(line);
                logged++;
            }
        }
        if (log_it) {
            char line[320];
            std::snprintf(line, sizeof(line), "[net view] frame list %08X..%08X: %d viewports redirected, %d scissors shown, %d hidden, %d projection matrices (shape %g), %d fills, HUD shift %d, %d HUD rectangles moved (%d of them to a screen edge), %d hidden, %zu passes",
                start, end, redirected, shown, hidden, matrices, shape, fills, hud_shift, rects_moved, edge_rects, rects_hidden, frame.size());
            conker::netsession::log(line);
        }
    }
}

bool conker::netview::full_view_bounds(uint8_t* rdram, int camera, float& left, float& top, float& right, float& bottom) {
    if (!view_is_full()) {
        return false;
    }
    const int players = players_now(rdram);
    const int local = conker::netsession::local_slot();
    if (players <= 1 || players > max_cameras || local < 0 || local >= players || camera != local) {
        return false;
    }
    const uint32_t base = (uint32_t)MEM_W(0, cameras_pointer);
    if ((base >> 24) != 0x80 || (base & 0x7FFFFFu) + (players + 1) * camera_size > 0x800000u) {
        return false;
    }
    const uint32_t full = base + (uint32_t)players * camera_size;
    left = get_float(rdram, full, field_left);
    top = get_float(rdram, full, field_top);
    right = get_float(rdram, full, field_right);
    bottom = get_float(rdram, full, field_bottom);
    return right - left > 100.0f && bottom - top > 100.0f;
}

// Nothing of the game's memory is changed for the view, so a snapshot can be taken or loaded at any time.
bool conker::netview::busy() {
    return false;
}

void conker::netview::port_read() {
}

void conker::netview::frame_begin(uint8_t* rdram) {
    (void)rdram;
    if (conker::netsession::active()) {
        conker::netsession::watch_poll("dlb", 0);
    }
    passes.clear();
}

void conker::netview::frame_end(uint8_t* rdram, uint32_t list_end) {
    if (conker::netsession::active()) {
        conker::netsession::watch_poll("dle", 0);
    }
    std::vector<Pass> frame;
    frame.swap(passes);
    if (!view_is_full() || frame.empty()) {
        return;
    }
    const int players = players_now(rdram);
    // CONKER_NET_VIEWSLOT=n (a test): every machine shows player n's view, whoever it plays, so that the two machines
    // rewrite their display lists the same way.
    static const int forced_slot = []() { const char* text = SDL_getenv("CONKER_NET_VIEWSLOT"); return text != nullptr ? std::atoi(text) : -1; }();
    const int local = forced_slot >= 0 ? forced_slot : conker::netsession::local_slot();
    if (players <= 1 || players > max_cameras || local < 0 || local >= players) {
        return;
    }
    const uint32_t base = (uint32_t)MEM_W(0, cameras_pointer);
    if ((base >> 24) != 0x80 || (base & 0x7FFFFFu) + (players + 1) * camera_size > 0x800000u) {
        return;
    }
    if (dumps_done == 0) {
        dumps_done = 1;
        dump_cameras(rdram, base, players);
    }
    // (frames' lists are logged a few times: the first three, and then one in 2000)
    const bool log_it = frames_rewritten < 3 || frames_rewritten % 2000 == 1000;
    uint32_t start = 0xFFFFFFFFu;
    for (const Pass& pass : frame) {
        start = std::min(start, pass.gfx);
    }
    static const bool keep_rewrite = SDL_getenv("CONKER_NET_KEEPREWRITE") != nullptr;
    const bool can_journal = !keep_rewrite && (start >> 24) == 0x80 && (list_end >> 24) == 0x80 && list_end > start &&
        list_end - start <= 0x100000u && (list_end & 0x7FFFFFu) <= 0x800000u;
    std::vector<uint8_t> before;
    if (can_journal) {
        before.assign(rdram + (start & 0x7FFFFFu), rdram + (list_end & 0x7FFFFFu));
    }
    rewrite_frame(rdram, frame, list_end, base, players, local, log_it);
    if (can_journal) {
        Journal journal;
        journal.start = start;
        const uint8_t* now = rdram + (start & 0x7FFFFFu);
        for (size_t off = 0; off + 4 <= before.size(); off += 4) {
            uint32_t a, b;
            std::memcpy(&a, &before[off], 4);
            std::memcpy(&b, now + off, 4);
            if (a != b) {
                journal.words.push_back({ start + (uint32_t)off, a, b });
            }
        }
        std::lock_guard<std::mutex> lock(journal_mutex);
        journals.push_back(std::move(journal));
        while (journals.size() > 4) {
            journals.pop_front();
        }
    }
    frames_rewritten++;
}

void conker::netview::render_done(uint8_t* rdram, uint32_t data_ptr) {
    std::lock_guard<std::mutex> lock(journal_mutex);
    if (journals.empty()) {
        return;
    }
    const Journal& journal = journals.front();
    if (journal.start < data_ptr || journal.start - data_ptr >= 0x100000u) {
        return;
    }
    for (const auto& word : journal.words) {
        uint32_t current;
        std::memcpy(&current, rdram + (word[0] & 0x7FFFFFu), 4);
        if (current == word[2]) {
            std::memcpy(rdram + (word[0] & 0x7FFFFFu), &word[1], 4);
        }
    }
    journals.pop_front();
}

// At the entry of func_15019464 (kind 1), func_15019BB8 (kind 2) and func_1501A490 (kind 0): where the frame's
// display list has the setup of a camera's pass. Nothing of the game's is changed here.
extern "C" void conker_net_view_pass(uint8_t* rdram, int gfx, int camera, int kind) {
    (void)rdram;
    if (!view_is_full()) {
        return;
    }
    // (a pass calls func_1501A490 for its setup: the pass is already listed, and the setup comes just after its start)
    if (kind == 0 && !passes.empty() && passes.back().gfx <= (uint32_t)gfx && (uint32_t)gfx < passes.back().gfx + 0x40) {
        return;
    }
    passes.push_back({ (uint32_t)gfx, camera, kind });
}


// Online, the players are drawn for every camera (hooks in conker.toml, in func_1502C974, the game's drawing of one
// object for one camera). The game hides an object from a camera in two ways, and hid the caveman from the raptor's
// camera far away (while the raptor, a bigger model, was never hidden from the caveman's): a byte at +0x74 with a bit
// for each camera it is hidden from, and a fade, func_1506196C: the object's byte for the camera (+0xB + camera, moved
// 50 a frame towards 255 or 0 by the game's own visibility test) times the object's opacity (+7), over 256, and nothing
// is drawn at 0. Both are overridden here for the players, in registers only: nothing in the game's memory changes, so
// every machine plays on the same. A player's own camera is left to the game, which hides the body in the first person
// (crossbow, knife). CONKER_NET_KEEPCULL puts the game's own choice back.
//   which 0: after the +0x74 byte is loaded ($t7); 1 and 3: after func_1506196C's answer ($v0), the first check and
//   the one for the opacity.
extern "C" void conker_player_visibility(uint8_t* rdram, recomp_context* ctx, int which) {
    const uint32_t object = (uint32_t)MEM_W(0x5C, ctx->r29);
    const uint32_t camera = (uint32_t)MEM_W(0x60, ctx->r29);
    static const bool keep_cull = std::getenv("CONKER_NET_KEEPCULL") != nullptr;
    if (keep_cull || object >= 4 || camera >= 4 || object == camera || !conker::netsession::active()) {
        return;
    }
    if (which == 0) {
        ctx->r15 = ctx->r15 & ~((gpr)1 << camera);
    } else {
        const uint32_t obj = (uint32_t)MEM_W(0x2C, ctx->r29);
        const uint32_t product = 0xFFu * (uint32_t)MEM_BU(7, (gpr)(int32_t)obj);
        ctx->r2 = (gpr)(int64_t)(int32_t)(product != 0xFE01u ? (product >> 8) : 0xFFu);
    }
}
