// Text chat for an online game. Y (on the keyboard) opens a line to type on; Enter sends it (Enter on an empty line closes it).
// The lines of every player show in the bottom-left corner for a few seconds. While you type, your own controller is held
// still (net_session.cpp asks typing()), and for a moment after, so the Enter that sends the line isn't a press of Start.
//
// Two recompui contexts, made when first needed (as notices.cpp does): the lines (which take no input) and the typing box
// (which does, while it's up). The lines travel through the host (net_session.cpp's chat functions); nothing here touches the
// lockstep. Esc isn't used to close the box: the UI's own Esc opens the settings menu.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <deque>
#include <string>

#include <SDL.h>

#include "recomp.h"
#include "recompui/config.h"
#include "recompui/recompui.h"
#include "elements/ui_label.h"
#include "elements/ui_text_input.h"
#include "elements/ui_theme.h"
#include "ultramodern/ultramodern.hpp"

#include "conker.hpp"

namespace {
    using clock = std::chrono::steady_clock;
    constexpr int max_lines = 8;
    constexpr auto line_time = std::chrono::seconds(10);
    constexpr auto still_after = std::chrono::milliseconds(500);

    struct Line {
        int slot;
        std::string text;
        clock::time_point until;
    };
    std::deque<Line> lines;

    recompui::ContextId lines_context = recompui::ContextId::null();
    recompui::ContextId input_context = recompui::ContextId::null();
    recompui::Element* lines_box = nullptr;
    recompui::Label* line_labels[max_lines] = {};
    recompui::TextInput* input = nullptr;
    bool created = false;
    std::atomic<bool> typing_now{ false };
    std::atomic<int64_t> still_until_ms{ 0 };   // steady clock, ms
    bool open = false;
    std::atomic<bool> input_focused{ false };   // the typing box has the keyboard (the UI drops it when the mouse moves)
    std::string typed;
    bool swallow_key = false;                    // the key that opened the box is still down: its character is not part of the line
    int64_t swallow_until_ms = 0;                // ... and a moment after it came up
    bool clear_box = false;
    bool y_was_down = false, enter_was_down = false;
    std::string shown_signature;

    // (0.11.4) the General tab's Chat: Key, a key of the keyboard bound to it (recomp/recompfrontend_chatkey.patch: its button waits for
    // the next key), Y to begin with, in place of its preset keys; none, chat is off (the others' lines still show)
    bool chat_key_down(bool* none_bound) {
        std::string name = "Y";
        if (recomp::config::Config* config = recompui::config::find_option_config("chat_key")) {
            name = std::get<std::string>(config->get_option_value("chat_key"));
        }
        const SDL_Scancode code = name.empty() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(name.c_str());
        *none_bound = code == SDL_SCANCODE_UNKNOWN;
        if (*none_bound) {
            return false;
        }
        int count = 0;
        const Uint8* keys = SDL_GetKeyboardState(&count);
        return (int)code < count && keys[code] != 0;
    }

