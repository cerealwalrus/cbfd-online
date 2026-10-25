// The window build's front end: RecompFrontend's launcher, settings and mod menus
// (recompui) and remappable keyboard/controller input (recompinput), on an SDL
// window rendered by RT64 through recompui's renderer.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include <SDL.h>
#if defined(_WIN32) || defined(__APPLE__)
// Only Windows and macOS need the native window handle. On Linux this header brings
// in X11's, whose None macro breaks ultramodern's Device::None.
#include <SDL_syswm.h>
#endif

#include "nfd.h"

#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "recompinput/input_binding.h"
#include "recompinput/input_events.h"
#include "recompinput/input_state.h"
#include "recompinput/players.h"
#include "recompinput/profiles.h"
#include "recompui/program_config.h"
#include "recompui/recompui.h"
#include "recompui/renderer.h"
#include "base/ui_launcher.h"
#include "base/ui_game_option.h"
#include "elements/ui_label.h"
#include "elements/ui_theme.h"
#include "util/file.h"

#include "conker.hpp"

// recompui's launcher shows the first entry (ui_launcher.cpp declares it extern).
std::vector<recomp::GameEntry> supported_games;
// The game window, which recompui also uses (ui_state.cpp declares it extern).
SDL_Window* window = nullptr;

void conker_mouse_camera_init();

namespace {
    std::vector<char> thumbnail;

    // Each poll also tells the look mode that the next mouse and gyro movement is in.
    void poll_inputs() {
        recompinput::poll_inputs();
        conker::look_aim::on_input_poll();
    }

    // The launcher's Version and Add ROM options, and the window title that names the
    // version in play. The title is set on the main thread (update_gfx), as macOS requires.
    recompui::GameOption* version_option = nullptr;
    recompui::GameOption* add_rom_option = nullptr;
    std::mutex title_mutex;
    std::string pending_title;

    // Prints what SDL says about a controller (name, GUID, mapping, device), for controller reports.
    void print_controller(int index) {
        char guid[64];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
        const char* name = SDL_GameControllerNameForIndex(index);
        const char* path = SDL_GameControllerPathForIndex(index);
        char* mapping = SDL_GameControllerMappingForDeviceIndex(index);
        std::printf("[controller] connected: %s (GUID %s, device %s)\n  mapping: %s\n", name ? name : "?", guid,
            path ? path : "?", mapping ? mapping : "none");
        SDL_free(mapping);
    }

    // The controllers connected, each opened here too (SDL counts its opens, so recompinput's own
    // stays as it is), by instance ID. Only the main thread changes this, as SDL announces a
    // controller connecting or disconnecting (watch_controllers) and at start (create_gfx); the
    // other threads read it (get_port_controllers), so none of them asks SDL for its devices.
    //
    // Issue #88: macOS crashed when a controller connected while playing. The Mac build's SDL2 is
    // sdl2-compat, SDL2's functions over SDL3, and it keeps one list of devices for the index-based
    // calls: SDL_NumJoysticks frees it and fetches it again, without a lock around the two. As a
    // DualSense connected, sdl2-compat refreshed it on the main thread for the event while the VI
    // thread's rumble listed the controllers (SDL_NumJoysticks, for the port controllers), and the
    // list was freed twice. Listing them only on the main thread, as SDL announces each, they're
    // never listed at once. A controller's buttons, sticks and rumble are safe from any thread.
    std::mutex open_controllers_mutex;
    std::vector<std::pair<SDL_JoystickID, SDL_GameController*>> open_controllers;

    // Online (or with CONKER_IGNORE_DIRECTINPUT=1) only XInput controllers are used: a pad Windows lists a second time as a
    // DirectInput device (or one that is only that) is left out, so it can't be a second controller or take the place of
    // another. CONKER_ALLOW_DIRECTINPUT=1 keeps them.
    bool ignore_directinput() {
        static const bool value = []() {
            if (SDL_getenv("CONKER_ALLOW_DIRECTINPUT") != nullptr) {
                return false;
            }
            const char* ignore = SDL_getenv("CONKER_IGNORE_DIRECTINPUT");
            return (ignore != nullptr && std::atoi(ignore) != 0) || SDL_getenv("CONKER_NET_HOST") != nullptr || SDL_getenv("CONKER_NET_JOIN") != nullptr;
        }();
        return value;
    }

