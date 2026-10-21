// The lobby: the launcher's "Online" screen. One player hosts (the game's port and how many play), the others join with the
// host's address, the player list fills up, and when the host presses Start every machine starts the game. The session
// itself is net_session.cpp's (its lobby functions); this file is only the screen.
//
// It's a recompui context of its own over the launcher (as the assign-players modal is). Everything is driven from the
// launcher's update (update_launcher -> conker::netlobby::update), which runs on the UI thread every frame while the
// launcher is up: the buttons only queue an action, and the update does it.

#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

#include "recompui/recompui.h"
#include "elements/ui_button.h"
#include "elements/ui_container.h"
#include "elements/ui_label.h"
#include "elements/ui_text_input.h"
#include "elements/ui_theme.h"

#include "conker.hpp"

namespace {
    enum class Action { Show, Host, Join, Start, Leave, Back, Players, Delay, Input };

    std::mutex action_mutex;
    std::deque<Action> actions;

    void queue(Action a) {
        std::lock_guard lock{ action_mutex };
        actions.push_back(a);
    }

    // The settings, kept for the run.
    int players = 2;
    int delay_frames = 3;
    int input_choice = 0;
    std::string address_text;
    std::string port_text = "47000";

    recompui::ContextId context = recompui::ContextId::null();
    bool created = false;
    bool shown = false;
    bool in_lobby = false;          // the lobby page is up (the session is open or connecting)
    bool launched = false;
    recompui::Element* setup_page = nullptr;
    recompui::Element* lobby_page = nullptr;
    recompui::Button* players_button = nullptr;
    recompui::Button* delay_button = nullptr;
    recompui::Button* input_button = nullptr;
    recompui::Button* start_button = nullptr;
    recompui::Label* title_label = nullptr;
    recompui::Label* message_label = nullptr;
    recompui::Label* info_label = nullptr;
    recompui::Label* player_labels[4] = {};
    std::string last_signature;

    const char* input_name() {
        return input_choice == 1 ? "Controller: keyboard only" : input_choice == 2 ? "Controller: gamepad only" : "Controller: keyboard and gamepad";
    }

    void refresh_settings_buttons() {
        players_button->set_text("Players: " + std::to_string(players));
        delay_button->set_text("Input delay: " + std::to_string(delay_frames) + (delay_frames == 1 ? " frame" : " frames"));
        input_button->set_text(input_name());
    }

    recompui::Label* make_label(recompui::Element* parent, const std::string& text, recompui::LabelStyle style, float margin_bottom = 0.0f) {
        recompui::Label* label = context.create_element<recompui::Label>(parent, text, style);
        label->set_margin_bottom(margin_bottom);
        return label;
    }

    recompui::Button* make_button(recompui::Element* parent, const std::string& text, recompui::ButtonStyle style, Action action) {
        recompui::Button* button = context.create_element<recompui::Button>(parent, text, style, recompui::ButtonSize::Medium);
        button->add_pressed_callback([action]() { queue(action); });
        return button;
    }

    recompui::Container* make_row(recompui::Element* parent) {
        recompui::Container* row = context.create_element<recompui::Container>(parent, recompui::FlexDirection::Row, recompui::JustifyContent::FlexStart, 0);
        row->set_gap(12.0f);
        row->set_align_items(recompui::AlignItems::Center);
        row->set_width(100.0f, recompui::Unit::Percent);
        row->set_height_auto();
        return row;
    }

    // Runs f with the lobby's context open (the launcher's is open while its update runs: it's put back after).
    template <typename F> void with_context(F f) {
        recompui::ContextId previous = recompui::try_close_current_context();
        f();
        if (previous != recompui::ContextId::null()) {
            previous.open();
        }
    }

