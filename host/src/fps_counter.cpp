// Show FPS (Graphics tab): a small counter in the top-right corner while the game runs, to spot
// slowdowns. It shows two rates, each averaged over half a second:
// - FPS: frames RT64 presented to the screen (RT64::presentedFrameCount, added by rt64.patch),
//   interpolated ones included. A drop here is the PC falling behind.
// - Game: frames the game itself made (a display list started, conker_frame_dl_begin in
//   widescreen.cpp). Conker runs at up to 30; a drop here with FPS steady is the game's own
//   slowdown, as it would be on the N64.
//
// It's a recompui context of its own that takes no input and no mouse, so the game and the
// menus work as before, shown only once the game has started (the launcher comes back when
// nothing is shown before then). recompui's UI is made on the render thread as RT64 starts, and
// update_gfx can run before that (the game started from the command line): the context is only
// made once the launcher's init has run (on_ui_ready), or creating it read the UI before it existed.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

#include "ultramodern/ultramodern.hpp"
#include "recompui/recompui.h"
#include "elements/ui_label.h"

#include "conker.hpp"

namespace RT64 {
    uint64_t presentedFrameCount();
    void latencyMark();
    bool latencyResult(uint64_t *serial, int64_t *firstNs, int64_t *fullNs);
}

namespace {
    using clock = std::chrono::steady_clock;
    constexpr auto interval = std::chrono::milliseconds(500);

    std::atomic<uint64_t> game_frames = 0;
    std::atomic<bool> ui_ready = false;

    bool created = false;
    recompui::ContextId context = recompui::ContextId::null();
    recompui::Label* label = nullptr;

    clock::time_point last_time;
    uint64_t last_presented = 0;
    uint64_t last_game = 0;

    void create() {
        context = recompui::create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);
        recompui::Element* box = context.create_element<recompui::Element>(context.get_root_element());
        box->set_position(recompui::Position::Absolute);
        box->set_top(8.0f);
        box->set_right(8.0f);
        box->set_padding_left(8.0f);
        box->set_padding_right(8.0f);
        box->set_padding_top(2.0f);
        box->set_padding_bottom(2.0f);
        box->set_border_radius(6.0f);
        box->set_background_color(recompui::theme::color::BGOverlay);
        label = context.create_element<recompui::Label>(box, "", recompui::LabelStyle::Small);
        context.close();
        created = true;
    }
}

void conker::fps_counter::game_frame() {
    game_frames.fetch_add(1, std::memory_order_relaxed);
}

void conker::fps_counter::on_ui_ready() {
    ui_ready = true;
}

void conker::fps_counter::update() {
    const bool wanted = ui_ready && (conker::fps_counter::enabled() || conker::latency_meter::enabled()) && ultramodern::is_game_started();
    if (!wanted) {
        if (created && recompui::is_context_shown(context)) {
            recompui::hide_context(context);
        }
        return;
    }
    if (!created) {
        create();
    }
    if (!recompui::is_context_shown(context)) {
        recompui::show_context(context, "");
        last_time = clock::time_point{};
    }

    const clock::time_point now = clock::now();
    const uint64_t presented = RT64::presentedFrameCount();
    const uint64_t game = game_frames.load(std::memory_order_relaxed);
    if (last_time == clock::time_point{}) {
        last_time = now;
        last_presented = presented;
        last_game = game;
        return;
    }
    if (now - last_time < interval) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - last_time).count();
    char text[64];
    std::snprintf(text, sizeof(text), "%.0f FPS  |  Game %.0f", (presented - last_presented) / seconds,
        (game - last_game) / seconds);
    std::string shown = conker::fps_counter::enabled() ? std::string(text) : std::string();
    const std::string latency = conker::latency_meter::text();
    if (!latency.empty()) {
        shown += shown.empty() ? latency : "\n" + latency;
    }
    last_time = now;
    last_presented = presented;
    last_game = game;

    context.open();
    label->set_text(shown);
    context.close();
}