    // From the main thread: the controller at device index, if it's one SDL has a mapping for and
    // it isn't in the list yet.
    void open_controller(int index) {
        if (!SDL_IsGameController(index)) {
            return;
        }
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
        {
            std::lock_guard lock{ open_controllers_mutex };
            for (const auto& [open_id, controller] : open_controllers) {
                if (open_id == id) {
                    return;
                }
            }
        }
        SDL_GameController* controller = SDL_GameControllerOpen(index);
        if (controller == nullptr) {
            std::printf("[controller] couldn't open device %d: %s\n", index, SDL_GetError());
            return;
        }
        if (ignore_directinput() && SDL_GameControllerGetPlayerIndex(controller) < 0) {
            std::printf("[controller] ignoring %s (not an XInput controller)\n", SDL_GameControllerName(controller) != nullptr ? SDL_GameControllerName(controller) : "a controller");
            std::fflush(stdout);
            SDL_GameControllerClose(controller);
            return;
        }
        std::lock_guard lock{ open_controllers_mutex };
        open_controllers.emplace_back(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)), controller);
    }

    // From the main thread: the controller with that instance ID leaves the list.
    void close_controller(SDL_JoystickID id) {
        SDL_GameController* closing = nullptr;
        {
            std::lock_guard lock{ open_controllers_mutex };
            for (auto it = open_controllers.begin(); it != open_controllers.end(); ++it) {
                if (it->first == id) {
                    closing = it->second;
                    open_controllers.erase(it);
                    break;
                }
            }
        }
        if (closing != nullptr) {
            SDL_GameControllerClose(closing);
        }
    }

    // Watches controllers connecting: each is printed, opened into the list above, and its C-buttons
    // remapped if they need it (pad_mappings.cpp). SDL calls this on the thread that announces the
    // device, the main one, as it pumps events, so the devices are listed there.
    // The launcher's menus answer the mouse only: every window on a computer receives the game controller's buttons, so a
    // press while playing in another window pressed this one's buttons (the dedicated server stopped by itself in a test).
    // Set each frame: a menu is up and no game is running in this window.
    std::atomic<bool> menus_mouse_only = false;

    int SDLCALL drop_pad_menu_events(void*, SDL_Event* event) {
        if (menus_mouse_only.load(std::memory_order_relaxed)) {
            switch (event->type) {
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
            case SDL_CONTROLLERAXISMOTION:
            case SDL_JOYBUTTONDOWN:
            case SDL_JOYBUTTONUP:
            case SDL_JOYHATMOTION:
            case SDL_JOYAXISMOTION:
                return 0;
            }
        }
        return 1;
    }

    int SDLCALL watch_controllers(void*, SDL_Event* event) {
        switch (event->type) {
        case SDL_JOYDEVICEADDED:
            conker::pad_mappings::fix_all();
            break;
        case SDL_CONTROLLERDEVICEADDED:
            print_controller(event->cdevice.which);
            open_controller(event->cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            close_controller(event->cdevice.which);
            break;
        }
        std::fflush(stdout);
        return 1;
    }

    void* create_gfx() {
        SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (ignore_directinput()) {
            SDL_SetHint("SDL_JOYSTICK_DIRECTINPUT", "0");   // SDL's DirectInput driver off: XInput pads stay
        }
#if defined(__linux__)
        // Nintendo's online classic controllers (the N64 one, and pads like the 8BitDo 64 in its
        // Switch mode) through the kernel's driver, not SDL's HIDAPI one, so there's one layout for
        // pad_mappings.cpp to make the C-buttons the right stick of (issue #28): SDL3, under Linux
        // distributions' sdl2-compat, maps both as a Switch pad, and HIDAPI's puts one C-button on
        // an axis. Windows keeps SDL's default: its SDL is ours (2.26), and without HIDAPI the pad
        // may have no mapping there. The SDL_JOYSTICK_HIDAPI_NINTENDO_CLASSIC environment variable
        // still overrides this.
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_NINTENDO_CLASSIC, "0");
#endif
        // Debugging aid: CONKER_NO_CONTROLLER=1 ignores game controllers, e.g. for test
        // runs while someone else is playing with the controller on the same machine.
        Uint32 subsystems = SDL_INIT_VIDEO;
        if (SDL_getenv("CONKER_NO_CONTROLLER") == nullptr) {
            subsystems |= SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC;
        }
        if (SDL_Init(subsystems) != 0) {
            std::fprintf(stderr, "[frontend] SDL_Init failed: %s\n", SDL_GetError());
        }
        SDL_SetEventFilter(drop_pad_menu_events, nullptr);
        SDL_version sdl_version;
        SDL_GetVersion(&sdl_version);
        std::printf("[frontend] SDL %d.%d.%d\n", sdl_version.major, sdl_version.minor, sdl_version.patch);
        // N64 pads and adapters SDL has no mapping for, or maps as other pads (assets/controllerdb.txt).
        // Only mappings, so the controllers connected at start are picked up when they're opened.
        const std::u8string controller_db = recompui::file::get_asset_path("controllerdb.txt").u8string();
        const int controller_mappings = SDL_GameControllerAddMappingsFromFile(reinterpret_cast<const char*>(controller_db.c_str()));
        if (controller_mappings < 0) {
            std::fprintf(stderr, "[frontend] couldn't load the controller mappings: %s\n", SDL_GetError());
        } else {
            std::printf("[frontend] controller mappings: %d added\n", controller_mappings);
        }
        SDL_AddEventWatch(watch_controllers, nullptr);
        // The controllers connected at start were announced before the watch.
        for (int index = 0; index < SDL_NumJoysticks(); index++) {
            print_controller(index);
            open_controller(index);
        }
        conker::pad_mappings::fix_all();
        // The file dialogs (Load ROM, mods). Only after SDL: on macOS, NFD_Init creates the
        // application object if it doesn't exist yet and makes it an accessory app, and SDL
        // then leaves it that way (no Dock icon, and the window opens behind the terminal).
        NFD_Init();
        // The mouse camera's scroll wheel zoom (mouse_camera.cpp).
        conker_mouse_camera_init();
        // Save states' keys, and the runtime's idle moment they're taken at (save_states.cpp).
        conker::save_states::init();
        return nullptr;
    }

    ultramodern::renderer::WindowHandle create_window(void*) {
        uint32_t flags = SDL_WINDOW_RESIZABLE;
#if defined(RT64_SDL_WINDOW_VULKAN)
        flags |= SDL_WINDOW_VULKAN;
#elif defined(__APPLE__)
        flags |= SDL_WINDOW_METAL;
#endif
        const int width = conker::frontend::window_width > 0 ? conker::frontend::window_width : 1600;
        const int height = conker::frontend::window_height > 0 ? conker::frontend::window_height : 900;
        window = SDL_CreateWindow(conker::window_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            width, height, flags);
        if (window == nullptr) {
            std::fprintf(stderr, "[frontend] SDL_CreateWindow failed: %s\n", SDL_GetError());
            return {};
        }
#if defined(_WIN32)
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        SDL_GetWindowWMInfo(window, &info);
        return ultramodern::renderer::WindowHandle{ info.info.win.window, GetCurrentThreadId() };
#elif defined(__APPLE__)
        // RT64 renders with Metal into the CAMetalLayer of a view added to the window.
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        SDL_GetWindowWMInfo(window, &info);
        SDL_MetalView view = SDL_Metal_CreateView(window);
        return ultramodern::renderer::WindowHandle{ info.info.cocoa.window, SDL_Metal_GetLayer(view) };
#else
        return ultramodern::renderer::WindowHandle{ window };
#endif
    }

    void update_gfx(void*) {
        // (not while a control is being bound: that takes the pad's buttons)
        menus_mouse_only = recompui::is_any_context_shown() && !ultramodern::is_game_started() && !recompinput::binding::is_binding();
        recompinput::handle_events();
        conker::texture_packs::update_unpacking();
        conker::fps_counter::update();
        conker::notices::update();
        conker::netchat::update();
        conker::reticle::update();
        std::string title;
        {
            std::lock_guard lock(title_mutex);
            title.swap(pending_title);
        }
        if (!title.empty() && window != nullptr) {
            SDL_SetWindowTitle(window, title.c_str());
        }
    }

    // Shows which version of the ROM is in play: on the launcher's Version option and in
    // the window title. Version and Add ROM are hidden until there's a ROM (Start Game's
    // Load ROM picks the first).
    void show_version() {
        std::string name = conker::roms::current_name();
        for (recompui::GameOption* option : { version_option, add_rom_option }) {
            if (option != nullptr) {
                if (name.empty()) {
                    option->display_hide();
                }
                else {
                    option->display_show();
                }
            }
        }
        if ((version_option != nullptr) && !name.empty()) {
            version_option->set_title("Version: " + name);
            // with one version loaded there's nothing to switch to: the option can't be clicked
            version_option->set_enabled(conker::roms::version_count() >= 2);
        }
        std::printf("[frontend] ROM version in play: %s (%zu kept)\n", name.empty() ? "none" : name.c_str(),
            conker::roms::version_count());
        std::lock_guard lock(title_mutex);
        // (the ROM version in play is on the Version option, not in the title)
        pending_title = conker::window_title;
    }

    // The Version option: switches to the next version kept.
    void on_version_selected() {
        if (conker::roms::version_count() < 2) {
            return;   // (nothing to switch to; the option is disabled then, see show_version)
        }
        if (conker::roms::switch_to_next()) {
            show_version();
        }
    }

    // The Add ROM option: loads another ROM, which is kept as a version and put in play.
    void on_add_rom_selected() {
        recompui::file::open_file_dialog([](bool success, const std::filesystem::path& path) {
            if (!success) {
                return;
            }
            recomp::RomValidationError result = recomp::select_rom(path, supported_games[0].game_id);
            if (result == recomp::RomValidationError::IncorrectVersion) {
                // Conker's Bad Fur Day, but not a US ROM the game plays: say which region it is.
                std::string region = conker::roms::region_of(path);
                if (!region.empty() && region != "US") {
                    std::string text = "This is the " + region + " version of Conker's Bad Fur Day. Only the US "
                        "version is supported (and ROM hacks of it that only change the game's assets).";
                    recompui::message_box(text.c_str());
                    return;
                }
            }
            if (result != recomp::RomValidationError::Good) {
                recompui::message_box(conker::rom_error_text(result));
            }
            // The launcher's update picks the new version up once it's written.
        });
    }

    // The look of the game's own menus: dark wood, grey lettering, orange for what is selected. Applied before the UI is
    // built (the colours are read as each element is made). CONKER_THEME=default keeps RecompFrontend's blue look.
    bool conker_theme_on() {
        const char* value = SDL_getenv("CONKER_THEME");
        return value == nullptr || std::string(value) != "default";
    }

    void apply_conker_theme() {
        if (!conker_theme_on()) {
            return;
        }
        using recompui::Color;
        namespace theme = recompui::theme;
        const auto set = [](theme::color c, Color v) { theme::set_theme_color(c, v); };
        const auto with_alphas = [&](theme::color base, theme::color a5, theme::color a20, theme::color a30, theme::color a50, theme::color a80, uint8_t r, uint8_t g, uint8_t b) {
            set(base, Color{ r, g, b, 255 });
            set(a5, Color{ r, g, b, 13 });
            set(a20, Color{ r, g, b, 51 });
            set(a30, Color{ r, g, b, 77 });
            set(a50, Color{ r, g, b, 128 });
            set(a80, Color{ r, g, b, 204 });
        };
        set(theme::color::Background1, Color{ 34, 24, 16, 255 });
        set(theme::color::Background2, Color{ 46, 33, 22, 255 });
        set(theme::color::Background3, Color{ 58, 42, 28, 255 });
        set(theme::color::BGOverlay, Color{ 12, 8, 4, 170 });
        set(theme::color::ModalOverlay, Color{ 42, 29, 19, 240 });
        set(theme::color::Elevated, Color{ 96, 68, 40, 255 });
        set(theme::color::Border, Color{ 22, 14, 8, 255 });
        set(theme::color::Text, Color{ 224, 220, 210, 255 });
        set(theme::color::TextActive, Color{ 255, 226, 90, 255 });
        set(theme::color::TextDim, Color{ 204, 198, 186, 255 });
        with_alphas(theme::color::Primary, theme::color::PrimaryA5, theme::color::PrimaryA20, theme::color::PrimaryA30,
            theme::color::PrimaryA50, theme::color::PrimaryA80, 232, 128, 20);
        set(theme::color::PrimaryL, Color{ 255, 204, 96, 255 });
        set(theme::color::PrimaryD, Color{ 150, 72, 6, 255 });
        set(theme::color::SecondaryL, Color{ 255, 232, 96, 255 });
        // Lilita One has the one weight: no bold to ask for
        theme::set_typography_preset(theme::Typography::Header1, 68.0f, 0.04f, 400);
        theme::set_typography_preset(theme::Typography::Header2, 52.0f, 0.04f, 400);
        theme::set_typography_preset(theme::Typography::Header3, 40.0f, 0.04f, 400);
        theme::set_typography_preset(theme::Typography::LabelLG, 40.0f, 0.05f, 400);
        theme::set_typography_preset(theme::Typography::LabelMD, 30.0f, 0.05f, 400);
        theme::set_typography_preset(theme::Typography::LabelSM, 22.0f, 0.06f, 400);
        theme::set_typography_preset(theme::Typography::LabelXS, 19.0f, 0.06f, 400);
        theme::set_typography_preset(theme::Typography::Body, 21.0f, 0.03f, 400);
    }

    // A launcher option as one of the game's wooden planks: dark wood, grey capitals, lighter wood and yellow lettering
    // when it's the one selected (the pulse between orange and yellow is RecompFrontend's own).
    void style_plank(recompui::GameOption* option, int index) {
        using recompui::Color;
        option->set_background_color(Color{ 66, 46, 27, 255 });
        option->set_border_width(3.0f);
        option->set_border_color(Color{ 20, 12, 6, 255 });
        option->set_border_radius(7.0f);
        option->set_padding(10.0f);
        option->set_margin_bottom(8.0f);
        option->set_color(Color{ 224, 220, 210, 255 });
        option->set_rotation((index % 2 == 0) ? -0.6f : 0.6f);
        option->hover_style.set_background_color(Color{ 104, 74, 43, 255 });
        option->hover_style.set_color(Color{ 255, 226, 90, 255 });
        option->focus_style.set_background_color(Color{ 112, 80, 46, 255 });
        option->focus_style.set_color(Color{ 255, 226, 90, 255 });
        if (recompui::Label* label = option->get_label()) {
            label->set_text_transform(recompui::TextTransform::Uppercase);
            label->set_font_size(44.0f);
            label->set_letter_spacing(2.0f);
        }
    }

    // The launcher's options: RecompFrontend's usual ones, with Version and Add ROM after Start Game.
    void init_launcher(recompui::LauncherMenu* menu) {
        const recomp::GameEntry& game = supported_games[0];
        // Here rather than at startup, so a ROM given with --rom is already the one stored.
        conker::roms::init(recomp::get_config_path() / game.stored_filename());
        recompui::GameOptionsMenu* options = menu->init_game_options_menu(
            game.game_id, game.mod_game_id, game.display_name, game.thumbnail_bytes);
        // Lower than recompui's 25% from the bottom: with Version and Add ROM, seven options
        // would reach up into the title (Online makes eight).
        options->set_bottom(5.0f, recompui::Unit::Percent);
        recompui::update_game_mod_id(game.mod_game_id);
        // The runtime has opened the mods by now: turn on the texture pack chosen in the settings.
        conker::texture_packs::apply();
        // And unpack the GLideN64 packs not unpacked yet, showing the progress over the launcher.
        conker::texture_packs::unpack_gliden64_packs();
        // recompui's UI exists now: the FPS counter and the aiming reticle can make their own.
        conker::fps_counter::on_ui_ready();
        conker::notices::on_ui_ready();
        conker::local_achievements::init();
        conker::reticle::on_ui_ready();
        options->add_start_game_or_load_rom_option();
        options->add_option("Online", []() { conker::netlobby::show(); });
        version_option = options->add_option("Version", on_version_selected);
        add_rom_option = options->add_option("Add ROM", on_add_rom_selected);
        options->add_setup_controls_option();
        options->add_settings_option();
        options->add_mods_option();
        options->add_exit_option();
        if (conker_theme_on()) {
            int index = 0;
            for (recompui::GameOption* option : options->get_options()) {
                style_plank(option, index++);
            }
        }
        conker::menu_skin::apply(menu, options, { version_option, add_rom_option });
        show_version();
    }

    void update_launcher(recompui::LauncherMenu*) {
        if (conker::roms::update()) {
            show_version();
        }
        conker::menu_skin::update();
        conker::netlobby::update(supported_games[0].game_id);
    }

    // Wraps the real renderer: once it has read a task's display list, the words the view rewrote in it are restored.
    class RewriteRestoringRenderer : public ultramodern::renderer::RendererContext {
    public:
        RewriteRestoringRenderer(std::unique_ptr<ultramodern::renderer::RendererContext> real_renderer, uint8_t* ram)
            : real(std::move(real_renderer)), rdram(ram) {
            setup_result = real->get_setup_result();
            chosen_api = real->get_chosen_api();
        }
        bool valid() override { return real->valid(); }
        ultramodern::renderer::SetupResult get_setup_result() const override { return real->get_setup_result(); }
        ultramodern::renderer::GraphicsApi get_chosen_api() const override { return real->get_chosen_api(); }
        bool update_config(const ultramodern::renderer::GraphicsConfig& a, const ultramodern::renderer::GraphicsConfig& b) override {
            return real->update_config(a, b);
        }
        void enable_instant_present() override { real->enable_instant_present(); }
        void send_dl(const OSTask* task) override {
            real->send_dl(task);
            conker::netview::render_done(rdram, (uint32_t)task->t.data_ptr);
        }
        void send_dummy_workload(uint32_t fb_address) override { real->send_dummy_workload(fb_address); }
        void update_screen() override { real->update_screen(); }
        void shutdown() override { real->shutdown(); }
        uint32_t get_display_framerate() const override { return real->get_display_framerate(); }
        float get_resolution_scale() const override { return real->get_resolution_scale(); }
    private:
        std::unique_ptr<ultramodern::renderer::RendererContext> real;
        uint8_t* rdram;
    };

    std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
        uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
        auto real = recompui::renderer::create_render_context(rdram, window_handle,
            ultramodern::renderer::PresentationMode::PresentEarly,
            // TEMP-DEBUG: CONKER_DEV_MODE turns on RT64's developer tools (F1: inspector).
            developer_mode || SDL_getenv("CONKER_DEV_MODE") != nullptr);
        return std::make_unique<RewriteRestoringRenderer>(std::move(real), rdram);
    }

}