    void build() {
        context = recompui::create_context();
        context.open();

        recompui::Element* window = context.create_element<recompui::Element>(context.get_root_element());
        window->set_display(recompui::Display::Flex);
        window->set_flex_direction(recompui::FlexDirection::Column);
        window->set_background_color(recompui::theme::color::Transparent);

        recompui::Element* overlay = context.create_element<recompui::Element>(window);
        overlay->set_background_color(recompui::theme::color::BGOverlay);
        overlay->set_position(recompui::Position::Absolute);
        overlay->set_top(0);
        overlay->set_right(0);
        overlay->set_bottom(0);
        overlay->set_left(0);

        recompui::Element* wrapper = context.create_element<recompui::Element>(window);
        wrapper->set_display(recompui::Display::Flex);
        wrapper->set_position(recompui::Position::Absolute);
        wrapper->set_top(0);
        wrapper->set_right(0);
        wrapper->set_bottom(0);
        wrapper->set_left(0);
        wrapper->set_align_items(recompui::AlignItems::Center);
        wrapper->set_justify_content(recompui::JustifyContent::Center);

        recompui::Element* panel = context.create_element<recompui::Element>(wrapper);
        panel->set_display(recompui::Display::Flex);
        panel->set_position(recompui::Position::Relative);
        panel->set_flex_direction(recompui::FlexDirection::Column);
        panel->set_width(100, recompui::Unit::Percent);
        panel->set_max_width(760, recompui::Unit::Dp);
        panel->set_height_auto();
        panel->set_margin_auto();
        panel->set_padding(24.0f);
        panel->set_gap(14.0f);
        panel->set_border_width(recompui::theme::border::width);
        panel->set_border_radius(recompui::theme::border::radius_lg);
        panel->set_border_color(recompui::theme::color::Border);
        panel->set_background_color(recompui::theme::color::ModalOverlay);

        title_label = context.create_element<recompui::Label>(panel, "Online", recompui::theme::Typography::Header3);

        // --- the setup page: host or join ---
        setup_page = context.create_element<recompui::Element>(panel);
        setup_page->set_display(recompui::Display::Flex);
        setup_page->set_flex_direction(recompui::FlexDirection::Column);
        setup_page->set_width(100.0f, recompui::Unit::Percent);
        setup_page->set_height_auto();
        setup_page->set_gap(12.0f);

        make_label(setup_page, "To host: choose the settings and press Host. To join: type the host's address and press Join.", recompui::LabelStyle::Small);

        make_label(setup_page, "Host's address (to join)", recompui::LabelStyle::Small);
        recompui::TextInput* address = context.create_element<recompui::TextInput>(setup_page);
        address->set_width(100.0f, recompui::Unit::Percent);
        address->set_max_width(100.0f, recompui::Unit::Percent);
        address->set_text(address_text);
        address->add_text_changed_callback([](const std::string& text) { address_text = text; });

        make_label(setup_page, "Port (the same for everybody)", recompui::LabelStyle::Small);
        recompui::TextInput* port = context.create_element<recompui::TextInput>(setup_page);
        port->set_width(200.0f, recompui::Unit::Dp);
        port->set_text(port_text);
        port->add_text_changed_callback([](const std::string& text) { port_text = text; });

        recompui::Container* settings_row = make_row(setup_page);
        players_button = make_button(settings_row, "", recompui::ButtonStyle::Secondary, Action::Players);
        delay_button = make_button(settings_row, "", recompui::ButtonStyle::Secondary, Action::Delay);
        input_button = make_button(settings_row, "", recompui::ButtonStyle::Secondary, Action::Input);
        refresh_settings_buttons();
        make_label(setup_page, "Players and input delay are the host's choice. Controller is for this computer.", recompui::LabelStyle::Annotation);

        recompui::Container* setup_buttons = make_row(setup_page);
        recompui::Button* host_button = make_button(setup_buttons, "Host", recompui::ButtonStyle::Primary, Action::Host);
        make_button(setup_buttons, "Join", recompui::ButtonStyle::Primary, Action::Join);
        make_button(setup_buttons, "Back", recompui::ButtonStyle::Tertiary, Action::Back);
        context.set_autofocus_element(host_button);

        // --- the lobby page: the players ---
        lobby_page = context.create_element<recompui::Element>(panel);
        lobby_page->set_display(recompui::Display::None);
        lobby_page->set_flex_direction(recompui::FlexDirection::Column);
        lobby_page->set_width(100.0f, recompui::Unit::Percent);
        lobby_page->set_height_auto();
        lobby_page->set_gap(10.0f);

        message_label = make_label(lobby_page, "", recompui::LabelStyle::Normal);
        info_label = make_label(lobby_page, "", recompui::LabelStyle::Small);
        for (int i = 0; i < 4; i++) {
            player_labels[i] = make_label(lobby_page, "", recompui::LabelStyle::Large);
        }
        recompui::Container* lobby_buttons = make_row(lobby_page);
        start_button = make_button(lobby_buttons, "Start game", recompui::ButtonStyle::Success, Action::Start);
        make_button(lobby_buttons, "Leave", recompui::ButtonStyle::Tertiary, Action::Leave);

        context.close();
        created = true;
    }

    void show_page(bool lobby) {
        in_lobby = lobby;
        setup_page->set_display(lobby ? recompui::Display::None : recompui::Display::Flex);
        lobby_page->set_display(lobby ? recompui::Display::Flex : recompui::Display::None);
        title_label->set_text(lobby ? "Online lobby" : "Online");
        last_signature.clear();
    }

    void start_the_game(const std::u8string& game_id) {
        launched = true;
        shown = false;
        std::printf("[lobby] starting the game\n");
        std::fflush(stdout);
        recomp::start_game(game_id, {});
        recompui::hide_all_contexts();
    }

