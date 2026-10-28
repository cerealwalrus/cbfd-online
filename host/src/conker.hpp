#ifndef __CONKER_HPP__
#define __CONKER_HPP__

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <mutex>
#include <string>
#include <vector>

#include <functional>
#include "librecomp/game.hpp"
#include "ultramodern/input.hpp"

struct SDL_Window;
#include "ultramodern/renderer_context.hpp"

namespace recomp::config {
    class Config;
}

struct _SDL_GameController;

namespace conker {
    // overlays.cpp
    void register_overlays();
    void register_tlb_mapped_code();
    void map_tlb_code_pages(uint8_t* rdram);
    std::vector<uint8_t> decompress_rom(std::span<const uint8_t> rom);

    // mod_api.cpp: functions the game exports to mods.
    void register_mod_exports();

    // cutscene_aspect.cpp: the Cutscene Aspect Ratio setting, full cutscenes in 4:3.
    namespace cutscene_aspect {
        // Every frame, as the game starts its display list: sets the renderer's aspect ratio for a
        // full cutscene, and back after it.
        void update(uint8_t* rdram);
        // conker_config.cpp: whether the setting is 4:3.
        bool in_4x3();
    }

    // rom_versions.cpp: the ROMs the game accepts, and the versions of them kept for the
    // launcher to switch between.
    namespace roms {
        inline constexpr uint64_t us_rom_hash = 0x23FBBA2DBCF2FD8EULL; // XXH3-64 of the US ROM (big-endian .z64)
        // The US ROM, or a ROM hack that only changes the game's assets.
        bool accept(std::span<const uint8_t> rom);
        // Keeps versions next to the runtime's stored ROM (the one in play).
        void init(const std::filesystem::path& stored_rom);
        // Keeps a newly stored ROM (from Load ROM) as a version. True if the one in play changed.
        bool update();
        size_t version_count();
        // "US Original", "US Uncensored" or "US ROM hack (<hash>)"; empty with no ROM yet.
        std::string current_name();
        // Puts the next kept version in play. False if there's no other.
        bool switch_to_next();
        // The region a ROM file's header names ("US", "European"...); empty if it can't tell.
        std::string region_of(const std::filesystem::path& rom_path);
        // The stored ROM in play (what a dedicated server started from the launcher loads).
        std::filesystem::path stored_rom_path();
    }

    // main.cpp: why a ROM was refused.
    const char* rom_error_text(recomp::RomValidationError error);