void conker::frontend::on_vi() {
    conker::rumble::update();
}

// Controller ports. recompinput's single-player mode reports all four ports as plugged
// in and gives every port the merged input of every device, so on the multiplayer
// screen one Start press joined players 2-4 as well. Instead, each controller gets its
// own port, in the order the controllers first press a button (Windows lists them in
// its own order, not the order they were turned on): the first to press one is player
// 1, with the keyboard, the next player 2, and so on. A controller that disconnects
// gives up its port, and the ones after it move up. As many ports as there are
// controllers report as plugged in (at least one), so the game sees player 2's port
// from the start; it gets that controller's input once it presses a button. Every
// controller uses the single-player controller bindings.
namespace {
    constexpr int max_ports = 4;

    std::mutex port_mutex;
    // The controllers holding ports 1-4, in the order they first pressed a button.
    std::vector<SDL_JoystickID> port_order;

    bool controller_any_button(SDL_GameController* controller) {
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
            if (SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)b)) {
                return true;
            }
        }
        // The triggers are axes; the sticks are left out, so a drifting stick doesn't claim a port.
        return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16384 ||
            SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384;
    }

    // Updates the port order and fills `out` with the controllers holding ports, in port
    // order. Returns how many hold ports; `connected` gets how many controllers are open.
    int get_port_controllers(std::array<SDL_GameController*, max_ports>& out, int* connected = nullptr) {
        std::lock_guard lock{ port_mutex };
        // The controllers connected, from the main thread's list (open_controllers): this runs on
        // the game's threads (input, rumble), where SDL's own device list isn't safe to read (#88).
        std::vector<std::pair<SDL_JoystickID, SDL_GameController*>> open;
        {
            std::lock_guard open_lock{ open_controllers_mutex };
            open = open_controllers;
        }
        auto find_open = [&](SDL_JoystickID id) -> SDL_GameController* {
            for (const auto& [open_id, controller] : open) {
                if (open_id == id) {
                    return controller;
                }
            }
            return nullptr;
        };
        // Disconnected controllers give up their ports.
        std::erase_if(port_order, [&](SDL_JoystickID id) { return find_open(id) == nullptr; });
        // Controllers pressing a button for the first time take the next port.
        for (const auto& [id, controller] : open) {
            if ((int)port_order.size() < max_ports && std::find(port_order.begin(), port_order.end(), id) == port_order.end() &&
                controller_any_button(controller)) {
                port_order.push_back(id);
            }
        }
        int count = 0;
        for (SDL_JoystickID id : port_order) {
            out[count++] = find_open(id);
        }
        if (connected != nullptr) {
            *connected = std::min((int)open.size(), max_ports);
        }
        return count;
    }

    // The stick axis read for an axis bound: with swap (Aiming: Swap Sticks, issue #84), the left stick's
    // axes are read from the right stick and the right's from the left, so whatever the profile binds to
    // either stick (by default the N64 stick to the left, the C-buttons to the right) moves to the other.
    int stick_axis(int axis, bool swap) {
        if (!swap) {
            return axis;
        }
        switch (axis) {
        case SDL_CONTROLLER_AXIS_LEFTX: return SDL_CONTROLLER_AXIS_RIGHTX;
        case SDL_CONTROLLER_AXIS_LEFTY: return SDL_CONTROLLER_AXIS_RIGHTY;
        case SDL_CONTROLLER_AXIS_RIGHTX: return SDL_CONTROLLER_AXIS_LEFTX;
        case SDL_CONTROLLER_AXIS_RIGHTY: return SDL_CONTROLLER_AXIS_LEFTY;
        default: return axis;
        }
    }

    float controller_field_analog(SDL_GameController* controller, const recompinput::InputField& field, bool swap = false) {
        switch (field.input_type) {
        case recompinput::InputType::ControllerDigital:
            if (field.input_id >= 0 && field.input_id < SDL_CONTROLLER_BUTTON_MAX) {
                return SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)field.input_id) ? 1.0f : 0.0f;
            }
            return 0.0f;
        case recompinput::InputType::ControllerAnalog: {
            int axis = std::abs(field.input_id) - 1;
            if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX) {
                return 0.0f;
            }
            float value = SDL_GameControllerGetAxis(controller, (SDL_GameControllerAxis)stick_axis(axis, swap)) * (1.0f / 32768.0f);
            if (field.input_id < 0) {
                value = -value;
            }
            return std::clamp(value, 0.0f, 1.0f);
        }
        default:
            return 0.0f;
        }
    }

    bool controller_field_digital(SDL_GameController* controller, const recompinput::InputField& field, bool swap = false) {
        if (field.input_type == recompinput::InputType::ControllerAnalog) {
            return controller_field_analog(controller, field, swap) >= recompinput::axis_digital_threshold;
        }
        return controller_field_analog(controller, field, swap) > 0.0f;
    }

    // The keyboard and the mouse are player 1's: their bindings count on port 1 only. Mouse buttons
    // can be bound with the keyboard's controls or, in single player, the controller's.
    bool is_keyboard_or_mouse(const recompinput::InputField& field) {
        return field.input_type == recompinput::InputType::Keyboard || field.input_type == recompinput::InputType::Mouse;
    }

    // A binding to the right stick (either axis, either way).
    bool is_right_stick(const recompinput::InputField& field) {
        if (field.input_type != recompinput::InputType::ControllerAnalog) {
            return false;
        }
        const int axis = std::abs(field.input_id) - 1;
        return axis == SDL_CONTROLLER_AXIS_RIGHTX || axis == SDL_CONTROLLER_AXIS_RIGHTY;
    }

    // One controller (or none) and/or the keyboard and mouse, through the single-player bindings.
    // With free_stick (Right Stick: Free Camera, while the stick turns the camera), the controller's
    // right stick presses no button: it turns the camera instead (mouse_camera.cpp). With swap_sticks
    // (Aiming: Swap Sticks, while aiming), the controller's two sticks are read the other way round.
    void read_port(SDL_GameController* controller, bool keyboard, bool free_stick, bool swap_sticks, bool z_turns_aim, uint16_t* buttons, float* x, float* y) {
        using recompinput::GameInput;
        static constexpr uint16_t button_values[] = {
            0x8000, 0x4000, 0x2000, 0x0020, 0x0010, 0x1000, 0x0008,
            0x0004, 0x0002, 0x0001, 0x0800, 0x0400, 0x0200, 0x0100,
        };
        // A..C Right, then the D-pad (N64_BUTTON_COUNT stops at C Right).
        static_assert(std::size(button_values) == (size_t)GameInput::DPAD_RIGHT - (size_t)GameInput::N64_BUTTON_START + 1);
        const int cont_profile = recompinput::profiles::get_sp_controller_profile_index();
        const int kb_profile = recompinput::profiles::get_sp_keyboard_profile_index();
        auto binding = [](int profile, GameInput input, size_t i) -> const recompinput::InputField& {
            return recompinput::profiles::get_input_binding(profile, input, i);
        };
        auto cont_analog = [&](GameInput input) {
            float v = 0.0f;
            for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
                const recompinput::InputField& field = binding(cont_profile, input, i);
                if (field.input_type == recompinput::InputType::Mouse) {
                    v += keyboard ? recompinput::get_input_analog(0, field) : 0.0f;
                }
                else if (controller != nullptr) {
                    v += controller_field_analog(controller, field, swap_sticks);
                }
            }
            return std::clamp(v, 0.0f, 1.0f);
        };
        auto kb_analog = [&](GameInput input) {
            float v = 0.0f;
            for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
                const recompinput::InputField& field = binding(kb_profile, input, i);
                if (is_keyboard_or_mouse(field)) {
                    v += recompinput::get_input_analog(0, field);
                }
            }
            return std::clamp(v, 0.0f, 1.0f);
        };

        uint16_t cur_buttons = 0;
        // With swap_sticks, the buttons pressed only by the left stick (bindings to the right stick, read
        // from the left): see below.
        uint16_t left_stick_buttons = 0;
        float cur_x = 0.0f;
        float cur_y = 0.0f;
        for (size_t b = 0; b < std::size(button_values); b++) {
            GameInput input = (GameInput)((size_t)GameInput::N64_BUTTON_START + b);
            bool pressed = false;
            bool by_left_stick = false;
            for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
                if (cont_profile >= 0) {
                    const recompinput::InputField& field = binding(cont_profile, input, i);
                    if (field.input_type == recompinput::InputType::Mouse) {
                        pressed |= keyboard && recompinput::get_input_digital(0, field);
                    }
                    else if (controller != nullptr && !(free_stick && is_right_stick(field))) {
                        const bool down = controller_field_digital(controller, field, swap_sticks);
                        if (swap_sticks && is_right_stick(field)) {
                            by_left_stick |= down;
                        }
                        else {
                            pressed |= down;
                        }
                    }
                }
                if (keyboard && kb_profile >= 0) {
                    const recompinput::InputField& field = binding(kb_profile, input, i);
                    if (is_keyboard_or_mouse(field)) {
                        pressed |= recompinput::get_input_digital(0, field);
                    }
                }
            }
            if (pressed) {
                cur_buttons |= button_values[b];
            }
            else if (by_left_stick) {
                left_stick_buttons |= button_values[b];
            }
        }
        // Aiming: Swap Sticks: the left stick presses the C-buttons, which move Conker while aiming, but not
        // C-Left and C-Right while Z is held aiming the shotgun (issue #84, z_turns_aim). With the shotgun, R aims
        // while he moves and strafes with the C-buttons; Z (the laser sight) makes C-Left and C-Right turn the aim
        // instead, so pushing the left stick sideways turned the aim against the right stick (logged: 40 degrees in
        // 0.4 seconds, the right stick still). Its forward and back (C-Up, C-Down) still count. Elsewhere Z is
        // left alone: in split screen it fires, and strafing while shooting was lost.
        constexpr uint16_t z_button = 0x2000, c_left = 0x0002, c_right = 0x0001;
        if ((cur_buttons & z_button) && z_turns_aim) {
            left_stick_buttons &= (uint16_t)~(c_left | c_right);
        }
        cur_buttons |= left_stick_buttons;
        if (controller != nullptr && cont_profile >= 0) {
            cur_x = cont_analog(GameInput::X_AXIS_POS) - cont_analog(GameInput::X_AXIS_NEG);
            cur_y = cont_analog(GameInput::Y_AXIS_POS) - cont_analog(GameInput::Y_AXIS_NEG);
            recompinput::apply_joystick_deadzone(cur_x, cur_y, &cur_x, &cur_y);
        }
        if (keyboard && kb_profile >= 0) {
            const float kb_x = kb_analog(GameInput::X_AXIS_POS) - kb_analog(GameInput::X_AXIS_NEG);
            const float kb_y = kb_analog(GameInput::Y_AXIS_POS) - kb_analog(GameInput::Y_AXIS_NEG);
            if (swap_sticks) {
                // Aiming: Swap Sticks with the keyboard: the mouse aims, so the keys bound to the stick (W, A, S, D
                // by default) press the C-buttons and move Conker, as the left stick does, instead of aiming
                // (issue #84: they only turned the aim). Not C-Left or C-Right while Z is held, the same as the stick.
                constexpr uint16_t c_up = 0x0008, c_down = 0x0004;
                uint16_t keys = 0;
                if (kb_x <= -recompinput::axis_digital_threshold) keys |= c_left;
                if (kb_x >= recompinput::axis_digital_threshold) keys |= c_right;
                if (kb_y >= recompinput::axis_digital_threshold) keys |= c_up;
                if (kb_y <= -recompinput::axis_digital_threshold) keys |= c_down;
                if ((cur_buttons & z_button) && z_turns_aim) {
                    keys &= (uint16_t)~(c_left | c_right);
                }
                cur_buttons |= keys;
            }
            else {
                cur_x += kb_x;
                cur_y += kb_y;
            }
        }
        // In the bank's slow motion leaps the mouse moves the crosshair, as the stick (issue #85): port 1's.
        if (keyboard) {
            conker::look_aim::leap_stick(cur_x, cur_y);
        }
        *buttons = cur_buttons;
        *x = std::clamp(cur_x, -1.0f, 1.0f);
        *y = std::clamp(cur_y, -1.0f, 1.0f);
    }

    bool get_port_input_live(int port, uint16_t* buttons, float* x, float* y);

    // Online, the aim stick while aiming (R-Look and the other aiming modes), on the machine it's read on, before it
    // travels: every machine plays the value sent, so the game's state stays the same on all of them. Measured from
    // two recordings (0.9.54 and a Nintendo 64's, the view's movement frame by frame): on the PC the aim turned left and
    // right in a square wave, from still to full speed in a frame and back, as the game turns a gun's aim by the stick
    // with no easing across and a modern stick goes to the edge at once; the Nintendo 64's turned in smooth curves, its
    // stick moved through on the way. So:
    // Across only (0.9.56): up and down is the stick as it is, the game's spring easing it.
    // - a curve: half tilt turns a third as fast as full, for small corrections, full tilt as fast as before;
    // - easing: the stick's value comes up to where it's pushed over about a fifth of a second and back down in about a
    //   tenth, so the aim speeds up and settles instead of jumping, and a flick still turns at once.
    struct AimEase {
        bool on = false;
        float x = 0.0f, y = 0.0f;
        std::chrono::steady_clock::time_point last;
    } aim_ease;
    // Seconds for the eased value to cover about two thirds of the way: coming up (across, up and down), going back.
    constexpr float aim_rise_across = 0.09f, aim_fall = 0.045f;

    float aim_curve(float tilt) {
        return 0.3f * tilt + 0.7f * tilt * tilt;
    }

    void shape_online_aim(bool aiming, float* x, float* y) {
        const auto now = std::chrono::steady_clock::now();
        if (!aiming) {
            aim_ease.on = false;
            return;
        }
        // (across only: up and down the game's spring eases already, and 0.9.55's curve and easing on it made it slow)
        float tx = *x;
        const float ty = *y;
        tx = std::copysign(aim_curve(std::min(std::fabs(tx), 1.0f)), tx);
        if (!aim_ease.on) {
            aim_ease = AimEase{ true, 0.0f, 0.0f, now };
        }
        const float dt = std::clamp(std::chrono::duration<float>(now - aim_ease.last).count(), 0.0f, 0.1f);
        aim_ease.last = now;
        const auto step = [dt](float& eased, float wanted, float rise) {
            const bool speeding_up = std::fabs(wanted) > std::fabs(eased) && wanted * eased >= 0.0f;
            const float seconds = speeding_up ? rise : aim_fall;
            eased += (wanted - eased) * (1.0f - std::exp(-dt / seconds));
            if (std::fabs(eased) < 0.002f && wanted == 0.0f) {
                eased = 0.0f;
            }
        };
        step(aim_ease.x, tx, aim_rise_across);
        aim_ease.y = ty;
        *x = aim_ease.x;
        *y = ty;
    }

    // (the game thread's: the sampler, and what asks for it, run there)
    SDL_GameController* local_pad_used = nullptr;

    // The online session's own player is whoever holds this machine's first port.
    bool online_local_sampler(uint16_t* buttons, float* x, float* y) {
        return get_port_input_live(0, buttons, x, y);
    }

    // Netplay's determinism test (netplay.cpp) sits between the game and the devices: it counts the game's
    // frames, and either records what the devices answer or answers from a recording. Without its environment
    // variables set, this is the devices' answer, unchanged.
    bool get_port_input(int port, uint16_t* buttons, float* x, float* y) {
        // The online session (net_session.cpp) goes first: the first read of a frame waits until the
        // frame's inputs, everyone's, are agreed, and every port is then answered from them.
        conker::netsession::on_port_read(port);
        conker::netplay::on_port_read(port);
        bool plugged = false;
        if (conker::netsession::input_for_game(port, &plugged, buttons, x, y)) {
            return plugged;
        }
        if (conker::netplay::replay_input(port, &plugged, buttons, x, y)) {
            return plugged;
        }
        const bool answered = get_port_input_live(port, buttons, x, y);
        conker::netplay::record_input(port, answered, *buttons, *x, *y);
        return answered;
    }

    bool get_port_input_live(int port, uint16_t* buttons, float* x, float* y) {
        // recompinput's own multiplayer mode (players assigned in its menus) gives each
        // player their controller and profiles already.
        if (!recompinput::players::is_single_player_mode()) {
            return recompinput::profiles::get_n64_input(port, buttons, x, y);
        }
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
        if (port < 0 || port >= max_ports) {
            return false;
        }
        std::array<SDL_GameController*, max_ports> controllers{};
        int connected = 0;
        int count = get_port_controllers(controllers, &connected);
        if (port >= std::max(connected, 1)) {
            return false;
        }
        // (online with CONKER_NET_INPUT set, two windows on one PC: the window that isn't in front still plays, with its own
        // controller, so the game's "input disabled" is not asked)
        if (!recompinput::game_input_disabled() || conker::netsession::input_filter() != 0) {
            // Port 1 has the keyboard, and its controller once one has pressed a button.
            // An online game on one PC (two windows) can give one window the keyboard and the other the gamepad
            // (CONKER_NET_INPUT=keyboard or gamepad, net_session.cpp); otherwise both feed this port.
            const int filter = conker::netsession::input_filter();
            // (filters 3 and 4: the second controller by the order the computer lists them in, not by who pressed a button
            // first, which each window only sees while it has the focus; CONKER_NET_PAD=n picks another one, for these
            // and for filter 2)
            static const int forced_pad = []() { const char* text = SDL_getenv("CONKER_NET_PAD"); return text != nullptr ? std::atoi(text) : -1; }();
            SDL_GameController* pad = (port < count && filter != 1) ? controllers[port] : nullptr;
            if (filter >= 2) {
                // (filter 2, the gamepad: the first controller listed)
                const int index = forced_pad >= 0 ? forced_pad : (filter == 2 ? 0 : 1);
                pad = nullptr;
                std::lock_guard open_lock{ open_controllers_mutex };
                // by the controller's XInput slot (the player number SDL gives it: the ring's light on a 360 pad), which
                // is what "controller 1" and "controller 2" mean, and only without one by the order listed
                for (const auto& item : open_controllers) {
                    if (SDL_GameControllerGetPlayerIndex(item.second) == index) {
                        pad = item.second;
                        break;
                    }
                }
                if (pad == nullptr && index < (int)open_controllers.size()) {
                    pad = open_controllers[index].second;
                }
            }
            {
                // the choice is logged (once, and again when it changes), to see which controller a window listens to
                static const SDL_GameController* last_pad = (const SDL_GameController*)1;
                static bool first = true;
                if (first || pad != last_pad) {
                    first = false;
                    last_pad = pad;
                    std::string text = "[net] input: filter " + std::to_string(filter) + ", CONKER_NET_PAD " + std::to_string(forced_pad) + ", port " + std::to_string(port) + ", controllers listed:";
                    {
                        std::lock_guard open_lock{ open_controllers_mutex };
                        for (size_t i = 0; i < open_controllers.size(); i++) {
                            const char* name = SDL_GameControllerName(open_controllers[i].second);
                            text += " [" + std::to_string(i) + "] " + (name != nullptr ? name : "?") + " (player " + std::to_string(SDL_GameControllerGetPlayerIndex(open_controllers[i].second)) + ")" + (open_controllers[i].second == pad ? " (USED)" : "");
                        }
                    }
                    text += pad == nullptr ? " -> no controller used" : "";
                    conker::netsession::log(text.c_str());
                }
            }
            const bool keys = port == 0 && filter != 2 && filter != 3;   // (4: the keyboard too)
            if (port == 0) {
                local_pad_used = pad;
            }
            // The right stick turns the camera (Right Stick: Free Camera): it presses no C-button. That's about the controller,
            // not the keyboard (an online window on one PC that has only the gamepad has no `keys`, and its stick still has to).
            const bool free_stick = port == 0 && (conker::netsession::active() ? conker::look_aim::stick_free_camera() : conker::mouse_camera::stick_turns_camera());
            {
                static int last_free = -1;
                if (conker::netsession::active() && last_free != (free_stick ? 1 : 0)) {
                    last_free = free_stick ? 1 : 0;
                    conker::netsession::log(free_stick ? "[input] right stick: free camera, no C-buttons from it" : "[input] right stick: C-buttons (Free Camera off)");
                }
            }
            // Online (Aiming: Swap Sticks, with a gamepad): the player aims with the right stick and moves with the left
            // while they aim in R-Look. The sticks are swapped from the moment the game aims, or R is held (it starts the
            // aim, three frames before this input is played: held R swaps at once, not after the aim has begun). The swap
            // travels with the frame's input (net_session.cpp) so every machine acts on it at the same frame.
            const bool online_game = conker::netsession::active();
            const int aim_player = online_game ? conker::netsession::local_slot() : port;
            const bool online_swap = online_game && port == 0 && pad != nullptr && conker::look_aim::online_swap_enabled();
            bool swapped = online_game ? (online_swap && conker::look_aim::aiming_recently(aim_player)) : conker::look_aim::swap_sticks_now(port);
            // (while the sticks are swapped the right stick is the aim's, not the camera's: no free camera then)
            read_port(pad, keys, free_stick && !swapped, swapped, conker::look_aim::z_turns_aim(aim_player), buttons, x, y);
            if (online_swap && !swapped && (*buttons & 0x0010) != 0) {
                swapped = true;
                read_port(pad, keys, false, true, conker::look_aim::z_turns_aim(aim_player), buttons, x, y);
            }
            // Online, from R pressed until the game's look mode starts (its stage, +0x73C of the camera, is 0 until then; it
            // moves in at once, but not always) the right stick does nothing: the camera turned (or the head did) ahead of the aim and then
            // the view jerked in. Swapped, the stick is the aim's (x, y); else it's the camera's, C-buttons or free.
            // And through the look's first stage (1: the camera swings round onto the aim, about a second, as on a Nintendo 64): held
            // in a direction, the stick turned the head ahead of the camera, which then jumped onto the aim as the stage ended.
            const int stage = conker::look_aim::look_stage(aim_player);
            const bool entry_hold = online_game && port == 0 && pad != nullptr && (*buttons & 0x0010) != 0
                && ((stage == 0 && !conker::look_aim::aiming_recently(aim_player)) || stage == 1);
            if (entry_hold) {
                if (swapped) {
                    *x = 0.0f;
                    *y = 0.0f;
                } else {
                    read_port(pad, keys, true, false, conker::look_aim::z_turns_aim(aim_player), buttons, x, y);
                }
            }
            // (the aim stick's curve and easing, above: while aiming, swapped or not, and not held still for the look's start)
            if (online_game && port == 0) {
                shape_online_aim(pad != nullptr && !entry_hold && (swapped || conker::look_aim::aiming_recently(aim_player)), x, y);
            }
            if (port == 0) {
                conker::look_aim::set_sampled_swap(online_game && swapped);
                conker::look_aim::set_entry_hold(entry_hold);
            }
        }
        return true;
    }
}