    // The lobby page, from the session's state.
    void refresh_lobby(const conker::netsession::LobbyStatus& st) {
        std::string signature = std::to_string(st.phase) + "|" + std::to_string(st.mask) + "|" + std::to_string(st.expected) + "|" +
            std::to_string(st.delay) + "|" + std::to_string(st.slot) + "|" + std::to_string(st.can_start) + "|" + st.message;
        if (signature == last_signature) {
            return;
        }
        last_signature = signature;
        std::string message;
        if (st.phase == 1) {
            message = st.message;
        }
        else if (st.phase == 3) {
            message = st.message.empty() ? "Something went wrong." : st.message;
        }
        else if (st.host) {
            message = "Hosting on port " + port_text + ". Tell the others your IP address and this port";
            message += st.can_start ? ". Everybody is here: press Start game." : " (your router has to forward it, TCP).";
        }
        else {
            message = "Joined. Waiting for the host to start the game.";
        }
        message_label->set_text(message);
        info_label->set_text(st.phase == 2 ? "Input delay: " + std::to_string(st.delay) + (st.delay == 1 ? " frame" : " frames") + " (the host chose it)" : std::string());
        for (int i = 0; i < 4; i++) {
            std::string line;
            if (i < st.expected && st.phase == 2) {
                line = "Player " + std::to_string(i + 1);
                line += i == 0 ? " (host)" : "";
                line += i == st.slot ? " (you)" : "";
                line += ((st.mask >> i) & 1) ? "  -  here" : "  -  waiting for a player...";
            }
            player_labels[i]->set_text(line);
            player_labels[i]->set_display(line.empty() ? recompui::Display::None : recompui::Display::Block);
        }
        start_button->set_display(st.host ? recompui::Display::Flex : recompui::Display::None);
        start_button->set_enabled(st.can_start);
    }
}

void conker::netlobby::show() {
    queue(Action::Show);
}

void conker::netlobby::update(const std::u8string& game_id) {
    if (launched) {
        return;
    }
    std::deque<Action> todo;
    {
        std::lock_guard lock{ action_mutex };
        todo.swap(actions);
    }
    if (todo.empty() && !shown) {
        return;
    }
    with_context([&]() {
        if (shown && !recompui::is_context_shown(context)) {
            // closed from outside (the UI's back button): the lobby is left too
            conker::netsession::lobby_leave();
            shown = false;
        }
        for (Action a : todo) {
            if (a == Action::Show) {
                if (!recomp::is_rom_valid(game_id)) {
                    recompui::message_box("Load the game's ROM first: choose Load ROM (the first option) and pick it.");
                    continue;
                }
                if (!created) {
                    build();
                }
                context.open();
                show_page(false);
                context.close();
                if (!recompui::is_context_shown(context)) {
                    recompui::show_context(context, "");
                }
                shown = true;
                continue;
            }
            if (!created || !shown) {
                continue;
            }
            context.open();
            switch (a) {
                case Action::Players:
                    players = players >= 4 ? 2 : players + 1;
                    refresh_settings_buttons();
                    break;
                case Action::Delay: {
                    static const int choices[] = { 2, 3, 4, 5, 6, 8, 10 };
                    int next = choices[0];
                    for (int c : choices) {
                        if (c > delay_frames) {
                            next = c;
                            break;
                        }
                    }
                    delay_frames = next;
                    refresh_settings_buttons();
                    break;
                }
                case Action::Input:
                    input_choice = (input_choice + 1) % 3;
                    refresh_settings_buttons();
                    break;
                case Action::Host:
                case Action::Join: {
                    conker::netsession::LobbySettings settings;
                    settings.host = a == Action::Host;
                    settings.address = address_text;
                    settings.port = std::atoi(port_text.c_str());
                    if (settings.port < 1 || settings.port > 65535) {
                        settings.port = 47000;
                    }
                    settings.players = players;
                    settings.delay = delay_frames;
                    settings.input = input_choice;
                    conker::netsession::lobby_open(settings);
                    show_page(true);
                    break;
                }
                case Action::Start:
                    if (conker::netsession::lobby_start()) {
                        context.close();
                        start_the_game(game_id);
                        context.open();
                    }
                    break;
                case Action::Leave:
                    conker::netsession::lobby_leave();
                    show_page(false);
                    break;
                case Action::Back:
                    conker::netsession::lobby_leave();
                    recompui::hide_context(context);
                    shown = false;
                    break;
                default:
                    break;
            }
            context.close();
        }
        if (shown && !launched && in_lobby) {
            const conker::netsession::LobbyStatus st = conker::netsession::lobby_status();
            if (st.launch) {
                start_the_game(game_id);
                return;
            }
            context.open();
            refresh_lobby(st);
            context.close();
        }
    });
}