    // main.cpp: the device info for whichever input backend is active.
    ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num);

    // null_renderer.cpp
    std::unique_ptr<ultramodern::renderer::RendererContext> create_null_renderer(
        uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

#if defined(CONKER_RT64)
    inline constexpr const char* program_name = "Conker's Bad Fur Day: Recompiled";
    // The game window's title (the program's name above is also what the menus and settings know it by, so it stays).
    inline constexpr const char* window_title = "Conker's Bad Fur Day: Online";
    // The data folder's name (%LOCALAPPDATA%\<id>). CONKER_TEST_PROFILE=1 gives test
    // runs their own ("ConkerRecompiledTest"), so they never touch the player's saves.
    std::u8string program_id();

    // frontend.cpp: RecompFrontend's launcher and menus (recompui) and input (recompinput).
    namespace frontend {
        // The window's size when it opens (main.cpp's --window); 1600 x 900 when 0.
        inline int window_width = 0;
        inline int window_height = 0;
        // Registers the game with the launcher and sets up the menus.
        void init(recomp::GameEntry& game);
        // The window, renderer, input and error callbacks.
        void set_callbacks(recomp::Configuration& cfg);
        void on_vi();
        ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num);
        // The controllers holding ports 1-4, in port order; returns how many hold one.
        int port_controllers(std::array<_SDL_GameController*, 4>& out);
        // The controller the online game's own player (this machine's port 1) is read from, as the last read chose it: with
        // two windows on one PC that is each window's own, not the first one listed. Null if none.
        _SDL_GameController* local_pad();
        // From the game's init (main.cpp): starts the online session if the environment asks for one.
        void start_online(uint8_t* rdram);
    }

    // pad_mappings.cpp: C-buttons SDL maps as face buttons made the right stick (issue #28).
    namespace pad_mappings {
        // Rewrites the mapping of each such controller connected. From the main thread only: at start,
        // and from SDL's event watch as a controller connects (it lists SDL's devices, issue #88).
        void fix_all();
    }

    // texture_packs.cpp: RT64 texture packs (issue #63).
    namespace texture_packs {
        // Registers the texture pack content type and .rtz files with the mod loader, and has the mod
        // installer take .htc files.
        void register_type();
        // 0.10.40: the texture pack that comes with the program (assets/packs: HD Icons, with the barrels' MANSION), put in the mods
        // folder at startup if it isn't there (once: deleted later, it isn't put back; a newer one that comes replaces it), and an
        // older copy of it moved out of the way (mods/replaced). Before the settings are made, so the Texture Pack setting lists it.
        void install_bundled();
        // At the launcher (the runtime has opened the mods): the pack installed just now is turned on.
        void enable_bundled();
        // gliden64_packs.cpp: unpacks each GLideN64 texture cache (.htc) in the mods folder not unpacked
        // yet into a pack folder, on a thread of its own, and has the runtime open it.
        void unpack_gliden64_packs();
        // gliden64_packs.cpp: on the main thread (update_gfx), shows the unpacking's progress, and once
        // it's done has the runtime open the packs.
        void update_unpacking();
        // gliden64_packs.cpp: the mod id of the pack a .htc unpacks into.
        std::string gliden64_pack_id(const std::filesystem::path& htc);
        // The Texture Pack setting, at the end of the Graphics tab, listing the packs in the mods
        // folder (a .htc as the pack it unpacks into).
        void add_options(recomp::config::Config& config);
        // Turns on the pack chosen in the settings and the others off (unless it's left to the
        // Mods menu). Needs the runtime to have opened the mods.
        void apply();
    }

    // rumble.cpp: the Rumble Pak, sent to each port's controller.
    namespace rumble {
        // The runtime's rumble callback: the game turning a port's Rumble Pak on or off. (0.10.4: rumble is off for good: nothing is
        // sent to the controllers.)
        void set(int port, bool on);
        // Every VI: nothing (0.10.4).
        void update();
    }

    // conker_config.cpp: the settings tabs.
    void init_config();

    // look_aim.cpp: gyro and mouse in the look mode (hold R), and how each input moves the view.
    namespace look_aim {
        // Its settings: the camera's and the stick's aiming on the Camera tab, the mouse's and
        // gyro's on the Mouse & Gyro tab.
        void add_camera_options(recomp::config::Config& config);
        void add_mouse_gyro_options(recomp::config::Config& config);
        // Called on every input poll: queues its mouse and gyro movement for the look mode.
        void on_input_poll();
        // The Mouse: Turn the Camera setting: whether the mouse turns the third-person camera
        // (mouse_camera.cpp).
        bool mouse_turns_camera();
        // The Right Stick: Free Camera setting (issue #65), in single player: whether the right stick
        // turns the third-person camera (mouse_camera.cpp) instead of pressing the C-buttons.
        bool stick_free_camera();
        // Aiming: Swap Sticks (issue #84): whether a player (0 to 3) is aiming now (either aiming mode
        // ran for their camera in the last few frames) with the setting on, so their controller's sticks
        // are swapped (frontend.cpp): the right stick aims and the left moves. Not with recompinput's own
        // multiplayer mode (players assigned in its menus), which maps each controller itself.
        bool swap_sticks_now(int player = 0);
        // Online, with Right Stick: Free Camera on, a player's controller aims with the right stick and moves with the left
        // while they aim in R-Look (frontend.cpp swaps the sticks for what this machine sends, and the swap travels with the
        // frame's input, see netsession::swap_flag, so every machine acts on it at the same frame).
        bool online_swap_enabled();
        // Whether either aiming mode ran for the player's camera in the last few frames (the setting needn't be on).
        bool aiming_recently(int player);
        // The stage of a player's look mode (+0x73C of the camera: 0 not started, 1 moving in, 2 aiming, 3 leaving), as of the last view.
        int look_stage(int player);
        // 0.10.13: the online hooks' state between frames back to how it starts (with a resync, on every machine at the same frame).
        void reset_online_state();
        // Online: whether the player's camera ran the second aiming mode (the sniper rifle's, the revolver's, the knives': not the
        // look mode's records) in the last few frames, from the game's own update on every machine (conker_aim_stick).
        bool second_aim_recent(int player);
        // Swap Sticks online, no weapon: the C-buttons the left stick pressed (buttons, as agreed) turned into the game's own strafe,
        // facing set so it goes the way pressed from the view. Called for the game's read of the controller only.
        void no_weapon_walk(uint8_t* rdram, int port, uint16_t* buttons);
        // Swap Sticks online, the tommy gun's R-Look: the left stick to the side walks him (C-Up for the game, the way pressed kept).
        void gun_side_walk(uint8_t* rdram, int port, uint16_t* buttons);
        // What the last read of this machine's controller did (set by frontend.cpp), for net_session.cpp to send along.
        void set_sampled_swap(bool swapped);
        // R is held online but the game's look mode hasn't started (about a second): the right stick does nothing yet.
        void set_entry_hold(bool hold);
        bool entry_hold();
        bool sampled_swap();
        // This machine's aiming settings that change what the game does (look_aim.cpp's look_flag bits), for its input online.
        uint8_t sampled_look_flags();
        // Whether the player's character is aiming the shotgun (state 0x3B), where Z (the laser sight) makes
        // C-Left and C-Right turn the aim: the swapped left stick leaves them out while Z is held (frontend.cpp).
        bool z_turns_aim(int player);
        // The bank's slow motion leaps (issue #85): from the game thread as a frame starts, whether
        // player 1's Conker is leaping; whether he was, from any thread; and, from the input thread,
        // the mouse's tilt added to player 1's stick while he is (the mouse moves the crosshair).
        void leap_frame(uint8_t* rdram);
        bool leaping_now();
        void leap_stick(float& x, float& y);
        // Camera: Invert Turning, for the free camera's stick: turning (x) and tilting (y).
        void free_camera_invert(bool& x, bool& y);
        // Camera: Turning Speed, 1 at 100%.
        float camera_turn_speed();
        // Camera: Field of View: the normal camera's vertical field of view, degrees.
        float camera_field_of_view();
    }

    // conker_config.cpp: whether the Graphics tab's settings exist (not with --headless, which makes
    // no menus). Their readers fall back to the defaults without them.
    bool graphics_config_ready();

    // Graphics: Overscan Borders (conker_config.cpp; the crop is set in widescreen.cpp).
    namespace overscan_borders {
        // Hidden: the picture is zoomed so the game's black border at its sides is off the screen.
        bool hidden();
    }

    // Graphics: Conker's Detail (conker_config.cpp; the model is picked in model_detail.cpp).
    namespace model_detail {
        // Always Highest: Conker stays on his most detailed model in single player.
        bool always_highest();
    }

    // interpolation.cpp: shadows aren't drawn on their first game frame back after a gap (issue #75).
    namespace shadows {
        // From the game thread, as the game starts a frame's display list.
        void game_frame();
    }

    // reticle.cpp: Aiming: Reticle, a ring in the middle of the screen while aiming (issue #74).
    namespace reticle {
        // look_aim.cpp: whether the setting is on.
        bool enabled();
        // From the game thread, each frame a camera aims in the second aiming mode (conker_aim_stick,
        // $s0 the camera state): player 1's shows the reticle, unless zoomed in or in split screen.
        void aim_frame(uint8_t* rdram, gpr camera);
        // The same each frame of the look mode (conker_look_targets), with its state: only the
        // slingshot's and the throwing knives' show the reticle, not plain R-Look's.
        void look_frame(uint8_t* rdram, gpr camera, uint32_t state);
        // From the launcher's init (frontend.cpp): recompui's UI exists now, so the reticle can be made.
        void on_ui_ready();
        // On the main thread (update_gfx): shows or hides the reticle.
        void update();
    }

    // local_achievements.cpp: the program's own achievements, for reaching the game's scenes, kept on this computer.
    namespace local_achievements {
        // At start (frontend.cpp, once the config folder is known): reads the unlocks so far.
        void init();
        // With the other settings tabs (conker_config.cpp): the Achievements tab.
        void add_tab();
        // From the game thread, as the game starts a frame's display list: unlocks what the save has reached.
        void game_frame(uint8_t* rdram);
        // Save states (save_states.cpp), as one is loaded: the scenes it has are put back with the game's memory,
        // so they count for nothing, as a save's do.
        void state_loaded();
    }

    // local_audio.cpp (0.10.96): a voice the host's notes took off its slot while it still plays on this machine, to be stopped (any thread)
    namespace localaudio {
        void stop_detached(uint32_t voice, uint16_t sound);
    }

    // net_menu.cpp: the online pause menu, the game's own pause screen run while the game goes on (see the file).
    namespace netmenu {
        void set_open(bool open);
        bool is_open();
        // (0.10.88) for the view (net_view.cpp): the menu is open, or its buttons were drawn into this frame's list (it closed since)
        bool open_for_view();
        void view_frame_begin();
        // The controller of the player who has the menu open, in a player slot nobody uses (stick: -80 to 80).
        void set_input(int slot, uint16_t buttons, int x, int y);
        // The menu reads the slot of a player in the game (all four are in): that slot's own input is kept and put back (net_menu.cpp).
        void set_borrowed(bool borrowed);
        // Online, the boot's black screen on the way to the multiplayer menu (0.9.80): while it's held, every controller reads as
        // nothing (0.9.81, net_session.cpp's input, online only: it doesn't check).
        bool boot_screen_held(uint8_t* rdram);
        // 0.10.31: online, the co-op campaign stage (net_menu.cpp, conker_coop_start) is being played, with two or more players: the
        // online pause menu takes it as a match, and the view shows the first camera's picture while a cutscene plays.
        bool coop_running(uint8_t* rdram);
        // (0.10.60) this frame's HUD drawn for player k (1-3) in co-op: where its commands are in the display list
        bool coop_hud_range(int k, uint32_t* start, uint32_t* end);
        void coop_hud_clear();
        // (0.10.75) the game thread is in the pause screen's drawing (func_151EB06C, func_151DDC20) with this machine's menu open: what
        // it does there is this machine's alone (heap_watch.cpp gives its allocations a pool of their own; its sounds aren't played)
        bool local_pause_drawing();
    }

    // notices.cpp: a line in the top-left corner for a few seconds, one at a time (an unlock, a save state's
    // "State saved"...).
    namespace notices {
        // From any thread. With replace, it's shown at once in place of what's shown (and of an earlier one to
        // replace), as a save state's slot is as F6 is pressed again and again.
        void show(const std::string& text, bool replace = false);
        // From the launcher's init (frontend.cpp): recompui's UI exists now, so the line's context can be made.
        void on_ui_ready();
        // On the main thread (update_gfx): shows the waiting lines, one at a time, once the game has started.
        void update();
    }

    // netplay.cpp: netplay milestone 1, the determinism test harness (record a play session's inputs and a
    // hash of the game's memory per frame, then replay it and report where the game stops matching). Does
    // nothing unless CONKER_NETPLAY_RECORD or CONKER_NETPLAY_REPLAY is set in the environment.
    namespace netplay {
        // From the game's init (main.cpp), with the game's memory.
        void init(uint8_t* rdram);
        bool active();
        // The online session (net_session.cpp) turns the same frame-length and clock fixes on.
        void enter_online(uint8_t* rdram);
    // Each call to the game's RNG (random_u32 / random_float), with the address it was called from; see netplay.cpp.
    void on_rng_call(uint32_t caller, uint32_t state);
        // Every port read, before it is answered (frontend.cpp): the first of a pass (one game frame) starts a frame.
        void on_port_read(int port);
        // True if the replay answers this port; false means ask the real device.
        bool replay_input(int port, bool* plugged, uint16_t* buttons, float* x, float* y);
        void record_input(int port, bool plugged, uint16_t buttons, float x, float y);
        // The same for which controllers are plugged in.
        bool replay_device(int controller, bool* present);
        void record_device(int controller, bool present);
    }

    // net_session.cpp: netplay milestone 2, the online session (lockstep: every machine runs the whole game and
    // only the controllers travel). Off unless CONKER_NET_HOST or CONKER_NET_JOIN is set; see net_session.cpp.
    namespace netsession {
        // With the game's memory and a function that reads this machine's own controller (port 1's device).
        // True if a session is being set up.
        bool init(uint8_t* rdram, bool (*local_sampler)(uint16_t* buttons, float* x, float* y));
        // The game the lobby would start (its save file is the one the host sends every player).
        void set_game_id(const std::u8string& game_id);

        // The lobby (net_lobby.cpp, the launcher's Online screen): the session is set up before the game starts. The host
        // opens it and waits for the players, a client joins; when the host presses Start every machine starts the game and
        // init() above finds the session ready.
        struct LobbySettings {
            bool host = true;
            std::string address;     // a client's: the host's address, with or without :port
            std::string nickname;    // how this player is called on the screen (empty: Player N)
            int port = 47000;
            int players = 2;         // the host's: how many play (2 to 4)
            int delay = 0;           // the host's: frames of input delay (0: Auto, from the round trips, changed during the game)
            int input = 0;           // 0 = keyboard and gamepad, 1 = keyboard only, 2 = gamepad only
        };
        struct LobbyStatus {
            int phase = 0;           // 0 nothing, 1 connecting, 2 in the lobby, 3 it didn't work (message says why)
            bool host = false;
            int slot = 0;
            int expected = 2;
            int delay = 3;
            bool auto_delay = false; // the delay is worked out from the round trips (delay: what it is now)
            int mask = 0;            // a bit for each player slot that is in
            std::array<std::string, 4> names;   // their nicknames (empty: none chosen)
            std::string log_path;    // the log file the session writes (netplay_host.txt or netplay_join.txt), once open
            bool can_start = false;  // host: everybody is here
            bool dedicated = false;  // the session is a dedicated server (slot 0 is the leader, not a host)
            bool launch = false;     // client: the host pressed Start
            std::string message;
            std::array<float, 4> ping_ms{ -1.0f, -1.0f, -1.0f, -1.0f };   // round trip to each player (a client: to the host), ms; -1: not known yet
        };
        // Text chat during a game (net_chat.cpp is its screen).
        bool chat_available();
        bool chat_send(const std::string& text);
        // The game's fatal-error trap fired: what is known goes to the netplay log.
        void note_halt(uint8_t* rdram, uint32_t vram, uint32_t return_address);
        // The lines the /network command shows (only on this machine): the round trip to the host (the host: to each player).
        std::vector<std::string> network_report();
        // The lines that have come in since the last call: (player slot, text).
        std::vector<std::pair<int, std::string>> chat_poll();
        void lobby_open(const LobbySettings& settings);
        void lobby_leave();
        LobbyStatus lobby_status();
        // Host: begins the game for everybody. False if not everybody is here.
        bool lobby_start();
        bool active();
        // The game frame that has just started, and this machine's player slot (0 is the host, port 1).
        uint32_t frame();
        // 0.10.13: counts the times the hooks' own state (what they keep between frames, outside the game's memory) started over:
        // with a resync, on every machine at the frame after the resync's order (see hook_state_reset in net_session.cpp).
        uint32_t hook_generation();
        // (0.10.90) CONKER_NET_DIAG=1: the heavy test logging and the memory comparisons (off in a normal game)
        bool diag_on();
        int local_slot();
        // (0.10.92) In a multiplayer match the game's players are the ones who joined on the character select, in the order they
        // joined (D_8008FE44): player k (its character, its camera k, +0x23D of the camera) plays with controller D_8008FE44[k],
        // which online is that machine's slot. With every slot in, in order, the two are the same; with fewer (players 1 and 3 of
        // four) they aren't. Outside a match, or with nothing to go by, they're the same.
        int player_of_port(int port);
        int port_of_player(int player);
        // this machine's player in the game (its camera, its character): player_of_port(local_slot())
        int local_player();
        // (0.10.95) the player this machine shows and hears: its own, or for a machine whose player isn't in the match (a watcher) the
        // one it watches (player 1 at first; the D-pad's left and right go through the others, spectate_poll)
        int view_player();
        void spectate_poll();
        // The name a player goes by on the screen: their nickname, or "Player N".
        std::string player_name(int slot);
        // Online: whether that player's right stick turns their camera this frame (the frame's agreed input), and by how
        // much (yaw and pitch, radians).
        bool camera_turn(int player, float* yaw, float* pitch);
        // Online: whether that player's sticks are swapped this frame (aiming with the right stick, moving with the
        // left), from the frame's agreed input.
        bool swap_flag(int player);
        // Online: that player's aiming settings (look_aim.cpp's look_flag bits), from the frame's agreed input.
        uint8_t look_flags(int player);
        // The game's read of a port (input, and Swap Sticks' no weapon walking).
        bool input_for_game(int port, bool* plugged, uint16_t* buttons, float* x, float* y);
        // Every port read, before it is answered: the first of a pass waits for the frame's agreed inputs.
        void on_port_read(int port);
        // True if the session answers this port; false means ask the real device.
        bool input(int port, bool* plugged, uint16_t* buttons, float* x, float* y);
        bool device_present(int controller, bool* present);
        // CONKER_NET_INPUT: 1 = this machine's player uses only the keyboard (and mouse), 2 = only the gamepad,
        // 0 = both (the default).
        int input_filter();
        // A line for the session's log (CONKER_NET_LOG), if there is one.
        void log(const char* line);
        // The game asked for a random number: where the call came from (for finding why two machines drift).
        void note_rng_call(uint32_t caller, uint32_t state);
        void watch_poll(const char* tag, uint32_t info);
    }

    // menu_skin.cpp: optional PNGs for the launcher (assets/menu_skin/), see that file.
}
namespace recompui { class LauncherMenu; class GameOptionsMenu; class GameOption; }
namespace conker {
    // background_video.cpp: the launcher's background video (assets/menu_skin/background.mp4), decoded on a thread of its own.
    namespace background_video {
        // Starts playing it (looped, no sound). False if there's no such file or it can't be played (then nothing runs).
        bool start(const std::filesystem::path& path);
        // The newest frame, if there's one since the last call (RGBA); also tells the player it's being watched.
        bool take_frame(std::vector<char>& rgba, uint32_t& width, uint32_t& height);
    }

