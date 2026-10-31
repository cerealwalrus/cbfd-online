// The settings menu (recompui's config tabs) for the RT64 build.

#include <cstdio>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "librecomp/game.hpp"
#include "ultramodern/config.hpp"
#include "ultramodern/ultramodern.hpp"
#include "recompui/config.h"
#include "recompinput/recompinput.h"
#include "util/file.h"

#include "conker.hpp"


namespace conker {
    namespace default_settings {
        extern const char* const general_json;
        extern const char* const graphics_json;
        extern const char* const online_json;
        extern const char* const camera_json;
        extern const char* const controls_json;
    }
}

namespace {
    // Cutscene Aspect Ratio (cutscene_aspect.cpp), on the Graphics tab.
    const std::string cutscene_aspect_id = "cutscene_aspect";
    enum class CutsceneAspect : uint32_t { Expand, Original };
    // Show FPS (fps_counter.cpp), on the Graphics tab.
    const std::string show_fps_id = "show_fps";
    // (0.11.4) Show Latency (fps_counter.cpp's latency meter), on the Graphics tab
    const std::string show_latency_id = "show_latency";
    enum class ShowFps : uint32_t { Off, On };
    enum class KillFeed : uint32_t { Off, On };
    enum class HudNotices : uint32_t { Text, Boxed };
    enum class KillFeedPlace : uint32_t { TopLeft, TopRight, BottomLeft, BottomRight };
    // Overscan Borders (widescreen.cpp, RT64's presentation crop), on the Graphics tab.
    // (0.11.4: Overscan Borders taken out, always Hidden: overscan_borders::hidden)
    // Conker's Detail (model_detail.cpp, the model the game picks by the camera's distance), on the
    // Graphics tab.
    const std::string conker_detail_id = "conker_detail";
    enum class ConkerDetail : uint32_t { AlwaysHighest, Original };
    // VSync (0.10.15: RT64's swap chain, rt64.patch's setVsyncWanted, applied from frontend.cpp), on the Graphics tab.
    const std::string vsync_id = "vsync";
    enum class VSync : uint32_t { On, Off };
    // (0.10.49) Windowed Size (window_chrome.cpp, frontend.cpp's create_window), on the Graphics tab.
    const std::string windowed_size_id = "windowed_size";
    enum class WindowedSize : uint32_t { FillScreen, Size1600x900 };
    // (possess67) the launcher's own window mode and size, on the General tab: the Graphics tab's Window Mode and Windowed Size are the
    // game's, taken as it starts (frontend.cpp's apply_window_mode, window_chrome.cpp)
    const std::string launcher_window_mode_id = "launcher_window_mode";
    const std::string launcher_windowed_size_id = "launcher_windowed_size";
    enum class LauncherWindowMode : uint32_t { Windowed, Fullscreen };
    // (0.11.4) Aspect Ratio, on the Graphics tab: one setting for RecompFrontend's Aspect Ratio (ar_option), Cutscene Aspect Ratio and
    // HUD Placement (hr_option), which are hidden and follow it. Wide: Expand, Expand, Expand; Original: Original, 4:3, Original.
    const std::string aspect_mode_id = "aspect_mode";
    enum class AspectMode : uint32_t { Original, Wide };

    void add_launcher_window_options(recomp::config::Config& config) {
        static const std::vector<recomp::config::ConfigOptionEnumOption> mode_choices = {
            {LauncherWindowMode::Windowed, "Windowed", "Windowed"},
            {LauncherWindowMode::Fullscreen, "Fullscreen", "Fullscreen"},
        };
        config.add_enum_option(launcher_window_mode_id, "Launcher: Window Mode",
            "Whether the launcher (the menus before the game starts) shows in a window or full screen. The game itself takes "
            "the Graphics tab's <recomp-color primary>Window Mode</recomp-color> as it starts.",
            mode_choices, LauncherWindowMode::Windowed);
        static const std::vector<recomp::config::ConfigOptionEnumOption> size_choices = {
            {WindowedSize::FillScreen, "FillScreen", "Fill Screen"},
            {WindowedSize::Size1600x900, "1600x900", "1600 x 900"},
        };
        config.add_enum_option(launcher_windowed_size_id, "Launcher: Windowed Size",
            "The size of the launcher's window when <recomp-color primary>Launcher: Window Mode</recomp-color> is Windowed. "
            "<recomp-color primary>Fill Screen</recomp-color> fills the screen above the taskbar. "
            "<recomp-color primary>1600 x 900</recomp-color> opens a 1600 x 900 window in the middle of the screen. "
            "The game itself takes the Graphics tab's <recomp-color primary>Windowed Size</recomp-color> as it starts.",
            size_choices, WindowedSize::FillScreen);
        config.add_option_disable_dependency(launcher_windowed_size_id, launcher_window_mode_id, LauncherWindowMode::Fullscreen);
    }

