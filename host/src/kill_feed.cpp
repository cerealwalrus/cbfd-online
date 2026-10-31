// (0.11.4) The kill feed: a line in the top-right corner for each death in a multiplayer match, "Beebo killed Mark", each name in its
// player's colour (P1 red, P2 blue, P3 green, P4 yellow, P5 purple; computer players grey, "Bot N" by their character's number) with a
// small badge of the player's number before it. General tab: Kill Feed, Off, Text (the lines alone, no box behind them) or Boxed (a dark
// box behind each line, as the chat's).
//
// Only read from the game's memory, once a frame on the game thread (game_frame, from conker_mp_characters), and shown on the UI thread
// (update): nothing of the game's is changed and nothing travels online, so it can't desync or add any delay.
//
// Deaths: each character's +0x31C (its weapons' and life's state) +0x120, 0 while alive (func_1507C8FC, the deaths frame by frame,
// goes over the characters by it): a character whose +0x120 leaves 0 has died.
// Who killed whom (0.11.5): the game's own kill counts and last hitters, watched every frame (see watch_kills below).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "recomp.h"
#include "recompui/config.h"
#include "recompui/recompui.h"
#include "elements/ui_label.h"
#include "elements/ui_image.h"
#include "ultramodern/ultramodern.hpp"

#include "conker.hpp"
#include "log_files.hpp"

namespace {
    using clock = std::chrono::steady_clock;
    constexpr int max_chars = 16;
    constexpr uint32_t characters = 0x800CC2D0u, character_size = 0x32Cu;
    constexpr uint32_t state_size = 0x200u;   // (of +0x31C, looked at)
    constexpr int look_after = 15;            // frames after a death its candidates are looked at

    // ---- the game thread's side ----
    std::FILE* log_file = nullptr;
    void log(const char* line) {
        if (log_file == nullptr) {
            log_file = std::fopen(conker::logs::path("killfeed.log").string().c_str(), "w");
            if (log_file == nullptr) {
                return;
            }
        }
        std::fprintf(log_file, "%s\n", line);
        std::fflush(log_file);
    }

    // (0.11.5) Who killed whom, from the game's own code. func_151E5FF4 (a hit: $a0 who hit, or a thrown or shot thing whose +0x124 is
    // its owner + 1; $a1 the one hit) writes D_800E0BB8[the one hit] = the hitter | 0x80, and when the hit leaves the one hit with no
    // health (+0x1CA 0) it gives the hitter a kill: gGameState (0x8008FDD4's pointer) +0x46 + 2 x the hitter, a halfword (or, on the
    // same team, a team kill: +0x70 + 12 x the hitter). The character's +0x120 only leaves 0 later, as it falls down (the old reading of
    // the stats at that moment missed the kill, made a second or more before). So the kills are watched every frame, and a death is the
    // last hitter's when that hitter was given a kill not long before it (or just after); otherwise (a fall, a mine of its own...) it's
    // the victim's own.
    constexpr uint32_t last_hitter = 0x800E0BB8u;
    constexpr uint32_t window = 30 * 20;      // frames back from a death that a kill given to its last hitter still counts
    constexpr uint32_t near_window = 30 * 4;  // frames back from a death that any other character's kill counts for it
    struct Credit { int killer; uint32_t frame; };
    std::vector<Credit> credits;
    std::array<int, max_chars> kills_last{};
    std::array<int, 4> team_kills_last{};
    bool have_kills = false;
    std::array<uint8_t, max_chars> last_life{};
    bool have_last = false;
    uint32_t frames = 0;
    struct Pending { int victim; uint32_t frame; uint8_t hitter; };
    std::vector<Pending> pending;