    namespace menu_skin {
        // corner: options that become small buttons in the corner when there is a background picture (Version, Add ROM).
        void apply(recompui::LauncherMenu* menu, recompui::GameOptionsMenu* options, const std::vector<recompui::GameOption*>& corner);
        void update();
    }

    // net_chat.cpp: the chat's screen during an online game (Y opens it).
    namespace netchat {
        // The Online tab of the settings (the chat key).
        void add_options(recomp::config::Config& config);
        void update();
        // True while the player is typing (and for a moment after): their controller is held still.
        bool typing();
    }

    // update_check.cpp: whether a newer release of this build is published on GitHub.
    namespace updates {
        // Starts the check (once a run, on a thread of its own).
        void check_in_background();
        // True once it has found a release newer than this build; the tag and the page to get it from.
        bool newer_available(std::string* tag, std::string* page);
        // (0.10.44) 0 while the check runs (or before it starts), 1 it reached GitHub and found the newest release, 2 it didn't.
        int check_state();
        // (0.10.44) checks again (the launcher's Check for updates), once the last check has finished and nothing is installing.
        void check_again();
        // This build's version (version.hpp).
        const char* current_version();
        // Windows: whether the newer release has a .zip that start_install can fetch.
        bool install_possible();
        // Downloads the release, then (state 4) a script is waiting for the game to close: the caller closes the game.
        void start_install();
        struct Status {
            int state = 0;          // 0 not started, 1 downloading, 2 checking, 3 failed (message), 4 ready: close the game now
            int percent = 0;
            std::string message;
        };
        Status status();
    }