    // (0.11.4) the hidden settings Aspect Ratio stands for, set to match it (Load: the value read from the file; Temporary: chosen in the
    // menu, kept until Apply as the tab's others are; Permanent: applied)
    void apply_aspect_mode(recomp::config::Config& config, AspectMode mode, recomp::config::OptionChangeContext context) {
        const bool wide = mode == AspectMode::Wide;
        const std::pair<std::string, uint32_t> parts[] = {
            { recompui::config::graphics::options::ar_option,
              (uint32_t)(wide ? ultramodern::renderer::AspectRatio::Expand : ultramodern::renderer::AspectRatio::Original) },
            { cutscene_aspect_id, (uint32_t)(wide ? CutsceneAspect::Expand : CutsceneAspect::Original) },
            { recompui::config::graphics::options::hr_option,
              (uint32_t)(wide ? ultramodern::renderer::HUDRatioMode::Full : ultramodern::renderer::HUDRatioMode::Original) },
        };
        for (const auto& part : parts) {
            if (!config.has_option(part.first)) {
                continue;
            }
            if (config.get_temp_option_value(part.first) != recomp::config::ConfigValueVariant{ part.second }) {
                config.update_option_value(part.first, part.second);
            }
            if (context != recomp::config::OptionChangeContext::Temporary) {
                config.apply_option_value(part.first);   // (as loaded or applied: not left waiting for Apply)
            }
        }
    }

    void add_graphics_options(recomp::config::Config& config) {
        static const std::vector<recomp::config::ConfigOptionEnumOption> choices = {
            {CutsceneAspect::Expand, "Expand", "Expand"},
            {CutsceneAspect::Original, "Original", "4:3"},
        };
        config.add_enum_option(cutscene_aspect_id, "Cutscene Aspect Ratio",
            "The aspect ratio of cutscenes: story scenes, conversations, B pads' hints and Conker's thoughts, "
            "whenever the game plays one and you can't move. "
            "<recomp-color primary>Expand</recomp-color> shows them as wide as the rest of the game, where characters "
            "waiting for their cue can sometimes be seen beside the original picture. "
            "<recomp-color primary>4:3</recomp-color> shows them as on the N64, with black bars at the sides, "
            "and the game goes on in widescreen after each one.",
            choices, CutsceneAspect::Expand);
        config.add_option_disable_dependency(cutscene_aspect_id,
            recompui::config::graphics::options::ar_option, ultramodern::renderer::AspectRatio::Original);
        config.update_option_hidden(cutscene_aspect_id, true);   // (0.11.4: Aspect Ratio sets it)

        static const std::vector<recomp::config::ConfigOptionEnumOption> fps_choices = {
            {ShowFps::Off, "Off", "Off"},
            {ShowFps::On, "On", "On"},
        };
        config.add_enum_option(show_fps_id, "Show FPS",
            "Shows the frame rate in the top-right corner while playing, to spot slowdowns. "
            "<recomp-color primary>FPS</recomp-color> is the frames drawn to the screen each second (with interpolation, "
            "more than the game makes): a drop there is the PC falling behind. "
            "<recomp-color primary>Game</recomp-color> is the frames the game itself makes, up to 30: a drop there with FPS "
            "steady is the game's own slowdown, as on the N64.",
            fps_choices, ShowFps::Off);
        config.add_enum_option(show_latency_id, "Show Latency", "", fps_choices, ShowFps::Off);

        // (0.11.4) Overscan Borders taken out of the launcher: Hidden for everyone (overscan_borders::hidden)
        // (possess69) Conker's Detail taken out of the launcher: Always Highest for everyone (model_detail::always_highest)

        static const std::vector<recomp::config::ConfigOptionEnumOption> vsync_choices = {
            {VSync::On, "On", "On"},
            {VSync::Off, "Off", "Off"},
        };
        config.add_enum_option(vsync_id, "VSync",
            "Whether each frame waits for the screen's refresh before it's shown. "
            "<recomp-color primary>On</recomp-color> never tears. "
            "<recomp-color primary>Off</recomp-color> shows each frame as soon as it's ready, a few milliseconds sooner, "
            "but a fast-moving picture can tear (a line across the screen where two frames meet) unless the monitor has "
            "G-Sync or FreeSync on. Online, each player can choose their own: it changes nothing in the game itself.",
            vsync_choices, VSync::Off);   // (0.10.16: Off unless chosen)

        static const std::vector<recomp::config::ConfigOptionEnumOption> size_choices = {
            {WindowedSize::FillScreen, "FillScreen", "Fill Screen"},
            {WindowedSize::Size1600x900, "1600x900", "1600 x 900"},
        };
        config.add_enum_option(windowed_size_id, "Windowed Size",
            "The size of the game's window when <recomp-color primary>Window Mode</recomp-color> is Windowed, taken as the game "
            "starts and when this is changed while it plays (the launcher has its own, on the General tab). "
            "<recomp-color primary>Fill Screen</recomp-color> fills the screen above the taskbar (maximized). "
            "<recomp-color primary>1600 x 900</recomp-color> makes it a 1600 x 900 window in the middle of the screen.",
            size_choices, WindowedSize::FillScreen);
        config.add_option_disable_dependency(windowed_size_id,
            recompui::config::graphics::options::wm_option, ultramodern::renderer::WindowMode::Fullscreen);
        // (0.11.4) last, so that loading the settings it reads the others first and then sets them to match
        {
            static const std::vector<recomp::config::ConfigOptionEnumOption> aspect_choices = {
                {AspectMode::Original, "Original", "Original"},
                {AspectMode::Wide, "Wide", "Wide"},
            };
            config.add_enum_option(aspect_mode_id, "Aspect Ratio", "", aspect_choices, AspectMode::Wide);
            config.add_option_change_callback(aspect_mode_id,
                [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext context) {
                    apply_aspect_mode(recompui::config::get_graphics_config(), static_cast<AspectMode>(std::get<uint32_t>(cur)), context);
                });
            config.update_option_hidden(recompui::config::graphics::options::ar_option, true);
            config.update_option_hidden(recompui::config::graphics::options::hr_option, true);
        }
    }