// ---- (0.11.4) Show Latency ----
// One press at a time, followed through: the press (SDL's event, its own timestamp), the game reading it (this machine's player's
// controller as given to the game showing a new button or dedicated control down: offline the controller read live, online the
// frame's agreed input, after the input delay), the display list the game makes next (begun, finished: its start address), that list
// handed to RT64 (rt64.patch's latencyMark just before: RT64 marks the workload made from it), and the screen: the first frame drawn
// that shows any of it (with interpolation, the first in-between frame toward it) and the one that shows all of it. Each measurement
// in latency.log, and the last 20's average in the corner. Not counted: the mouse's or controller's own delay and the monitor's.
namespace {
    using ns_clock = std::chrono::steady_clock;
    int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(ns_clock::now().time_since_epoch()).count();
    }
    enum class Stage { Idle, Pressed, Read, Begun, Ended, Sent };
    std::mutex lat_mutex;
    Stage stage = Stage::Idle;
    int64_t t_press = 0, t_read = 0, t_begin = 0, t_end = 0, t_sent = 0;
    uint32_t marked_list = 0;
    uint32_t sends_since_end = 0;   // lists handed over since the frame's ended (its start not matched: the next one taken after 2)
    uint32_t last_buttons = 0;
    uint64_t last_serial = 0;
    struct Sample { double read, made, first, full; };
    std::deque<Sample> samples;
    std::string corner;
    std::FILE* lat_file = nullptr;
    uint32_t lat_count = 0;
    std::atomic<bool> lat_on_cached = false;

    void lat_reset_if_stale(int64_t now) {
        // (a press the game never reads, a key bound to nothing say, dropped after 250 ms: the first try read the next press instead,
        // and counted 600 ms)
        if ((stage == Stage::Pressed && now - t_press > 250'000'000LL) || (stage != Stage::Idle && now - t_press > 2'000'000'000LL)) {
            stage = Stage::Idle;
        }
    }
}

void conker::latency_meter::pressed(int64_t at_ns) {
    if (!lat_on_cached.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard lock{ lat_mutex };
    lat_reset_if_stale(now_ns());
    if (stage == Stage::Idle) {
        stage = Stage::Pressed;
        t_press = at_ns;
    }
}

void conker::latency_meter::game_read(uint32_t buttons) {
    if (!lat_on_cached.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard lock{ lat_mutex };
    lat_reset_if_stale(now_ns());
    const bool new_down = (buttons & ~last_buttons) != 0;
    last_buttons = buttons;
    if (stage == Stage::Pressed && new_down) {
        stage = Stage::Read;
        t_read = now_ns();
    }
}

void conker::latency_meter::frame_begin() {
    if (!lat_on_cached.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard lock{ lat_mutex };
    if (stage == Stage::Read) {
        stage = Stage::Begun;
        t_begin = now_ns();
    }
}

void conker::latency_meter::frame_end(uint32_t list_start) {
    if (!lat_on_cached.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard lock{ lat_mutex };
    if (stage == Stage::Begun) {
        stage = Stage::Ended;
        t_end = now_ns();
        marked_list = list_start & 0x1FFFFFFFu;
        sends_since_end = 0;
    }
}

void conker::latency_meter::before_send(uint32_t list) {
    if (!lat_on_cached.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard lock{ lat_mutex };
    if (stage == Stage::Ended && ((list & 0x1FFFFFFFu) == marked_list || ++sends_since_end > 2)) {
        stage = Stage::Sent;
        t_sent = now_ns();
        RT64::latencyMark();
    }
}

std::string conker::latency_meter::text() {
    const bool on = conker::latency_meter::enabled();
    lat_on_cached.store(on, std::memory_order_relaxed);
    if (!on) {
        return std::string();
    }
    uint64_t serial = 0;
    int64_t first = 0, full = 0;
    std::lock_guard lock{ lat_mutex };
    if (RT64::latencyResult(&serial, &first, &full) && serial != last_serial) {
        last_serial = serial;
        if (stage == Stage::Sent && first >= t_sent && full >= first) {
            const Sample s{ (t_read - t_press) / 1e6, (t_sent - t_read) / 1e6, (first - t_press) / 1e6, (full - t_press) / 1e6 };
            samples.push_back(s);
            if (samples.size() > 20) {
                samples.pop_front();
            }
            if (lat_file == nullptr) {
                lat_file = std::fopen("latency.log", "w");
                if (lat_file != nullptr) {
                    std::fprintf(lat_file, "Show Latency: each press, in ms from the press (SDL's event): the game reads it, its frame handed to "
                        "the renderer, the screen first shows any of that frame (interpolated), the screen shows all of it\n");
                }
            }
            if (lat_file != nullptr && lat_count < 5000) {
                lat_count++;
                std::fprintf(lat_file, "#%u  read %.1f  handed over %.1f  first shown %.1f  fully shown %.1f  (online %s)\n", (unsigned)lat_count,
                    s.read, s.read + s.made, s.first, s.full, conker::netsession::active() ? "yes" : "no");
                std::fflush(lat_file);
            }
        }
        stage = Stage::Idle;
    }
    lat_reset_if_stale(now_ns());
    if (samples.empty()) {
        return "Latency: press a button";
    }
    double first_sum = 0.0, full_sum = 0.0, read_sum = 0.0;
    for (const Sample& s : samples) {
        first_sum += s.first;
        full_sum += s.full;
        read_sum += s.read;
    }
    const double n = (double)samples.size();
    char text[128];
    std::snprintf(text, sizeof(text), "Latency %.0f ms  (full %.0f, game read %.0f)", first_sum / n, full_sum / n, read_sum / n);
    return std::string(text);
}