    // hq_ping.cpp (0.10.45): the launcher's PING (0.10.47: to GitHub). keep_going every frame the launcher is up (it pings every 5 s while
    // it's called); last_ms: the last round trip in ms, -1 no answer, -2 none yet.
    namespace hq_ping {
        void keep_going();
        int last_ms();
    }

    // upnp_port.cpp (0.10.46): whether UDP 47000 can be reached from the internet (so this player can host); opens it on the router
    // over UPnP as the launcher starts (CONKER_NO_UPNP=1: only checks).
    namespace upnp {
        struct Status {
            int state = 0;        // 0 checking, 1 open, 2 closed, 3 can't tell
            std::string detail;   // why, in capitals ("UPNP", "CGNAT / DOUBLE NAT", "NO UPNP ROUTER"...)
        };
        void start();
        Status status();
    }

    // window_chrome.cpp (0.10.44): on Windows the launcher's window has no Windows title bar: the launcher draws its own (menu_skin.cpp)
    // and the window is dragged by it (SDL's hit test). Once the game starts the Windows border is back. CONKER_NATIVE_WINDOW=1 keeps it.
    namespace window_chrome {
        // Before the window is made: whether the launcher draws its own title bar, and the window's place when it isn't maximized
        // (its size, smaller on a smaller screen, in the middle).
        bool wanted();
        bool start_rect(bool fill, int* x, int* y, int* width, int* height);
        // After it's made (hidden): the frame taken over, the window put there and shown (maximized or not).
        void attach(SDL_Window* window, bool maximized, int x, int y, int width, int height);
        // True while the launcher draws the title bar.
        bool custom();
        // From the launcher's buttons (any thread): done on the main thread, in update.
        void minimize();
        void toggle_fill();
        // (0.10.45) whether it's maximized (the middle button shows restore then, maximize otherwise)
        bool is_filled();
        // (0.10.50) the mouse is on the middle button (Windows has it, for Snap Layouts, so the launcher doesn't see it)
        bool maximize_hovered();
        // Every frame, on the main thread (frontend.cpp's update_gfx); full_screen: Fullscreen in the graphics settings.
        void update(bool game_started, bool full_screen);
    }