    // Settings moved to another tab keep their ids, but each tab saves to its own json (<tab id>.json in
    // the config folder): the values saved in the old tab's file are copied into the new tab's, once,
    // before the tabs load them (any the new file has already are left as they are). The old file loses
    // them the next time its tab saves.
    void carry_over(const std::string& from_id, const std::string& to_id, const std::vector<std::string>& keys) {
        const std::filesystem::path folder = recomp::get_config_path();
        const auto read = [](const std::filesystem::path& path) {
            std::ifstream file(path);
            nlohmann::json json = file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json();
            return json.is_object() ? json : nlohmann::json::object();
        };
        const nlohmann::json from = read(folder / (from_id + ".json"));
        nlohmann::json to = read(folder / (to_id + ".json"));
        bool changed = false;
        for (const std::string& key : keys) {
            if (from.contains(key) && !to.contains(key)) {
                to[key] = from[key];
                changed = true;
            }
        }
        if (changed) {
            std::ofstream file(folder / (to_id + ".json"), std::ios::trunc);
            file << to.dump(4);
        }
    }

    // The tabs the General tab's camera and aiming settings moved to (look_aim.cpp), and the Texture
    // Packs tab's one setting, moved to the end of the Graphics tab.
    const std::string camera_tab_id = "camera";
    void carry_over_moved_settings() {
        carry_over(recompui::config::general::id, camera_tab_id, {
            "camera_invert_turning", "camera_turn_speed", "stick_free_camera", "camera_field_of_view_degrees",
            "look_stick_response", "look_stick_invert", "aim_swap_sticks", "aim_lock_on" });
        carry_over("texture_packs", recompui::config::graphics::id, { "texture_pack" });
        // (possess67) Chat: Key moved from the Online tab (gone for now) to General
        carry_over("online", recompui::config::general::id, { "online_chat_key" });
        // (possess67) the launcher's window mode and size start as the window settings were (the Graphics tab's, the game's now)
        {
            const std::filesystem::path folder = recomp::get_config_path();
            const auto read = [](const std::filesystem::path& path) {
                std::ifstream file(path);
                nlohmann::json json = file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json();
                return json.is_object() ? json : nlohmann::json::object();
            };
            const nlohmann::json graphics = read(folder / (recompui::config::graphics::id + ".json"));
            nlohmann::json general = read(folder / (recompui::config::general::id + ".json"));
            bool changed = false;
            const std::pair<const char*, const char*> pairs[] = { { "wm_option", "launcher_window_mode" }, { "windowed_size", "launcher_windowed_size" } };
            for (const auto& pair : pairs) {
                if (graphics.contains(pair.first) && !general.contains(pair.second)) {
                    general[pair.second] = graphics[pair.first];
                    changed = true;
                }
            }
            if (changed) {
                std::ofstream file(folder / (recompui::config::general::id + ".json"), std::ios::trunc);
                file << general.dump(4);
            }
        }
    }