    uint32_t game_state(uint8_t* rdram) {
        const uint32_t gs = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8008FDD4u);
        if ((gs >> 24) != 0x80 || (gs & 0x00FFFFFFu) >= 0x007FF000u) {
            return 0;
        }
        return gs;
    }

    struct Event { int killer; int victim; };
    std::mutex events_mutex;
    std::deque<Event> events;

    int count_now(uint8_t* rdram) {
        const int n = (int)MEM_B(0, (gpr)(int32_t)0x8008FD8Cu);   // (func_1507C8FC's count of characters)
        return std::clamp(n, 0, max_chars);
    }

    void push_event(int killer, int victim) {
        std::lock_guard lock{ events_mutex };
        events.push_back({ killer, victim });
        while (events.size() > 32) {
            events.pop_front();
        }
    }

    // the kills given since the last frame, each a credit
    void watch_kills(uint8_t* rdram, int count) {
        const uint32_t gs = game_state(rdram);
        if (gs == 0) {
            have_kills = false;
            return;
        }
        std::array<int, max_chars> kills{};
        std::array<int, 4> team_kills{};
        for (int k = 0; k < max_chars; k++) {
            kills[(size_t)k] = (int)(int16_t)MEM_H(0x46 + 2 * k, (gpr)(int32_t)gs);
        }
        for (int k = 0; k < 4; k++) {
            team_kills[(size_t)k] = (int)(int16_t)MEM_H(0x70 + 12 * k, (gpr)(int32_t)gs);
        }
        if (have_kills) {
            for (int k = 0; k < count; k++) {
                const int more = kills[(size_t)k] - kills_last[(size_t)k] + (k < 4 ? team_kills[(size_t)k] - team_kills_last[(size_t)k] : 0);
                for (int i = 0; i < more && i < 4; i++) {
                    credits.push_back({ k, frames });
                    char line[80];
                    std::snprintf(line, sizeof(line), "[kill feed] f=%u character %d given a kill (now %d)", frames, k, kills[(size_t)k]);
                    log(line);
                }
            }
        }
        kills_last = kills;
        team_kills_last = team_kills;
        have_kills = true;
        // (old ones dropped)
        credits.erase(std::remove_if(credits.begin(), credits.end(), [](const Credit& c) { return frames > c.frame + window * 2; }), credits.end());
    }

    void judge(const Pending& p) {
        int killer = p.victim;
        const char* why = "no kill given to its last hitter: its own";
        if (p.hitter & 0x80) {
            const int h = p.hitter & 0x7F;
            if (h != p.victim) {
                for (size_t i = 0; i < credits.size(); i++) {
                    const Credit& c = credits[i];
                    if (c.killer == h && c.frame + window >= p.frame && c.frame <= p.frame + look_after) {
                        killer = h;
                        why = "its last hitter, given a kill";
                        credits.erase(credits.begin() + (std::ptrdiff_t)i);
                        break;
                    }
                }
            }
        }
        // (0.11.5, the 31 Oct log: D_800E0BB8 held 0xF6 and 0xAD whoever killed, so not the hitter as read here; but every kill was given
        // 0 to 76 frames before the death it was for. So, failing the hitter: the latest kill given to another character in the 4 seconds
        // before the death (or just after it), each kill used once)
        if (killer == p.victim) {
            int best = -1;
            for (size_t i = 0; i < credits.size(); i++) {
                const Credit& c = credits[i];
                if (c.killer != p.victim && c.frame + near_window >= p.frame && c.frame <= p.frame + look_after &&
                    (best < 0 || c.frame > credits[(size_t)best].frame)) {
                    best = (int)i;
                }
            }
            if (best >= 0) {
                killer = credits[(size_t)best].killer;
                why = "the kill given just before it";
                credits.erase(credits.begin() + best);
            }
        }
        char line[160];
        std::snprintf(line, sizeof(line), "[kill feed] f=%u character %d died; last hitter %02X; killer %d (%s)", p.frame, p.victim, (unsigned)p.hitter,
            killer, why);
        log(line);
        push_event(killer, p.victim);
    }

    // ---- the badges: the game's own P1 to P4 icons (and our P5), kept as the game draws them (possess_test.cpp) ----
    std::mutex icon_mutex;
    std::array<std::vector<char>, 5> icon_bytes;
    std::array<bool, 5> icon_waiting{};
    std::array<bool, 5> icon_ready{};

    // ---- the UI's side ----
    constexpr int max_lines = 5;
    constexpr auto line_time = std::chrono::seconds(6);
    struct Shown { int killer; int victim; clock::time_point until; };
    std::deque<Shown> shown_lines;
    bool ui_created = false;
    recompui::ContextId context = recompui::ContextId::null();
    recompui::Element* rows[max_lines] = {};
    recompui::Label* badge_k[max_lines] = {};
    recompui::Image* icon_k[max_lines][5] = {};
    recompui::Image* icon_v[max_lines][5] = {};
    recompui::Label* name_k[max_lines] = {};
    recompui::Label* verb[max_lines] = {};
    recompui::Label* badge_v[max_lines] = {};
    recompui::Label* name_v[max_lines] = {};
    std::string shown_signature;
    int shown_mode = -1;

    enum class Mode : uint32_t { Off, Text, Boxed };
    // (Kill Feed: Placement) Top Left, Top Right, Bottom Left, Bottom Right (the default): each corner kept clear of the HUD's own
    // pieces there (top left the ammo's bricks, top right the radar: below them; bottom left above the chat's lines)
    enum class Place : uint32_t { TopLeft, TopRight, BottomLeft, BottomRight };
    Place place_now() {
        if (recomp::config::Config* config = recompui::config::find_option_config("kill_feed_place")) {
            return (Place)std::get<uint32_t>(config->get_option_value("kill_feed_place"));
        }
        return Place::BottomRight;
    }
    recompui::Element* feed_box = nullptr;
    int placed = -1;
    // (the box fills the screen, its lines pushed into the corner: by the flex's own ends, and padding keeping them off the HUD)
    void place_box(Place where) {
        const bool left = where == Place::TopLeft || where == Place::BottomLeft;
        const bool top = where == Place::TopLeft || where == Place::TopRight;
        feed_box->set_justify_content(top ? recompui::JustifyContent::FlexStart : recompui::JustifyContent::FlexEnd);
        feed_box->set_align_items(left ? recompui::AlignItems::FlexStart : recompui::AlignItems::FlexEnd);
        feed_box->set_left(40.0f);
        feed_box->set_right(40.0f);
        feed_box->set_top(where == Place::TopLeft ? 150.0f : 390.0f);
        feed_box->set_bottom(where == Place::BottomLeft ? 340.0f : 150.0f);
    }
    // (the Kill Feed option On or Off; its look, Text or Boxed, the HUD Notifications option's, every message's alike)
    Mode mode_now() {
        if (recomp::config::Config* config = recompui::config::find_option_config("kill_feed")) {
            if (std::get<uint32_t>(config->get_option_value("kill_feed")) == 0) {
                return Mode::Off;
            }
        }
        return conker::notices::boxed() ? Mode::Boxed : Mode::Text;
    }

    // (0.11.5) the players are the match's first D_8008FE40 characters (the players on the screen, the same on every machine) and, online
    // with 5, the bot's character the 5th player plays. (D_80082FA0, the views less one, was used before: online each machine draws one
    // view, so it was 0 there and player 2 was taken for a bot, "Bot 2" in grey.)
    bool is_human(uint8_t* rdram, int k) {
        if (k < 0) {
            return false;
        }
        const int players = (int)(int8_t)MEM_B(0, (gpr)(int32_t)0x8008FE40u);
        const int views = (int)(int32_t)MEM_W(0, (gpr)(int32_t)0x80082FA0u) + 1;
        if (k < std::max(players, views)) {
            return true;
        }
        return conker::netsession::active() && k == conker::possess_test::fifth_object();
    }

    recompui::Color player_colour(int k, bool human) {
        if (!human) {
            return recompui::Color{ 190, 190, 190, 255 };
        }
        switch (k) {
            case 0: return recompui::Color{ 255, 70, 60, 255 };
            case 1: return recompui::Color{ 80, 140, 255, 255 };
            case 2: return recompui::Color{ 70, 220, 90, 255 };
            case 3: return recompui::Color{ 255, 215, 50, 255 };
            default: return recompui::Color{ 200, 110, 255, 255 };
        }
    }

    uint8_t* ui_rdram = nullptr;

    std::string name_of(int k, bool human) {
        if (!human) {
            return "Bot " + std::to_string(k + 1);
        }
        if (conker::netsession::active()) {
            if (k == conker::possess_test::fifth_object()) {
                return conker::netsession::player_name(4);
            }
            const int port = conker::netsession::port_of_player(k);
            return conker::netsession::player_name(port >= 0 ? port : k);
        }
        return "Player " + std::to_string(k + 1);
    }

    void create() {
        context = recompui::create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);
        recompui::Element* box = context.create_element<recompui::Element>(context.get_root_element());
        feed_box = box;
        box->set_position(recompui::Position::Absolute);
        box->set_display(recompui::Display::Flex);
        box->set_flex_direction(recompui::FlexDirection::Column);
        box->set_gap(4.0f);
        placed = (int)place_now();
        place_box((Place)placed);
        for (int i = 0; i < max_lines; i++) {
            rows[i] = context.create_element<recompui::Element>(box);
            rows[i]->set_display(recompui::Display::None);
            rows[i]->set_flex_direction(recompui::FlexDirection::Row);
            rows[i]->set_align_items(recompui::AlignItems::Center);
            rows[i]->set_gap(8.0f);
            rows[i]->set_padding_left(10.0f);
            rows[i]->set_padding_right(10.0f);
            rows[i]->set_padding_top(2.0f);
            rows[i]->set_padding_bottom(2.0f);
            rows[i]->set_border_radius(5.0f);
            auto badge = [&](recompui::Label*& l) {
                l = context.create_element<recompui::Label>(rows[i], "", recompui::LabelStyle::Small);
                l->set_font_size(18.4f);    // 16 + 15%
                l->set_padding_left(4.0f);
                l->set_padding_right(4.0f);
                l->set_border_radius(4.0f);
            };
            auto icons = [&](recompui::Image* (&set)[5]) {
                for (int n = 0; n < 5; n++) {
                    set[n] = context.create_element<recompui::Image>(rows[i], "killfeed_p" + std::to_string(n + 1));
                    set[n]->set_width(25.3f);    // 22 + 15%
                    set[n]->set_height(25.3f);
                    set[n]->set_display(recompui::Display::None);
                }
            };
            icons(icon_k[i]);
            badge(badge_k[i]);
            name_k[i] = context.create_element<recompui::Label>(rows[i], "", recompui::LabelStyle::Normal);
            verb[i] = context.create_element<recompui::Label>(rows[i], "", recompui::LabelStyle::Normal);
            verb[i]->set_color(recompui::Color{ 240, 240, 240, 255 });
            icons(icon_v[i]);
            badge(badge_v[i]);
            name_v[i] = context.create_element<recompui::Label>(rows[i], "", recompui::LabelStyle::Normal);
        }
        context.close();
        ui_created = true;
    }

    void set_who(recompui::Label* badge, recompui::Image* (&icons)[5], recompui::Label* name, int k, const char* after = "") {
        const bool human = ui_rdram != nullptr && is_human(ui_rdram, k);
        const recompui::Color c = player_colour(k, human);
        // (the game's icon once it's been seen: the P1 to P5 one, super small; until then a coloured text badge)
        const int n = human ? std::min(k, 4) : -1;
        for (int m = 0; m < 5; m++) {
            icons[m]->set_display(m == n && icon_ready[(size_t)m] ? recompui::Display::Block : recompui::Display::None);
        }
        badge->set_display(n >= 0 && icon_ready[(size_t)n] ? recompui::Display::None : recompui::Display::Block);
        badge->set_text(human ? "P" + std::to_string(k + 1) : "CPU");
        badge->set_color(recompui::Color{ 20, 20, 20, 255 });
        badge->set_background_color(c);
        name->set_text(name_of(k, human) + after);
        name->set_color(c);
    }
}