void conker::frontend::start_online(uint8_t* rdram) {
    if (conker::netsession::init(rdram, online_local_sampler)) {
        conker::netplay::enter_online(rdram);
    }
}

SDL_GameController* conker::frontend::local_pad() {
    return local_pad_used;
}

int conker::frontend::port_controllers(std::array<SDL_GameController*, max_ports>& out) {
    return get_port_controllers(out);
}

namespace {
    ultramodern::input::connected_device_info_t get_connected_device_info_live(int controller_num);
}

// Netplay's determinism test (netplay.cpp) records which controllers are plugged in, or answers from a recording.
ultramodern::input::connected_device_info_t conker::frontend::get_connected_device_info(int controller_num) {
    bool present = false;
    if (conker::netsession::device_present(controller_num, &present)) {
        return present ? ultramodern::input::connected_device_info_t{ ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak }
                       : ultramodern::input::connected_device_info_t{ ultramodern::input::Device::None, ultramodern::input::Pak::None };
    }
    if (conker::netplay::replay_device(controller_num, &present)) {
        return present ? ultramodern::input::connected_device_info_t{ ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak }
                       : ultramodern::input::connected_device_info_t{ ultramodern::input::Device::None, ultramodern::input::Pak::None };
    }
    const ultramodern::input::connected_device_info_t info = get_connected_device_info_live(controller_num);
    conker::netplay::record_device(controller_num, info.connected_device != ultramodern::input::Device::None);
    return info;
}