    void set_control_descriptions() {
        using recompinput::GameInput;
        using recompinput::set_game_input_description;
        // (0.11.4) short, about online play
        const char* stick = "Move.";
        set_game_input_description(GameInput::Y_AXIS_POS, stick);
        set_game_input_description(GameInput::Y_AXIS_NEG, stick);
        set_game_input_description(GameInput::X_AXIS_NEG, stick);
        set_game_input_description(GameInput::X_AXIS_POS, stick);
        set_game_input_description(GameInput::A, "Jump. Press again in the air to hover. With the SMG, M4 or revolver out: reload too.");
        set_game_input_description(GameInput::B, "Take your weapon out or put it away.");
        set_game_input_description(GameInput::Z, "Crouch. With a weapon out: fire or attack.");
        set_game_input_description(GameInput::L, "Not used.");
        set_game_input_description(GameInput::R, "Hold to aim and look around.");
        set_game_input_description(GameInput::START, "Pause menu.");
        set_game_input_description(GameInput::C_UP, "With a weapon out: walk forward. Scoped: zoom in.");
        set_game_input_description(GameInput::C_DOWN, "Throw a grenade.");
        set_game_input_description(GameInput::C_LEFT, "Strafe left.");
        set_game_input_description(GameInput::C_RIGHT, "Strafe right.");
        const char* dpad = "Not used.";
        set_game_input_description(GameInput::DPAD_UP, "Walk forward while aiming the shotgun with R.");
        set_game_input_description(GameInput::DPAD_DOWN, "Walk back while aiming the shotgun with R.");
        set_game_input_description(GameInput::DPAD_LEFT, dpad);
        set_game_input_description(GameInput::DPAD_RIGHT, dpad);
        recompinput::set_game_input_disabled(GameInput::CHAT, true);   // (0.11.4: chat's key is the General tab's Chat: Key)
        // (0.11.4) the menu inputs hidden, their defaults everyone's (frontend.cpp's init_launcher puts them back each start)
        for (GameInput input : { GameInput::TOGGLE_MENU, GameInput::ACCEPT_MENU, GameInput::BACK_MENU, GameInput::APPLY_MENU,
                                 GameInput::TAB_LEFT_MENU, GameInput::TAB_RIGHT_MENU }) {
            recompinput::set_game_input_disabled(input, true);
        }
        set_game_input_description(GameInput::GRENADE, "Throw a grenade. Hold to keep it in your hand, let go to throw.");
        // (0.11.4) the dedicated controls (look_aim.cpp's dedicated_controls), bound to nothing to begin with
        // (0.11.4) First-Person View hidden: the arenas have no first-person view on C-Up to copy (the game's C-Up camera toggle is single
        // player's, off in the arenas); its bit in the dedicated controls stays (read_dedicated's order: the bits travel online)
        recompinput::set_game_input_disabled(GameInput::FIRST_PERSON, true);
        set_game_input_description(GameInput::CROUCH, "Crouch (with no gun out).");
        set_game_input_description(GameInput::ZOOM_IN, "Zoom in while scoped.");
        set_game_input_description(GameInput::ZOOM_OUT, "Zoom out while scoped.");
        set_game_input_description(GameInput::AIM_UP, "Aim up. Not aiming: tilts the Free Camera up.");
        set_game_input_description(GameInput::AIM_DOWN, "Aim down. Not aiming: tilts the Free Camera down.");
        set_game_input_description(GameInput::AIM_LEFT, "Aim left. Not aiming: turns the camera left.");
        set_game_input_description(GameInput::AIM_RIGHT, "Aim right. Not aiming: turns the camera right.");
        set_game_input_description(GameInput::FIRE, "Fire your gun (Z with a gun out).");
        set_game_input_description(GameInput::MOVE_FORWARD, "Walk forward, and nothing else (never zooms a scope). A second set of movement keys.");
        set_game_input_description(GameInput::MOVE_BACK, "Walk back, and nothing else (never zooms a scope).");
        set_game_input_description(GameInput::MOVE_LEFT, "Walk or strafe left, and nothing else.");
        set_game_input_description(GameInput::MOVE_RIGHT, "Walk or strafe right, and nothing else.");
        set_game_input_description(GameInput::WEAPON_SWAP, "Take your weapon out or put it away (B for the weapon alone).");
        set_game_input_description(GameInput::RELOAD, "Reload the SMG, M4 or revolver (never jumps, unlike A).");
    }
    // The settings a new player starts with (default_settings.cpp). Each flat tab file (general, graphics, camera) gets the keys it does
    // not have yet, so a setting the player has saved is never changed, and a tab that was never saved starts with these; the controls file
    // is only written when there is none. It runs after carry_over_moved_settings, so values moved from an old tab's file are kept.
    void seed_defaults() {
        const std::filesystem::path folder = recomp::get_config_path();
        const auto read = [](const std::filesystem::path& path) {
            std::ifstream file(path);
            nlohmann::json json = file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json();
            return json.is_object() ? json : nlohmann::json::object();
        };
        const auto write = [](const std::filesystem::path& path, const nlohmann::json& json) {
            std::ofstream file(path, std::ios::trunc);
            if (file) {
                file << json.dump(4);
            }
        };
        const struct { const char* id; const char* text; } flat_tabs[] = {
            { "general", conker::default_settings::general_json },
            { "graphics", conker::default_settings::graphics_json },
            { "camera", conker::default_settings::camera_json },
            { "online", conker::default_settings::online_json },   // (0.10.19)
        };
        for (const auto& tab : flat_tabs) {
            try {
                const nlohmann::json defaults = nlohmann::json::parse(tab.text, nullptr, false);
                if (!defaults.is_object()) {
                    continue;
                }
                const std::filesystem::path path = folder / (std::string(tab.id) + ".json");
                nlohmann::json current = read(path);
                bool changed = false;
                for (auto it = defaults.begin(); it != defaults.end(); ++it) {
                    if (!current.contains(it.key())) {
                        current[it.key()] = it.value();
                        changed = true;
                    }
                }
                if (changed) {
                    write(path, current);
                }
            }
            catch (const std::exception&) {
            }
        }
        try {
            const std::filesystem::path path = folder / "controls.json";
            if (!std::filesystem::exists(path)) {
                const nlohmann::json defaults = nlohmann::json::parse(conker::default_settings::controls_json, nullptr, false);
                if (defaults.is_object()) {
                    write(path, defaults);
                }
            }
        }
        catch (const std::exception&) {
        }
    }
}