    int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count();
    }

    recompui::theme::color player_color(int slot) {
        switch (slot) {
            case 0: return recompui::theme::color::Player1;
            case 1: return recompui::theme::color::Player2;
            case 2: return recompui::theme::color::Player3;
            default: return recompui::theme::color::Player4;
        }
    }

    void create() {
        lines_context = recompui::create_context();
        lines_context.open();
        lines_context.set_captures_input(false);
        lines_context.set_captures_mouse(false);
        lines_box = lines_context.create_element<recompui::Element>(lines_context.get_root_element());
        lines_box->set_position(recompui::Position::Absolute);
        lines_box->set_left(16.0f);
        lines_box->set_bottom(150.0f);
        lines_box->set_max_width(760.0f);
        lines_box->set_display(recompui::Display::Flex);
        lines_box->set_flex_direction(recompui::FlexDirection::Column);
        lines_box->set_gap(2.0f);
        for (int i = 0; i < max_lines; i++) {
            line_labels[i] = lines_context.create_element<recompui::Label>(lines_box, "", recompui::LabelStyle::Normal);
            line_labels[i]->set_padding_left(10.0f);
            line_labels[i]->set_padding_right(10.0f);
            line_labels[i]->set_padding_top(2.0f);
            line_labels[i]->set_padding_bottom(2.0f);
            line_labels[i]->set_border_radius(5.0f);
            line_labels[i]->set_background_color(recompui::Color{ 6, 9, 4, 170 });
            line_labels[i]->set_display(recompui::Display::None);
        }
        lines_context.close();

        input_context = recompui::create_context();
        input_context.open();
        input_context.set_captures_input(true);
        input_context.set_captures_mouse(false);
        recompui::Element* bar = input_context.create_element<recompui::Element>(input_context.get_root_element());
        bar->set_position(recompui::Position::Absolute);
        bar->set_left(16.0f);
        bar->set_bottom(80.0f);
        bar->set_width(700.0f);
        bar->set_display(recompui::Display::Flex);
        bar->set_flex_direction(recompui::FlexDirection::Row);
        bar->set_align_items(recompui::AlignItems::Center);
        bar->set_gap(10.0f);
        bar->set_padding(10.0f);
        bar->set_border_radius(8.0f);
        bar->set_background_color(recompui::Color{ 6, 9, 4, 220 });
        input_context.create_element<recompui::Label>(bar, "Say:", recompui::LabelStyle::Normal);
        input = input_context.create_element<recompui::TextInput>(bar);
        input->set_flex_grow(1.0f);
        input->set_width(100.0f, recompui::Unit::Percent);
        input->add_text_changed_callback([](const std::string& text) {
            // opening the box with a key also types that key's character into it: the first letter, still in the box, is dropped
            if (!text.empty() && text.size() == 1 && (swallow_key || now_ms() < swallow_until_ms)) {
                typed.clear();
                clear_box = true;
                return;
            }
            typed = text;
        });
        input->set_focus_callback([](bool active) { input_focused = active; });
        input_context.close();
        created = true;
    }

    void open_box() {
        typed.clear();
        input_context.open();
        input->set_text("");
        input_context.close();
        recompui::show_context(input_context, "");
        input_context.open();
        input->focus();
        input_context.close();
        open = true;
        typing_now = true;
        input_focused = false;
        swallow_key = true;
        swallow_until_ms = 0;
        clear_box = false;
    }

    void close_box() {
        if (recompui::is_context_shown(input_context)) {
            recompui::hide_context(input_context);
        }
        open = false;
        typing_now = false;
        still_until_ms = now_ms() + std::chrono::duration_cast<std::chrono::milliseconds>(still_after).count();
    }

    void refresh_lines(clock::time_point now) {
        std::string signature = open ? "o" : "c";
        std::vector<const Line*> visible;
        for (auto it = lines.rbegin(); it != lines.rend() && (int)visible.size() < max_lines; ++it) {
            if (open || now < it->until) {
                visible.push_back(&*it);
                signature += std::to_string(it->slot) + ":" + it->text + "\n";
            }
        }
        if (signature == shown_signature) {
            return;
        }
        shown_signature = signature;
        lines_context.open();
        for (int i = 0; i < max_lines; i++) {
            // the oldest of the visible ones at the top
            const int k = (int)visible.size() - 1 - i;
            if (k >= 0) {
                const Line* line = visible[k];
                if (line->slot < 0) {
                    line_labels[i]->set_text(line->text);
                    line_labels[i]->set_color(recompui::Color{ 236, 206, 112, 255 });
                }
                else {
                    line_labels[i]->set_text(conker::netsession::player_name(line->slot) + ": " + line->text);
                    line_labels[i]->set_color(player_color(line->slot));
                }
                line_labels[i]->set_display(recompui::Display::Block);
            }
            else {
                line_labels[i]->set_display(recompui::Display::None);
            }
        }
        lines_context.close();
        if (visible.empty()) {
            if (recompui::is_context_shown(lines_context)) {
                recompui::hide_context(lines_context);
            }
        }
        else if (!recompui::is_context_shown(lines_context)) {
            recompui::show_context(lines_context, "");
        }
    }
}

bool conker::netchat::typing() {
    return typing_now.load() || now_ms() < still_until_ms.load();
}

void conker::netchat::update() {
    if (!ultramodern::is_game_started() || !conker::netsession::chat_available()) {
        if (created && open) {
            close_box();
        }
        return;
    }
    if (!created) {
        create();
    }
    const clock::time_point now = clock::now();
    // lines that came in
    for (auto& [slot, text] : conker::netsession::chat_poll()) {
        // (/bot from anyone: applied on every machine alike)
        const std::string reply = conker::mp_characters::bot_command(text);
        if (!reply.empty()) {
            lines.push_back({ -1, reply, now + line_time });
        }
        lines.push_back({ slot, text, now + line_time });
        while (lines.size() > 50) {
            lines.pop_front();
        }
    }
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    bool chat_off = false;
    const bool y_down = chat_key_down(&chat_off) && !chat_off;
    const bool enter_down = keys[SDL_SCANCODE_RETURN] != 0 || keys[SDL_SCANCODE_KP_ENTER] != 0;
    const bool focused = SDL_GetKeyboardFocus() != nullptr;
    if (!open) {
        if (focused && y_down && !y_was_down && !recompui::is_context_capturing_input()) {
            open_box();
        }
    }
    else if (enter_down && !enter_was_down) {
        std::string text = typed;
        std::string command = text;
        command.erase(0, command.find_first_not_of(' '));
        while (!command.empty() && command.back() == ' ') {
            command.pop_back();
        }
        std::transform(command.begin(), command.end(), command.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
        if (command == "/network") {
            // answered here, not sent to the others
            for (const std::string& line : conker::netsession::network_report()) {
                lines.push_back({ -1, line, now + std::chrono::seconds(20) });
            }
        }
        else {
            const bool sent = !text.empty() && conker::netsession::chat_send(text);
            // (/bot offline, or as sent: applied here too; the others apply it as it reaches them)
            if (command.rfind("/bot", 0) == 0) {
                const std::string reply = conker::mp_characters::bot_command(text);
                lines.push_back({ -1, reply, now + std::chrono::seconds(10) });
            }
        }
        close_box();
    }
    else if (!focused) {
        close_box();
    }
    // While the box is up it must have the keyboard: the UI takes it away when the mouse moves, and the first try
    // right after the box is shown can come too early; so ask again until the box says it has it.
    if (open && recompui::is_context_shown(input_context) && !input_focused) {
        input_context.open();
        input->focus();
        input_context.close();
    }
    if (open && swallow_key && !y_down) {
        swallow_key = false;
        swallow_until_ms = now_ms() + 150;
    }
    if (open && clear_box) {
        clear_box = false;
        input_context.open();
        input->set_text("");
        input_context.close();
    }
    y_was_down = y_down;
    enter_was_down = enter_down;
    refresh_lines(now);
}