    // net_lobby.cpp: the launcher's Online screen (the lobby), see net_session.cpp's lobby functions.
    namespace netlobby {
        // The Online option was chosen (from any thread).
        void show();
        // Every frame while the launcher is up.
        void update(const std::u8string& game_id);
    }

    // net_view.cpp: the full-screen view online.
    namespace netview {
        // (0.10.79) held while the renderer reads a task's display list (frontend.cpp) and while a snapshot is taken or loaded
        // (save_states.cpp): a resync's load replaced the memory under the renderer as it read a list, which crashed it
        std::mutex& render_mutex();
        // Around the frame's drawing (the hooks in func_1501878C), and at every port read.
        void frame_begin(uint8_t* rdram);
        void frame_end(uint8_t* rdram, uint32_t list_end);
        void port_read();
        // After the renderer has read a task's display list (frontend.cpp's wrapper around send_dl): the words the
        // view rewrote in it are put back, so the game's memory is the same on every machine.
        void render_begin(uint8_t* rdram, uint32_t data_ptr);   // (0.10.69) before the renderer reads a task's list
        void render_done(uint8_t* rdram, uint32_t data_ptr);
        // True while a frame is being drawn with this machine's view: no snapshot is taken or loaded then.
        bool busy();
        // Online with the full-screen view, `camera` (an index into D_800BE628) is this machine's player's: the full-screen
        // camera's bounds (the game's 292 x 216 frame), which what the game draws for `camera` is to fill (widescreen.cpp's
        // circle wipe). False otherwise.
        bool full_view_bounds(uint8_t* rdram, int camera, float& left, float& top, float& right, float& bottom);
        // From the object-drawing hook (interpolation.cpp): object (a game object's index) is being drawn for that camera.
    }