extern "C" void conker_kill_feed_frame(uint8_t* rdram) {
    ui_rdram = rdram;
    frames++;
    const bool match = MEM_BU(0, (gpr)(int32_t)0x800BE616) != 0;
    if (!match) {
        have_last = false;
        have_kills = false;
        pending.clear();
        credits.clear();
        return;
    }
    const int count = count_now(rdram);
    watch_kills(rdram, count);
    // deaths: +0x120 of +0x31C leaving 0
    for (int k = 0; k < count; k++) {
        const uint32_t c = characters + (uint32_t)k * character_size;
        const uint32_t st = (uint32_t)MEM_W(0x31C, (gpr)(int32_t)c);
        if ((st >> 24) != 0x80) {
            continue;
        }
        const uint8_t life = MEM_BU(0x120, (gpr)(int32_t)st);
        if (have_last && last_life[k] == 0 && life != 0) {
            pending.push_back({ k, frames, (uint8_t)MEM_BU(k, (gpr)(int32_t)last_hitter) });
        }
        last_life[k] = life;
    }
    have_last = true;
    for (size_t i = 0; i < pending.size();) {
        if (frames >= pending[i].frame + look_after) {
            judge(pending[i]);
            pending.erase(pending.begin() + (std::ptrdiff_t)i);
        } else {
            i++;
        }
    }
}