namespace {
    // (0.11.4) every setting's description: short, about online play (bigger text: recomp/recompfrontend_text.patch, the description's
    // own size, as an inline size in the text didn't take)
    std::string big(const std::string& text) {
        return text;
    }

    struct ShortText {
        const char* tab;
        std::string option;
        const char* text;
    };

    const std::vector<ShortText>& short_texts() {
        namespace g = recompui::config::graphics::options;
        static const std::vector<ShortText> texts = {
            // General
            { "general", recompui::config::general::options::joystick_deadzone,
              "How far you push the left stick before you move. Raise it if you drift without touching the stick. Recommended: 5%." },
            { "general", recompui::config::general::options::background_input_mode,
              "Your controller keeps working when the game isn't the window in front. Recommended: On." },
            { "general", "aim_stick_sensitivity", "How fast the stick aims, for every weapon: R-Look and scopes, online and single player.<br/>Recommended: 100%." },
            // Mouse (the Controls tab's Mouse page)
            { "mouse", "mouse_enabled", "Aim and turn the camera with the mouse, online and single player. Off: the mouse does nothing in the game." },
            { "mouse", recompui::config::general::options::mouse_sensitivity, "How fast the mouse aims and turns the camera.<br/>Recommended: 100%." },
            { "mouse", "look_mouse_invert", "Flips the mouse when aiming. None: moving the mouse up looks up." },
            { "mouse", "mouse_turns_camera", "The mouse turns the camera around you when you're not aiming, with or without Free Camera.<br/>Recommended: On." },
            { "general", "chat_key", "The key that opens text chat online. Click it, then press a key. Backspace: no chat." },
            { "general", "kill_feed_place", "Which corner of the screen the kill feed is in, kept clear of the HUD there." },
            { "general", "kill_feed", "Shows who killed whom in multiplayer, each name in its player's colour. Bots are named by their number." },
            { "general", "hud_notices", "How every message drawn over the game looks: the kill feed and the notices at the top of the screen. Text: the words alone. Boxed: a dark box behind each." },
            { "general", launcher_window_mode_id, "The launcher in a window or full screen." },
            { "general", launcher_windowed_size_id, "The launcher window's size. F11 switches between Fill Screen and 1600 x 900." },
            // Camera
            { "camera", "aim_swap_sticks", "While aiming, the right stick aims and the left stick moves.<br/>Recommended: On, for modern controls." },
            { "camera", "camera_field_of_view_degrees", "How much of the world you see online, in degrees tall. Your own view only; single player, the scope and R-Look keep the original's.<br/>Recommended: 50 (the original)." },
            { "camera", "camera_invert_turning", "Flips the Free Camera. X: left and right. Y: up and down." },
            { "camera", "camera_turn_speed", "How fast the Free Camera turns. Recommended: 100%." },
            { "camera", "stick_free_camera", "The right stick moves the camera around you.<br/>Recommended: On, for modern controls." },
            { "camera", "look_stick_response", "How your aim feels online.<br/>Original: the original game's aim.<br/>Modern: no camera lag, faster up and down, for every weapon.<br/>Direct: the view snaps straight to your aim.<br/>Recommended: Modern." },
            { "camera", "look_stick_invert", "Flips the stick when aiming. Invert Y (the original): pushing up looks down." },
            // Graphics
            { "graphics", aspect_mode_id, "Wide fills a widescreen, with your HUD at the screen's edges. Original is the N64's 4:3 picture. Recommended: Wide." },
            { "graphics", g::res_option, "How sharp the picture is. Auto matches your window. Recommended: Auto." },
            { "graphics", g::ds_option, "Extra sharpness for the Original resolutions. Higher is slower." },
            { "graphics", g::wm_option, "Play in a window or full screen. F11 or Alt + Enter switches." },
            { "graphics", windowed_size_id, "The game window's size. Fill Screen fills your screen." },
            { "graphics", g::rr_option, "How smooth the picture is. Doesn't change gameplay. Original: the N64's own rate. Display: smoothest, with a little delay. NoLeSync: each new frame shown at once, with only the camera's turning smoothed in between." },
            { "graphics", g::rr_manual_value, "How smooth the picture is. Doesn't change gameplay. Original: the N64's own rate. Display: smoothest, with a little delay. NoLeSync: each new frame shown at once, with only the camera's turning smoothed in between." },
            { "graphics", g::msaa_option, "Smooths jagged edges. Higher is slower." },
            { "graphics", show_fps_id, "Shows your frame rate in the corner." },
            { "graphics", show_latency_id, "Shows how long from pressing a button to seeing it on screen, in the corner (first: the screen starts showing it; full: all of it). Each press also goes in latency.log. Doesn't count your controller's or monitor's own delay." },
            { "graphics", vsync_id, "On: syncs to your monitor's refresh rate.<br/>Off: reduces input latency.<br/>Recommended: Off." },
            { "graphics", "texture_pack", "Swap in HD or custom textures. Only changes what you see." },
            // Sound
            { "sound", recompui::config::sound::options::main_volume, "Your game's volume." },
        };
        return texts;
    }