    // save_states.cpp: save states (issue #94), F5 to save, F7 to load, F6 for the next slot.
    namespace save_states {
        // At start (frontend.cpp): the keys, and the runtime's idle moment that states are taken at.
        void init();
        // The headless server (main.cpp --dedicated): only the safe-point hook, which the online session's snapshots need.
        void init_headless();
        // For the online session (net_session.cpp): a state of the game as bytes, or loading one, each done at
        // the next safe point (on the game's idle thread, where done is called). Both fail after a few seconds.
        // False (and done is never called) if another such job is waiting.
        bool net_capture(std::function<void(bool ok, std::vector<uint8_t>&& state)> done);
        bool net_restore(std::vector<uint8_t>&& state, std::function<void(bool ok)> done);
        // The runtime has no graphics task in flight and no message waiting (ultramodern::save_states::is_quiet).
        bool runtime_quiet();
    }

    // fps_counter.cpp: Show FPS, the frame rate counter.
    namespace windowed_size {
        // conker_config.cpp (0.10.49): the Windowed Size setting (Graphics tab): Fill Screen (true) or 1600 x 900.
        bool fill_screen();
    }

    namespace vsync {
        // conker_config.cpp: whether the VSync setting is on (Graphics tab; frontend.cpp hands it to RT64 each frame).
        bool enabled();
    }