void conker::killfeed::note_icon(int n, const uint32_t* words) {
    if (n < 0 || n >= 5) {
        return;
    }
    std::vector<char> bytes(32 * 32 * 4);
    for (int i = 0; i < 32 * 32; i++) {
        bytes[(size_t)i * 4 + 0] = (char)(words[i] >> 24);
        bytes[(size_t)i * 4 + 1] = (char)(words[i] >> 16);
        bytes[(size_t)i * 4 + 2] = (char)(words[i] >> 8);
        bytes[(size_t)i * 4 + 3] = (char)words[i];
    }
    std::lock_guard lock{ icon_mutex };
    icon_bytes[(size_t)n] = std::move(bytes);
    icon_waiting[(size_t)n] = true;
}

void conker::killfeed::update() {
    if (!ultramodern::is_game_started()) {
        return;
    }
    {
        std::lock_guard lock{ icon_mutex };
        for (int n = 0; n < 5; n++) {
            if (icon_waiting[(size_t)n]) {
                icon_waiting[(size_t)n] = false;
                recompui::queue_image_from_bytes_rgba32("killfeed_p" + std::to_string(n + 1), icon_bytes[(size_t)n], 32, 32);
                icon_ready[(size_t)n] = true;
                shown_signature.clear();   // (drawn again with it)
            }
        }
    }
    const Mode mode = mode_now();
    const clock::time_point now = clock::now();
    {
        std::lock_guard lock{ events_mutex };
        while (!events.empty()) {
            const Event e = events.front();
            events.pop_front();
            if (mode != Mode::Off) {
                shown_lines.push_back({ e.killer, e.victim, now + line_time });
            }
        }
    }
    while (!shown_lines.empty() && (shown_lines.front().until <= now || shown_lines.size() > (size_t)max_lines)) {
        shown_lines.pop_front();
    }
    std::string signature;
    for (const Shown& s : shown_lines) {
        signature += std::to_string(s.killer) + ">" + std::to_string(s.victim) + ";";
    }
    if (mode == Mode::Off || shown_lines.empty()) {
        if (ui_created && recompui::is_context_shown(context)) {
            recompui::hide_context(context);
        }
        shown_signature.clear();
        return;
    }
    if (!ui_created) {
        create();
    }
    if ((int)place_now() != placed) {
        placed = (int)place_now();
        context.open();
        place_box((Place)placed);
        context.close();
    }
    if (signature != shown_signature || (int)mode != shown_mode) {
        shown_signature = signature;
        shown_mode = (int)mode;
        context.open();
        for (int i = 0; i < max_lines; i++) {
            if (i < (int)shown_lines.size()) {
                const Shown& s = shown_lines[(size_t)i];
                const bool killed = s.killer >= 0 && s.killer != s.victim;
                badge_k[i]->set_display(killed ? recompui::Display::Block : recompui::Display::None);
                for (int m = 0; m < 5; m++) {
                    icon_k[i][m]->set_display(recompui::Display::None);
                }
                name_k[i]->set_display(killed ? recompui::Display::Block : recompui::Display::None);
                if (killed) {
                    set_who(badge_k[i], icon_k[i], name_k[i], s.killer);
                }
                verb[i]->set_text(killed ? "killed" : "");
                verb[i]->set_display(killed ? recompui::Display::Block : recompui::Display::None);
                set_who(badge_v[i], icon_v[i], name_v[i], s.victim, killed ? "" : " died");
                rows[i]->set_background_color(mode == Mode::Boxed ? recompui::Color{ 6, 9, 4, 170 } : recompui::Color{ 0, 0, 0, 0 });
                rows[i]->set_display(recompui::Display::Flex);
            } else {
                rows[i]->set_display(recompui::Display::None);
            }
        }
        context.close();
    }
    if (!recompui::is_context_shown(context)) {
        recompui::show_context(context, "");
    }
}