    void apply_short_descriptions() {
        for (const ShortText& t : short_texts()) {
            try {
                recomp::config::Config& config = recompui::config::get_config(t.tab);
                if (config.has_option(t.option) && config.get_option(t.option).description != big(t.text)) {
                    config.update_option_description(t.option, big(t.text));
                }
            }
            catch (...) {
            }
        }
    }

    // (0.11.4) Joystick Deadzone's default 5% (was 4%): a player still on the old default (4, as seeded) moved to it, once (a file of
    // its own says so: the tab's file keeps only its settings)
    void seed_deadzone_5() {
        try {
            const std::filesystem::path done = recomp::get_config_path() / "deadzone_5.done";
            if (std::filesystem::exists(done)) {
                return;
            }
            const std::filesystem::path path = recomp::get_config_path() / "general.json";
            std::ifstream in(path);
            nlohmann::json json = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json();
            in.close();
            if (json.is_object() && json.contains("joystick_deadzone") && json["joystick_deadzone"].is_number() &&
                json["joystick_deadzone"].get<double>() == 4.0) {
                json["joystick_deadzone"] = 5;
                std::ofstream out(path, std::ios::trunc);
                if (out) {
                    out << json.dump(4);
                }
            }
            std::ofstream mark(done);
        }
        catch (...) {
        }
    }

    // (0.11.4) Chat: Key, a key now: a player's preset key carried over (the old setting's online_chat_key)
    void seed_chat_key() {
        try {
            const std::filesystem::path path = recomp::get_config_path() / "general.json";
            std::ifstream in(path);
            nlohmann::json json = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json();
            in.close();
            if (!json.is_object() || json.contains("chat_key") || !json.contains("online_chat_key") || !json["online_chat_key"].is_string()) {
                return;
            }
            const std::string old = json["online_chat_key"].get<std::string>();
            std::string key = "Y";
            if (old == "T" || old == "V" || old == "B" || old == "N" || old == "M") key = old;
            else if (old == "Tab") key = "Tab";
            else if (old == "Backtick") key = "`";
            else if (old == "Backslash") key = "\\";
            else if (old == "Off") key = "";
            json["chat_key"] = key;
            std::ofstream out(path, std::ios::trunc);
            if (out) {
                out << json.dump(4);
            }
        }
        catch (...) {
        }
    }

    // (0.11.4) Stick: Aiming Response's Modern (Direct's place): everyone moved to it once, as the online aim they had (the easing and the SMG
    // and M4's) is Modern's now; Original is the game's own
    void seed_modern_aim() {
        try {
            const std::filesystem::path done = recomp::get_config_path() / "modern_aim.done";
            if (std::filesystem::exists(done)) {
                return;
            }
            const std::filesystem::path path = recomp::get_config_path() / "camera.json";
            std::ifstream in(path);
            nlohmann::json json = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json();
            in.close();
            if (json.is_object()) {
                json["look_stick_response"] = "Direct";
                std::ofstream out(path, std::ios::trunc);
                if (out) {
                    out << json.dump(4);
                }
            }
            std::ofstream mark(done);
        }
        catch (...) {
        }
    }

