// Text chat for an online game. Y (on the keyboard) opens a line to type on; Enter sends it (Enter on an empty line closes it).
// The lines of every player show in the bottom-left corner for a few seconds. While you type, your own controller is held
// still (net_session.cpp asks typing()), and for a moment after, so the Enter that sends the line isn't a press of Start.
//
// Two recompui contexts, made when first needed (as notices.cpp does): the lines (which take no input) and the typing box
// (which does, while it's up). The lines travel through the host (net_session.cpp's chat functions); nothing here touches the
// lockstep. Esc isn't used to close the box: the UI's own Esc opens the settings menu.

#include <algorithm>
#include <atomic>
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
    bool y_was_down = false, enter_was_down = false;
    std::string shown_signature;

    // The Online tab's Chat Key setting (the settings are made by add_options; without them, Y).
    enum class ChatKey : uint32_t { Y, T, V, B, N, M, Tab, Backtick, Backslash, Off };
    const std::string chat_key_option = "online_chat_key";

    SDL_Scancode chat_scancode(bool* off) {
        *off = false;
        uint32_t value = 0;
        if (recomp::config::Config* config = recompui::config::find_option_config(chat_key_option)) {
            value = std::get<uint32_t>(config->get_option_value(chat_key_option));
        }
        switch ((ChatKey)value) {
            case ChatKey::T: return SDL_SCANCODE_T;
            case ChatKey::V: return SDL_SCANCODE_V;
            case ChatKey::B: return SDL_SCANCODE_B;
            case ChatKey::N: return SDL_SCANCODE_N;
            case ChatKey::M: return SDL_SCANCODE_M;
            case ChatKey::Tab: return SDL_SCANCODE_TAB;
            case ChatKey::Backtick: return SDL_SCANCODE_GRAVE;
            case ChatKey::Backslash: return SDL_SCANCODE_BACKSLASH;
            case ChatKey::Off: *off = true; return SDL_SCANCODE_UNKNOWN;
            default: return SDL_SCANCODE_Y;
        }
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
            line_labels[i]->set_background_color(recompui::Color{ 12, 8, 4, 170 });
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
        bar->set_background_color(recompui::Color{ 12, 8, 4, 220 });
        input_context.create_element<recompui::Label>(bar, "Say:", recompui::LabelStyle::Normal);
        input = input_context.create_element<recompui::TextInput>(bar);
        input->set_flex_grow(1.0f);
        input->set_width(100.0f, recompui::Unit::Percent);
        input->add_text_changed_callback([](const std::string& text) { typed = text; });
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
                line_labels[i]->set_text(conker::netsession::player_name(line->slot) + ": " + line->text);
                line_labels[i]->set_color(player_color(line->slot));
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
        lines.push_back({ slot, text, now + line_time });
        while (lines.size() > 50) {
            lines.pop_front();
        }
    }
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    bool chat_off = false;
    const SDL_Scancode chat_scan = chat_scancode(&chat_off);
    const bool y_down = !chat_off && keys[chat_scan] != 0;
    const bool enter_down = keys[SDL_SCANCODE_RETURN] != 0 || keys[SDL_SCANCODE_KP_ENTER] != 0;
    const bool focused = SDL_GetKeyboardFocus() != nullptr;
    if (!open) {
        if (focused && y_down && !y_was_down && !recompui::is_context_capturing_input()) {
            open_box();
        }
    }
    else if (enter_down && !enter_was_down) {
        std::string text = typed;
        const bool sent = !text.empty() && conker::netsession::chat_send(text);
        (void)sent;
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
    y_was_down = y_down;
    enter_was_down = enter_down;
    refresh_lines(now);
}

// The Online tab of the settings.
void conker::netchat::add_options(recomp::config::Config& config) {
    using EnumOptions = const std::vector<recomp::config::ConfigOptionEnumOption>;
    static EnumOptions keys = {
        {ChatKey::Y, "Y", "Y"},
        {ChatKey::T, "T", "T"},
        {ChatKey::V, "V", "V"},
        {ChatKey::B, "B", "B"},
        {ChatKey::N, "N", "N"},
        {ChatKey::M, "M", "M"},
        {ChatKey::Tab, "Tab", "Tab"},
        {ChatKey::Backtick, "Backtick", "` (backtick)"},
        {ChatKey::Backslash, "Backslash", "\\ (backslash)"},
        {ChatKey::Off, "Off", "Off"},
    };
    config.add_enum_option(chat_key_option, "Chat: Key",
        "The keyboard key that opens the text chat in an online game. Pick one the controls don't use, as the key does what it's "
        "bound to as well. Enter sends the line you typed (on an empty line it closes the box). <recomp-color primary>Off</recomp-color> "
        "turns typing off; the others' lines still show.",
        keys, ChatKey::Y);
}