    namespace fps_counter {
        // conker_config.cpp: whether the setting is on.
        bool enabled();
        // From the launcher's init (frontend.cpp): recompui's UI exists now, so the counter can be made.
        void on_ui_ready();
        // On the main thread (update_gfx): shows or hides the counter, and updates it.
        void update();
        // From the game thread, as the game starts a frame's display list.
        void game_frame();
    }

    // field_of_view.cpp: Camera: Field of View.
    namespace field_of_view {
        // From updateCullScales_1510B958's return (widescreen.cpp): with the setting widening the
        // camera's view, works the cull scales out for the view it draws.
        void adjust_cull_scales(uint8_t* rdram, uint64_t camera);
    }

    // mouse_camera.cpp: the free orbit camera, turned by the mouse and the right stick.
    namespace mouse_camera {
        // Whether last frame's camera update was the normal camera (where the C-buttons turn it: not
        // R-Look, aiming, cutscenes or other special cameras).
        bool normal_camera();
        // Whether the right stick turned the orbit last frame's camera update could run (the setting
        // on, the normal camera, not R-Look or aiming): the controller's input then leaves the right
        // stick off the C-buttons (frontend.cpp). Safe from any thread.
        bool stick_turns_camera();
        // Online: the local player's turn of the camera from the right stick for the next frame (yaw and pitch in radians, in
        // whole 1/10000), settings applied; false if the stick isn't turning the camera now.
        bool online_turn(float* yaw, float* pitch);
        // Every camera's orbit starts over (after a snapshot is taken or loaded).
        void reset();
    }

    // SDL sound output (audio_output.cpp).
    namespace audio {
        void queue_samples(int16_t* samples, size_t sample_count);
        size_t get_frames_remaining();
        void set_frequency(uint32_t frequency);
    }
#endif
}

#endif