    // (0.11.4) Aspect Ratio for someone who played before it: Original if their Aspect Ratio was, Wide otherwise
    void seed_aspect_mode() {
        try {
            const std::filesystem::path path = recomp::get_config_path() / "graphics.json";
            std::ifstream in(path);
            nlohmann::json json = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json();
            in.close();
            if (!json.is_object() || json.contains(aspect_mode_id)) {
                return;
            }
            const bool original = json.contains("ar_option") && json["ar_option"].is_string() && json["ar_option"].get<std::string>() == "Original";
            json[aspect_mode_id] = original ? "Original" : "Wide";
            std::ofstream out(path, std::ios::trunc);
            if (out) {
                out << json.dump(4);
            }
        }
        catch (...) {
        }
    }
}

// (0.11.4) The library writes the Framerate setting's description again as it learns the display's refresh rate: put back, from the
// launcher's update.
void conker::settings_text::refresh() {
    apply_short_descriptions();
}

// (0.11.4) F11 in the launcher: full screen goes to a Fill Screen window; windowed, Fill Screen and 1600 x 900 swap. (RecompFrontend's
// own F11 changed the game's Window Mode, which the launcher's own then put back: the window jumped between the two.)
void conker::windowed_size::launcher_f11() {
    recomp::config::Config* config = recompui::config::find_option_config(launcher_window_mode_id);
    if (config == nullptr) {
        return;
    }
    const bool full = static_cast<LauncherWindowMode>(std::get<uint32_t>(config->get_option_value(launcher_window_mode_id))) == LauncherWindowMode::Fullscreen;
    if (full) {
        config->update_option_value(launcher_window_mode_id, (uint32_t)LauncherWindowMode::Windowed);
        config->update_option_value(launcher_windowed_size_id, (uint32_t)WindowedSize::FillScreen);
    }
    else {
        const bool fill = static_cast<WindowedSize>(std::get<uint32_t>(config->get_option_value(launcher_windowed_size_id))) == WindowedSize::FillScreen;
        config->update_option_value(launcher_windowed_size_id, (uint32_t)(fill ? WindowedSize::Size1600x900 : WindowedSize::FillScreen));
    }
    config->save_config();
    std::printf("[window] F11 in the launcher: %s\n", full ? "full screen to a Fill Screen window" : "the window's size swapped");
}

void conker::init_config() {
    std::filesystem::path app_folder = recompui::file::get_app_folder_path();
    if (!app_folder.empty()) {
        std::filesystem::create_directories(app_folder);
    }

    carry_over_moved_settings();
    seed_defaults();
    seed_aspect_mode();
    seed_deadzone_5();
    seed_chat_key();
    seed_modern_aim();

    // The menu library's own rumble, gyro and mouse settings are left off the General tab: no rumble, and (0.11.4) no mouse or gyro
    // aiming at all, so their sensitivities read as 0 (the mouse, and the cursor, left alone). (Each Config is used as soon as it's
    // made: create_config_tab keeps them in a vector, so the next tab can move the last one.)
    recompui::config::GeneralTabOptions general_options{};
    general_options.has_rumble_strength = false;
    general_options.has_gyro_sensitivity = false;
    general_options.has_mouse_sensitivity = false;
    // (0.10.4: rumble is off for good, and its settings are gone: no rumble options on the General tab)
    recomp::config::Config& general_config = recompui::config::create_general_tab(general_options);
    // (possess67) Chat: Key on the General tab; the Online tab taken out for now
    // (0.11.4) Chat: Key, a key bound to it (its button waits for the next key: recomp/recompfrontend_chatkey.patch), Y to begin with, in
    // place of its preset keys (net_chat.cpp)
    general_config.add_string_option("chat_key", "Chat: Key", "", "Y");
    // (0.11.4) Stick: Aiming Sensitivity, how fast the stick aims every weapon (look_aim.cpp's aim_sensitivity): online each player's
    // own, in their input (net_session.cpp), so it can't desync
    general_config.add_number_option("aim_stick_sensitivity", "Stick: Aiming Sensitivity", "", 50.0, 200.0, 5.0, 0, true, 100.0);
    // (0.11.4) Kill Feed (kill_feed.cpp): On or Off. HUD Notifications: every message drawn over the game (the kill feed, the
    // notices at the top) as Text (the lines alone) or Boxed (a dark box behind each)
    {
        static const std::vector<recomp::config::ConfigOptionEnumOption> kill_feed_choices = {
            {KillFeed::Off, "Off", "Off"},
            {KillFeed::On, "On", "On"},
        };
        general_config.add_enum_option("kill_feed", "Kill Feed", "", kill_feed_choices, KillFeed::On);
        static const std::vector<recomp::config::ConfigOptionEnumOption> hud_notice_choices = {
            {HudNotices::Text, "Text", "Text"},
            {HudNotices::Boxed, "Boxed", "Boxed"},
        };
        general_config.add_enum_option("hud_notices", "HUD Notifications", "", hud_notice_choices, HudNotices::Boxed);
        static const std::vector<recomp::config::ConfigOptionEnumOption> kill_feed_places = {
            {KillFeedPlace::TopLeft, "TopLeft", "Top Left"},
            {KillFeedPlace::TopRight, "TopRight", "Top Right"},
            {KillFeedPlace::BottomLeft, "BottomLeft", "Bottom Left"},
            {KillFeedPlace::BottomRight, "BottomRight", "Bottom Right"},
        };
        general_config.add_enum_option("kill_feed_place", "Kill Feed: Placement", "", kill_feed_places, KillFeedPlace::BottomRight);
    }
    add_launcher_window_options(general_config);
    conker::look_aim::add_camera_options(recompui::config::create_config_tab("Camera", camera_tab_id, false));

    recomp::config::Config& graphics_config = recompui::config::create_graphics_tab();
    add_graphics_options(graphics_config);
    conker::texture_packs::add_options(graphics_config);
    // (0.11.4) the mouse back (no gyro): its aim online in each player's input (net_session.cpp), so it can't desync. Its settings on the
    // Controls tab's Mouse page (recompfrontend_mousepage.patch): a config of their own with no tab, "mouse" (mouse.json). Made after
    // the others are filled in: each new config can move the ones before it (the library keeps them in a vector), so a config's
    // reference is only good until the next is made (the 0.11.4 test's crash: made between Chat: Key and the launcher's options,
    // the General tab's reference was left pointing at freed memory).
    conker::look_aim::add_mouse_options(recompui::config::create_hidden_config("Mouse", "mouse"));

    set_control_descriptions();
    recompui::config::create_controls_tab();

    recompui::config::create_sound_tab();
    apply_short_descriptions();

    conker::local_achievements::add_tab();

    recompui::config::create_mods_tab();

    recompui::config::finalize();
}


