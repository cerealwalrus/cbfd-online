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
// Who killed whom: not known yet from the game's code. Each death is looked at half a second later: every other character's counters
// (bytes and halfwords of its +0x31C and of the character itself) that went up by exactly one since the frame before the death are the
// candidates for its kills, kept by place. A place where exactly one other character went up by one at two deaths or more, and never two
// characters at once, is taken for the kills' counter from then on: the one who went up there is the killer. Until it's found the line is
// "Mark died". Every death's candidates go in killfeed.log, to find the game's own place for the next version.

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
            log_file = std::fopen("killfeed.log", "w");
            if (log_file == nullptr) {
                return;
            }
        }
        std::fprintf(log_file, "%s\n", line);
        std::fflush(log_file);
    }

    struct Snapshot {
        bool valid = false;
        std::array<uint8_t, character_size> body{};
        std::array<uint8_t, state_size> state{};
    };
    struct Pending {
        int victim;
        uint32_t frame;
        std::array<Snapshot, max_chars> before;
        std::array<int32_t, 16> scores_before{};
        bool have_scores = false;
        std::array<uint8_t, 0x40> matrix_before{};
        bool have_matrix = false;
        std::array<uint8_t, 0x60> stats_before{};
        bool have_stats = false;
    };
    // (30 Oct, again) D_800E0AD0 turned out to be two bytes a character (the HUD's func reads it at its number x 2), its second the
    // deaths: no killer in it. The players' stats are in the game state (gGameState, 0x8008FDD4's pointer): +0x46 (0x20 bytes) and +0x66
    // (0xC a player, cleared as a match starts; +0x66 shots that hit, +0x6C shots fired, as the HUD shows them). The killer is the one
    // player other than the victim with a halfword there gone up by one at the death; every one of them logged to pin it down.
    std::array<uint8_t, 0x60> stats_last{};
    bool have_stats_last = false;
    bool read_stats(uint8_t* rdram, std::array<uint8_t, 0x60>& out) {
        const uint32_t gs = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8008FDD4u);
        if ((gs >> 24) != 0x80 || (gs & 0x00FFFFFFu) >= 0x007FF000u) {
            return false;
        }
        for (uint32_t i = 0; i < out.size(); i++) {
            out[i] = (uint8_t)MEM_BU(0x40 + i, (gpr)(int32_t)gs);
        }
        return true;
    }
    // (0.11.4, the killer at last) D_800E0AD0 is the game's table of kills, a byte for each victim and killer: victim x the characters +
    // the killer (the 30 Oct killfeed.log: character 0 killed by 1, +1; character 1 by itself, a fall, +3; the scores table never moved
    // and D_800E0AC0 is the places, 1st and 2nd, not slots). The byte that went up at a death gives the killer.
    constexpr uint32_t kill_matrix = 0x800E0AD0u;
    std::array<uint8_t, 0x40> matrix_last{};
    bool have_matrix_last = false;
    void read_matrix(uint8_t* rdram, std::array<uint8_t, 0x40>& out) {
        for (uint32_t i = 0; i < out.size(); i++) {
            out[i] = (uint8_t)MEM_BU(i, (gpr)(int32_t)kill_matrix);
        }
    }
    // (the killer, from the game's own scores) The HUD's score ("1 (2nd)", func_15091B..., 0x15092000) is a word of the table that
    // D_800E0BD8 points to, +0x20 + 4 x the slot, the slot each character's byte in D_800E0AC0. A kill puts one on the killer's slot's
    // score: the slot whose score went up, and the character in it other than the one who died, is the killer.
    std::array<int32_t, 16> scores_last{};
    bool have_scores_last = false;
    bool read_scores(uint8_t* rdram, std::array<int32_t, 16>& out) {
        const uint32_t table = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800E0BD8u);
        if ((table >> 24) != 0x80 || (table & 0x00FFFFFFu) >= 0x007FF000u) {
            return false;
        }
        for (int i = 0; i < 16; i++) {
            out[(size_t)i] = (int32_t)MEM_W(0x20 + 4 * i, (gpr)(int32_t)table);
        }
        return true;
    }
    std::array<Snapshot, max_chars> last;
    // (0.11.4, finding the score table: the characters' own counters weren't it) the game's globals, 0x80080000 to 0x800F0000, as the
    // frame before each death left them; half a second after, every byte that went up by exactly one is logged
    constexpr uint32_t globals_from = 0x80080000u, globals_to = 0x800F0000u;
    std::vector<uint8_t> globals_last;
    struct PendingGlobals { uint32_t frame; std::vector<uint8_t> before; };
    std::vector<PendingGlobals> pending_globals;
    std::array<uint8_t, max_chars> last_life{};
    bool have_last = false;
    uint32_t frames = 0;
    std::vector<Pending> pending;

    // a place: (0: the character, 1: its +0x31C) << 16 | offset << 1 | (1: a halfword)
    struct Score { int alone = 0; int crowded = 0; };
    std::map<uint32_t, Score> scores;
    int counter_place = -1;

    struct Event { int killer; int victim; };
    std::mutex events_mutex;
    std::deque<Event> events;

    int count_now(uint8_t* rdram) {
        const int n = (int)MEM_B(0, (gpr)(int32_t)0x8008FD8Cu);   // (func_1507C8FC's count of characters)
        return std::clamp(n, 0, max_chars);
    }

    bool take(uint8_t* rdram, int k, Snapshot& s) {
        const uint32_t c = characters + (uint32_t)k * character_size;
        const uint32_t st = (uint32_t)MEM_W(0x31C, (gpr)(int32_t)c);
        if ((st >> 24) != 0x80 || (st & 0x00FFFFFFu) >= 0x007FF000u) {
            s.valid = false;
            return false;
        }
        for (uint32_t i = 0; i < character_size; i++) {
            s.body[i] = (uint8_t)MEM_BU(i, (gpr)(int32_t)c);
        }
        for (uint32_t i = 0; i < state_size; i++) {
            s.state[i] = (uint8_t)MEM_BU(i, (gpr)(int32_t)st);
        }
        s.valid = true;
        return true;
    }

    uint32_t read_place(const Snapshot& s, uint32_t place) {
        const bool half = (place & 1u) != 0;
        const uint32_t off = (place >> 1) & 0x7FFFu;
        const uint8_t* p = ((place >> 16) == 0) ? s.body.data() : s.state.data();
        const uint32_t size = ((place >> 16) == 0) ? character_size : state_size;
        if (off + (half ? 2u : 1u) > size) {
            return 0;
        }
        return half ? ((uint32_t)p[off] << 8 | p[off + 1]) : p[off];
    }

    void push_event(int killer, int victim) {
        std::lock_guard lock{ events_mutex };
        events.push_back({ killer, victim });
        while (events.size() > 32) {
            events.pop_front();
        }
    }

    void judge(uint8_t* rdram, const Pending& p, int count) {
        // every other character's places that went up by one since the frame before the death
        std::map<uint32_t, std::vector<int>> up;
        for (int k = 0; k < count; k++) {
            if (k == p.victim || !p.before[k].valid) {
                continue;
            }
            Snapshot now;
            if (!take(rdram, k, now)) {
                continue;
            }
            for (uint32_t region = 0; region < 2; region++) {
                const uint32_t size = region == 0 ? character_size : state_size;
                for (uint32_t off = 0; off < size; off++) {
                    for (uint32_t half = 0; half < 2; half++) {
                        if (half && (off & 1u)) {
                            continue;
                        }
                        const uint32_t place = (region << 16) | (off << 1) | half;
                        if (read_place(now, place) == read_place(p.before[k], place) + 1) {
                            up[place].push_back(k);
                        }
                    }
                }
            }
        }
        int killer = -1;
        // (the game state's players' stats: the halfword gone up by one, and whose block it's in)
        if (p.have_stats) {
            std::array<uint8_t, 0x60> now{};
            if (read_stats(rdram, now)) {
                std::set<int> who;
                std::string notes;
                for (uint32_t off = 0; off + 1 < now.size(); off += 2) {
                    const int was = (int)((p.stats_before[off] << 8) | p.stats_before[off + 1]);
                    const int is = (int)((now[off] << 8) | now[off + 1]);
                    if (is == was) {
                        continue;
                    }
                    const uint32_t at = 0x40 + off;
                    int player = -1;
                    if (at >= 0x66 && at < 0x96) {
                        player = (int)((at - 0x66) / 0xC);
                    } else if (at >= 0x46 && at < 0x66) {
                        player = (int)((at - 0x46) / 8);
                    }
                    char piece[48];
                    std::snprintf(piece, sizeof(piece), " +0x%X %d->%d (player %d)", (unsigned)at, was, is, player);
                    notes += piece;
                    if (is == was + 1 && player >= 0 && player != p.victim) {
                        who.insert(player);
                    }
                }
                if (who.size() == 1) {
                    killer = *who.begin();
                }
                char sline[400];
                std::snprintf(sline, sizeof(sline), "[kill feed] f=%u the game state's stats:%s -> killer %d", p.frame, notes.empty() ? " none changed" : notes.c_str(), killer);
                log(sline);
            }
        }
        // (failing that: a halfword of another character's +0x31C block between +0x80 and +0x200 gone up by one, alone: the kills by weapon
        // there, by the 30 Oct killfeed.log: +0x174 for the killer as player 1 shot Bot 2, +0x138 and +0x90 for the bot's own kills)
        if (killer < 0) {
            std::set<int> who;
            for (const auto& [place, ks] : up) {
                const uint32_t off = (place >> 1) & 0x7FFF;
                if ((place >> 16) == 1 && (place & 1) && off >= 0x80 && off < 0x200 && off != 0x11A && ks.size() == 1) {
                    who.insert(ks[0]);
                }
            }
            if (who.size() == 1) {
                killer = *who.begin();
            }
        }
        // (nobody else's: by its own hand)
        if (killer < 0) {
            killer = p.victim;
        }
        // (the game's D_800E0AD0, for the log only now)
        if (p.have_matrix) {
            std::array<uint8_t, 0x40> now{};
            read_matrix(rdram, now);
            int at = -1;
            for (int i = 0; i < (int)now.size(); i++) {
                if (now[(size_t)i] == (uint8_t)(p.matrix_before[(size_t)i] + 1)) {
                    at = i;
                    break;
                }
            }
            int rows = -1;
            if (at >= 0) {
                // (rows as long as the characters, or failing that whatever length puts the byte in the victim's row)
                if (count > 0 && at / count == p.victim) {
                    rows = count;
                } else {
                    for (int n = count; n <= 16; n++) {
                        if (n > 0 && at / n == p.victim) {
                            rows = n;
                            break;
                        }
                    }
                }
                (void)rows;
            }
            char mline[160];
            std::snprintf(mline, sizeof(mline), "[kill feed] f=%u D_800E0AD0: byte %d went up (character %d's deaths)", p.frame, at, at >= 0 ? at / 2 : -1);
            log(mline);
        }
        // (the scores: the slot that went up)
        if (killer < 0 && p.have_scores) {
            std::array<int32_t, 16> now{};
            if (read_scores(rdram, now)) {
                int slot = -1;
                for (int i = 0; i < 16; i++) {
                    if (now[(size_t)i] > p.scores_before[(size_t)i]) {
                        slot = i;
                        break;
                    }
                }
                char sline[160];
                std::snprintf(sline, sizeof(sline), "[kill feed] f=%u scores: slot %d went up (victim %d's slot %d)", p.frame, slot, p.victim,
                    (int)MEM_B(p.victim, (gpr)(int32_t)0x800E0AC0u));
                log(sline);
                if (slot >= 0) {
                    for (int k = 0; k < count; k++) {
                        if (k != p.victim && (int)MEM_B(k, (gpr)(int32_t)0x800E0AC0u) == slot) {
                            killer = k;
                            break;
                        }
                    }
                }
            }
        }
        if (killer < 0 && counter_place >= 0 && false) {
            auto it = up.find((uint32_t)counter_place);
            if (it != up.end() && it->second.size() == 1) {
                killer = it->second[0];
            }
        }
        for (const auto& [place, who] : up) {
            Score& s = scores[place];
            (who.size() == 1 ? s.alone : s.crowded)++;
        }
        // the counter: the place alone most often (at least twice), never crowded
        if (counter_place < 0 && false) {
            int best = -1, best_alone = 1;
            for (const auto& [place, s] : scores) {
                if (s.crowded == 0 && s.alone > best_alone) {
                    best = (int)place;
                    best_alone = s.alone;
                }
            }
            if (best >= 0) {
                counter_place = best;
                char line[120];
                std::snprintf(line, sizeof(line), "[kill feed] the kills' counter taken to be %s +0x%X (%s), up alone at %d deaths",
                    (best >> 16) == 0 ? "the character" : "its +0x31C", (unsigned)((best >> 1) & 0x7FFF), (best & 1) ? "halfword" : "byte", best_alone);
                log(line);
                auto it = up.find((uint32_t)best);
                if (it != up.end() && it->second.size() == 1) {
                    killer = it->second[0];
                }
            }
        }
        char line[160];
        std::snprintf(line, sizeof(line), "[kill feed] f=%u character %d died (its +0x120 %02X); killer %d; %zu places up by one in the others:",
            p.frame, p.victim, (unsigned)last_life[p.victim], killer, up.size());
        log(line);
        int shown = 0;
        for (const auto& [place, who] : up) {
            if (shown++ >= 60) {
                break;
            }
            std::string ks;
            for (int k : who) {
                ks += " " + std::to_string(k);
            }
            std::snprintf(line, sizeof(line), "[kill feed]   %s +0x%X %s: characters%s", (place >> 16) == 0 ? "body" : "+0x31C", (unsigned)((place >> 1) & 0x7FFF),
                (place & 1) ? "h" : "b", ks.c_str());
            log(line);
        }
        push_event(killer, p.victim);
        for (size_t g = 0; g < pending_globals.size(); g++) {
            if (pending_globals[g].frame != p.frame) {
                continue;
            }
            std::string row;
            int n = 0;
            for (uint32_t a = globals_from; a < globals_to; a++) {
                const uint8_t was = pending_globals[g].before[a - globals_from];
                const uint8_t now = (uint8_t)MEM_BU(0, (gpr)(int32_t)a);
                if (now == (uint8_t)(was + 1) && was < 200) {
                    char piece[16];
                    std::snprintf(piece, sizeof(piece), " %08X:%u", (unsigned)a, (unsigned)now);
                    row += piece;
                    if (++n >= 400) {
                        break;
                    }
                }
            }
            char head[96];
            std::snprintf(head, sizeof(head), "[kill feed] f=%u character %d's death: globals up by one (%d):", p.frame, p.victim, n);
            log(head);
            for (size_t at = 0; at < row.size(); at += 180) {
                log(("[kill feed]  " + row.substr(at, 180)).c_str());
            }
            pending_globals.erase(pending_globals.begin() + (std::ptrdiff_t)g);
            break;
        }
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

    bool is_human(uint8_t* rdram, int k) {
        return k >= 0 && k <= (int)(int32_t)MEM_W(0, (gpr)(int32_t)0x80082FA0u);
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
        pending.clear();
        return;
    }
    const int count = count_now(rdram);
    // deaths: +0x120 of +0x31C leaving 0
    for (int k = 0; k < count; k++) {
        const uint32_t c = characters + (uint32_t)k * character_size;
        const uint32_t st = (uint32_t)MEM_W(0x31C, (gpr)(int32_t)c);
        if ((st >> 24) != 0x80) {
            continue;
        }
        const uint8_t life = MEM_BU(0x120, (gpr)(int32_t)st);
        if (have_last && last[k].valid && last_life[k] == 0 && life != 0) {
            Pending p;
            p.victim = k;
            p.frame = frames;
            p.before = last;
            p.scores_before = scores_last;
            p.have_scores = have_scores_last;
            p.matrix_before = matrix_last;
            p.have_matrix = have_matrix_last;
            p.stats_before = stats_last;
            p.have_stats = have_stats_last;
            pending.push_back(p);
            if (!globals_last.empty() && pending_globals.size() < 8) {
                pending_globals.push_back({ frames, globals_last });
            }
            char line[100];
            std::snprintf(line, sizeof(line), "[kill feed] f=%u character %d: +0x120 0 -> %02X", frames, k, (unsigned)life);
            log(line);
        }
        last_life[k] = life;
    }
    for (size_t i = 0; i < pending.size();) {
        if (frames >= pending[i].frame + look_after) {
            judge(rdram, pending[i], count);
            pending.erase(pending.begin() + (std::ptrdiff_t)i);
        } else {
            i++;
        }
    }
    for (int k = 0; k < max_chars; k++) {
        if (k < count) {
            take(rdram, k, last[k]);
        } else {
            last[k].valid = false;
        }
    }
    have_last = true;
    have_scores_last = read_scores(rdram, scores_last);
    read_matrix(rdram, matrix_last);
    have_matrix_last = true;
    have_stats_last = read_stats(rdram, stats_last);
    globals_last.resize(globals_to - globals_from);
    for (uint32_t a = globals_from; a < globals_to; a += 4) {
        const uint32_t w = (uint32_t)MEM_W(0, (gpr)(int32_t)a);
        globals_last[a - globals_from + 0] = (uint8_t)(w >> 24);
        globals_last[a - globals_from + 1] = (uint8_t)(w >> 16);
        globals_last[a - globals_from + 2] = (uint8_t)(w >> 8);
        globals_last[a - globals_from + 3] = (uint8_t)w;
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