namespace {
ultramodern::input::connected_device_info_t get_connected_device_info_live(int controller_num) {
    if (!recompinput::players::is_single_player_mode()) {
        if (recompinput::players::get_player_is_assigned(controller_num)) {
            return { ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak };
        }
        return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
    }
    std::array<SDL_GameController*, max_ports> controllers{};
    int connected = 0;
    get_port_controllers(controllers, &connected);
    if (controller_num == 0 || controller_num < connected) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}
} // namespace

std::u8string conker::program_id() {
    return SDL_getenv("CONKER_TEST_PROFILE") != nullptr ? u8"ConkerRecompiledTest" : u8"ConkerRecompiled";
}

void conker::frontend::init(recomp::GameEntry& game) {
    recompui::programconfig::set_program_name(program_name);
    recompui::programconfig::set_program_id(program_id());

    // The launcher's picture of the game.
    std::ifstream file(recompui::file::get_asset_path("thumbnail.png"), std::ios::binary);
    thumbnail.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    game.thumbnail_bytes = std::span<const char>(thumbnail);
    supported_games.push_back(game);

    // Versions of the ROM (the original, an uncensored ROM hack...) to switch between.
    recompui::register_launcher_init_callback(init_launcher);
    recompui::register_launcher_update_callback(update_launcher);

    // Lilita One (SIL Open Font License, see assets/LilitaOne-OFL.txt): a chunky, cartoony face close to the game's own lettering.
    // CONKER_FONT=inter goes back to RecompFrontend's usual Inter.
    if (SDL_getenv("CONKER_FONT") != nullptr && std::string(SDL_getenv("CONKER_FONT")) == "inter") {
        recompui::register_primary_font("InterVariable.ttf", "Inter Variable");
    }
    else {
        recompui::register_primary_font("LilitaOne-Regular.ttf", "Lilita One");
    }
    apply_conker_theme();
    recompui::register_ui_exports();
    recompinput::players::set_single_player_mode(true);
    conker::init_config();

    // Texture packs (texture_packs.cpp).
    conker::texture_packs::register_type();
}

void conker::frontend::set_callbacks(recomp::Configuration& cfg) {
    cfg.renderer_callbacks.create_render_context = create_render_context;
    cfg.gfx_callbacks = { create_gfx, create_window, update_gfx };
    cfg.input_callbacks = { poll_inputs, get_port_input, conker::rumble::set,
                            conker::get_connected_device_info };
    cfg.error_handling_callbacks.message_box = recompui::message_box;
}