bool conker::graphics_config_ready() {
    // Not with --headless, which makes no menus: the game still asks for these every frame, and
    // reading them threw ("Graphics config has not been created yet").
    static bool ready = false;
    if (!ready) {
        try {
            (void)recompui::config::get_graphics_config();
            ready = true;
        }
        catch (const std::exception&) {
        }
    }
    return ready;
}

bool conker::cutscene_aspect::in_4x3() {
    if (!conker::graphics_config_ready()) {
        return false;
    }
    const auto value = recompui::config::get_graphics_config().get_option_value(cutscene_aspect_id);
    return static_cast<CutsceneAspect>(std::get<uint32_t>(value)) == CutsceneAspect::Original;
}

bool conker::overscan_borders::hidden() {
    return true;   // (0.11.4) always: the setting is gone
}

bool conker::model_detail::always_highest() {
    // (possess69) always on for everyone, the setting gone (the multiplayer arenas keep the game's own pick: model_detail.cpp)
    return true;
}

bool conker::vsync::enabled() {
    if (!conker::graphics_config_ready()) {
        return false;
    }
    const auto value = recompui::config::get_graphics_config().get_option_value(vsync_id);
    return static_cast<VSync>(std::get<uint32_t>(value)) == VSync::On;
}

bool conker::fps_counter::enabled() {
    if (!conker::graphics_config_ready()) {
        return false;
    }
    const auto value = recompui::config::get_graphics_config().get_option_value(show_fps_id);
    return static_cast<ShowFps>(std::get<uint32_t>(value)) == ShowFps::On;
}

bool conker::latency_meter::enabled() {
    if (!conker::graphics_config_ready()) {
        return false;
    }
    const auto value = recompui::config::get_graphics_config().get_option_value(show_latency_id);
    return static_cast<ShowFps>(std::get<uint32_t>(value)) == ShowFps::On;
}

bool conker::windowed_size::fill_screen() {
    if (!conker::graphics_config_ready()) {
        return true;
    }
    // (possess67) the launcher's own size until the game starts, then the game's (the Graphics tab's)
    const std::string& id = ultramodern::is_game_started() ? windowed_size_id : launcher_windowed_size_id;
    recomp::config::Config* config = recompui::config::find_option_config(id);
    if (config == nullptr) {
        return true;
    }
    return static_cast<WindowedSize>(std::get<uint32_t>(config->get_option_value(id))) == WindowedSize::FillScreen;
}

bool conker::windowed_size::launcher_fullscreen() {
    recomp::config::Config* config = recompui::config::find_option_config(launcher_window_mode_id);
    if (config == nullptr) {
        return false;
    }
    return static_cast<LauncherWindowMode>(std::get<uint32_t>(config->get_option_value(launcher_window_mode_id))) == LauncherWindowMode::Fullscreen;
}
