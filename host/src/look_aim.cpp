// Gyro and mouse aiming in the look mode (hold R, look around with the stick), and a choice of
// smooth or direct response for each of the stick, the mouse and gyro.
//
// func_15120158 runs the look mode once a frame. The stick moves a target yaw and pitch, in
// degrees ($t0 + 0x34 and + 0x38; yaw grows to the left and isn't wrapped), then the pitch target
// is clamped to limits that depend on what Conker is doing (0x15120B44 to 0x15120D28), and then a
// spring (func_15049688) eases the current angles ($s0 + 0x37C yaw, $t0 + 0x3C pitch) toward the
// targets, keeping their velocities in $t0 + 0x18 and + 0x1C. Everything the camera is built from
// is copied from the current angles after that.
//
// Hooks (conker.toml):
//   conker_look_stick_yaw_a/_b and conker_look_stick_pitch, where the stick's turn is stored into
//     the targets (0x15120A14, 0x15120A44, 0x15120A98): each can turn its axis around. The game's
//     stick has X normal and Y inverted (up looks down).
//   conker_look_targets, before the clamp: the mouse and gyro turn the targets, so the game's own
//     limits apply to them just as they do to the stick.
//   conker_look_currents, after the springs (L_15120E8C, where both of their branches meet): for
//     an input set to Direct, the current angles take its movement in the same frame instead of
//     being eased toward it.
//   conker_look_yaw_from_facing(_scaled), where some states work the yaw target out each frame from
//     Conker's facing less an aiming angle the stick turns: the mouse and gyro turn that angle too.
//   conker_aim_stick, in the second aiming mode (func_15126378: the sniper scope, the magnum,
//     throwables): the stick's invert, and the mouse and gyro, scaled by the zoom.
// The mouse and gyro are player 1's only: in multiplayer both modes run for each player's camera.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <mutex>
#include <stdexcept>
#include <type_traits>

#include "recomp.h"

#include "conker.hpp"
#include "det_math.hpp"

#if defined(CONKER_RT64)
#include "librecomp/game.hpp"
#include "recompinput/input_state.h"
#include "recompinput/players.h"
#include "recompui/config.h"
#include "recompui/recompui.h"
#include "util/steam_deck.h"
#endif

namespace {
    // Field offsets (see above).
    constexpr int32_t target_yaw = 0x34, target_pitch = 0x38;      // $t0
    constexpr int32_t yaw_velocity = 0x18, pitch_velocity = 0x1C;  // $t0
    constexpr int32_t current_yaw = 0x37C;                         // $s0
    constexpr int32_t current_pitch = 0x3C;                        // $t0

    // Degrees per pixel of mouse movement at 100% mouse sensitivity.
    constexpr float mouse_degrees_per_pixel = 0.1f;
    // recompinput's gyro delta isn't an angle: it adds up the controller's angular velocity
    // (degrees per second) once per sensor event, without the time between events, so a frame's
    // worth depends on the controller's report rate. Tuned by feel, not derived.
    constexpr float gyro_scale = 1.0f / 60.0f;

    float read_float(uint8_t* rdram, gpr base, int32_t offset) {
        uint32_t word = (uint32_t)MEM_W(offset, base);
        float value;
        std::memcpy(&value, &word, sizeof(value));
        return value;
    }

    void write_float(uint8_t* rdram, gpr base, int32_t offset, float value) {
        uint32_t word;
        std::memcpy(&word, &value, sizeof(word));
        MEM_W(offset, base) = (int32_t)word;
    }

    // Each input poll's mouse and gyro movement, queued so the look mode takes one poll's worth a
    // frame. Measured in the look mode: 30 polls and 30 look updates a second, but on different
    // threads (the game's controller thread polls), so two polls sometimes land between two
    // updates and none before the next. recompinput keeps only the latest poll's movement, and
    // taking that once per update left some frames still and lost others: the view hitched.
    // Queued, nothing is lost, and a second poll waits a frame instead of doubling one. Outside
    // the look mode nothing takes from the queue, so the oldest is dropped past two, rather than
    // saved up and turned into a jump when R is pressed.
    struct Movement {
        float mouse_x, mouse_y, gyro_x, gyro_y;
    };
    std::mutex queue_mutex;
    std::deque<Movement> queue;

    // What conker_look_targets did this frame, for conker_look_currents.
    struct Frame {
        bool targets_moved = false;
        float pitch_target_set = 0.0f;  // the pitch target as conker_look_targets left it
        float direct_yaw = 0.0f;        // the part of the turn from inputs set to Direct
        float direct_pitch = 0.0f;
    } frame;

    // Set by conker_look_yaw_from_facing(_scaled) when this frame's look works its yaw out from the
    // way Conker faces less the aiming angle, for conker_look_targets: how many of the aiming angle's
    // 16-bit units turn the view a degree (0 when it doesn't). And the part of a unit not yet taken.
    float aim_units_per_degree = 0.0f;
    float aim_remainder = 0.0f;

    // Aiming: Swap Sticks (issue #84): when each player's camera (by its player, +0x23D) last ran either aiming
    // mode, from the game thread (steady clock ticks), for the input thread (swap_sticks_now). It counts as
    // aiming a little over two game frames after, so a frame the game takes longer over doesn't swap
    // the sticks back for a poll. Each player's, so in split screen each controller swaps as its player aims.
    // (possess47) the 4 players' and, online, the 5th player's: the possess test's extra camera, its +0x23D 4 (camera_player). What the
    // game's memory gets from Swap Sticks (the walking while aiming, the view on the aim, a hit's hold...) is worked out for all 5 on every
    // machine alike, as the game runs the extra camera on all of them.
    constexpr int aim_players = 5;
    // (possess45) the signals the input thread reads for its own player (when it aims, its look's stage, the shotgun's aim): the 5th
    // player's too (signal_player), so its machine's right stick aims in R-Look.
    constexpr int signal_players = aim_players;
    std::atomic<int64_t> last_aim_ticks[signal_players] = {};
    // The aim's yaw for each player's view this frame, from the look mode (conker_look_currents), while swapped.
    std::atomic<int> look_stage_seen[signal_players] = {};
    // Online, how far each player's look has settled from its start (0 swinging in, 1 settled): what Swap Sticks' view on the aim
    // (conker_look_currents) and the guns' up and down at once (gun_aim_online) are mixed in by. They put the view on the aim every
    // frame, so from the look's second stage its swing in had stopped dead: 0.9.56's snap, and 0.9.53's when the stick was held.
    // Now the game's springs swing the view in, and once it's within a degree of the aim (or 20 frames into the second stage, over
    // the next 10) the view is on the aim again. Worked out from the game's memory on every machine for every player's camera, frame
    // by frame, so the same on all of them.
    float look_settle[aim_players] = {};
    // Whether the first stage's target has been followed from the aim since the stage began (conker_look_stage_one).
    bool stage_one_followed[aim_players] = {};
    // Swap Sticks' view yaw as each player's view was last built, the frame, and its turn from the frame before (conker_look_view).
    struct ViewTurn {
        float yaw = 0.0f, step = 0.0f;
        uint32_t frame = 0;
        bool have = false;
    };
    ViewTurn view_turn[aim_players];
    // Each player's eye (+0x2F8) as their view was last built, and its move from the frame before (conker_look_view).
    struct EyeTrack {
        float eye[3] = {}, step[3] = {};
        uint32_t frame = 0;
        bool have = false;
    };
    EyeTrack eye_track[aim_players];
    // The game's own eye (+0x2F8) before conker_look_view changed it for the view, put back at the next frame's look (conker_look_stage_one):
    // the game works the next eye out from it, and changed it fed back (0.9.62: walking, taking the lift off made the eye drop 25 below,
    // the view tilting up, the upward jolt; 0.9.58 too). Only the view is drawn from the changed one.
    struct EyeRestore {
        bool pending = false;
        uint32_t frame = 0;
        float eye[3] = {};
    };
    EyeRestore eye_restore[aim_players];
    // The lift of the eye at the start of R-Look's second stage (0.9.60 and 0.9.61's [look] lines: the eye's height over the look-at point's
    // went +8.3, 10.9, 10.4, 8.4, 6.0, 3.8, 2.1, 0.9, 0.2 and back in the 9 frames after the first stage ended, the same each time, nothing
    // moved by the stick). Taken off the eye as it's built, scaled to its first frame.
    constexpr float stage_two_lift[] = { 8.3f, 10.9f, 10.4f, 8.4f, 6.0f, 3.8f, 2.1f, 0.9f, 0.2f };
    struct LiftState {
        int stage_two_frames = 0;
        int step = -1;          // the lift's frame being taken off (-1: none)
        float scale = 1.0f;
        float last_height = 0.0f;
        bool have_last = false;
    };
    LiftState lift_state[aim_players];
    int look_stage_two_frames[aim_players] = {};
    // An angle's difference, -180 to 180.
    float angle_difference(float to, float from) {
        float d = std::fmod(to - from, 360.0f);
        if (d > 180.0f) d -= 360.0f;
        if (d < -180.0f) d += 360.0f;
        return d;
    }
    float swap_view_yaw[aim_players] = {};
    bool swap_view_yaw_set[aim_players] = {};
    constexpr auto aim_linger = std::chrono::milliseconds(80);
    int camera_player(uint8_t* rdram, gpr camera) {
        const int player = (int)MEM_BU(0x23D, camera);
        if (player < 4) {
            return player;
        }
        // (possess47) online, the 5th player's extra camera (4), while there is one: made at the same frame on every machine
        return (player == 4 && conker::netsession::active() && conker::possess_test::online_extra_camera()) ? 4 : -1;
    }
    // (possess45) a camera's player for the input thread's signals: camera_player's (possess47: the same for the game's memory now)
    int signal_player(uint8_t* rdram, gpr camera) {
        return camera_player(rdram, camera);
    }
    // Whether a player's sticks are swapped (they aim with the right stick, move with the left) for what the game does about
    // it: offline the setting and the aiming as it was a moment ago; online the flag in the frame's agreed input, which
    // every machine has the same (net_session.cpp), not anything local.
    bool swap_state(int player) {
        if (conker::netsession::active()) {
            return conker::netsession::swap_flag(player);
        }
        return conker::look_aim::swap_sticks_now(player);
    }
    // Whether each player's character is in the shotgun's aiming state (0x3B, +0x4 of the camera's +0x3D0), where Z
    // (the laser sight) makes C-Left and C-Right turn the aim, for the input thread (z_turns_aim).
    constexpr uint8_t shotgun_aim_state = 0x3B;
    std::atomic<bool> shotgun_aiming[signal_players] = {};
    void note_aiming(uint8_t* rdram, gpr camera, int player) {
        if (player >= 0 && player < signal_players) {
            last_aim_ticks[player] = std::chrono::steady_clock::now().time_since_epoch().count();
            const gpr character = (gpr)(int32_t)MEM_W(0x3D0, camera);
            // (possess127) or the look's own kind (+0x23E) the shotgun's: the co-op mansion's players aren't always in Conker's 0x3B
            shotgun_aiming[player] = MEM_BU(0x23E, camera) == shotgun_aim_state ||
                ((((uint32_t)character & 0xFF000000u) == 0x80000000u) && (MEM_BU(0x4, character) == shotgun_aim_state));
        }
    }

    // The look mode's kinds (+0x23E of the camera) a gun is aimed in: 0x39 and 0x09 are what the SMG and the M4 logged
    // (aim_log: nothing else ran while aiming them). In these, at full stick, the yaw turned 4.06 degrees a frame (the
    // aim's angle, worked out from the way Conker faces each frame, so the view had no lag left to right) and the pitch
    // 1.9 (its target, the view's pitch then easing toward it by the spring, 14 frames to settle): up and down felt slow and
    // drunk. So online the stick's pitch is scaled to the yaw's rate, and the view takes the pitch at once. Not a setting:
    // the game's state has to be the same on every machine, so every machine does it, for every player, from the
    // game's own memory.
    constexpr float gun_pitch_scale = 4.06f / 1.9f;
    bool gun_aim_online(uint8_t* rdram, gpr camera) {
        // On again (0.9.57: up and down as before 0.9.53, which the test liked better), but only for up and down: the view's left and
        // right is no longer put on the aim here, and up and down only once the look has settled (look_settle), so R-Look's start
        // swings in as on a Nintendo 64.
        if (!conker::netsession::active()) {
            return false;
        }
        const uint8_t kind = MEM_BU(0x23E, camera);
        return kind == 0x39 || kind == 0x09;
    }

#if defined(CONKER_RT64)
    namespace options {
        const std::string stick_response = "look_stick_response";
        const std::string stick_invert = "look_stick_invert";
        const std::string mouse_response = "look_mouse_response";
        const std::string gyro_response = "look_gyro_response";
        const std::string mouse_invert = "look_mouse_invert";
        const std::string gyro_invert = "look_gyro_invert";
        const std::string camera_turn_invert = "camera_invert_turning";
        const std::string camera_turn_speed = "camera_turn_speed";
        const std::string mouse_camera = "mouse_turns_camera";
        const std::string stick_camera = "stick_free_camera";
        const std::string camera_fov = "camera_field_of_view_degrees";
        const std::string aim_swap_sticks = "aim_swap_sticks";
        const std::string aim_lock_on = "aim_lock_on";
    }

    enum class Response : uint32_t { Smooth, Direct };
    enum class Toggle : uint32_t { On, Off };
    enum class Invert : uint32_t { None, X, Y, Both };

    // The settings are on the Camera and Mouse & Gyro tabs (conker_config.cpp), read from whichever
    // config has them. There are none with --headless, which makes no menus, though the game still
    // asks for the camera's settings (its field of view every frame): reading the General tab's then
    // threw, and the headless run aborted ("General config has not been created yet").
    recomp::config::Config* option_config(const std::string& id) {
        return recompui::config::find_option_config(id);
    }

    // An enum setting, or its first value without the settings (Off for a toggle).
    template <typename T>
    T option(const std::string& id) {
        recomp::config::Config* config = option_config(id);
        if (config == nullptr) {
            if constexpr (std::is_same_v<T, Toggle>) {
                return Toggle::Off;
            }
            return T{};
        }
        return static_cast<T>(std::get<uint32_t>(config->get_option_value(id)));
    }

    // A number setting, or the fallback without the settings.
    double number_option(const std::string& id, double fallback) {
        recomp::config::Config* config = option_config(id);
        return config ? std::get<double>(config->get_option_value(id)) : fallback;
    }

    void apply_invert(Invert invert, float& x, float& y) {
        if (invert == Invert::X || invert == Invert::Both) x = -x;
        if (invert == Invert::Y || invert == Invert::Both) y = -y;
    }
#endif
}

#if defined(CONKER_RT64)
// Smooth is every input's default. Known: with Direct (or the stick's Direct, which makes every
// input direct), gyro now and then hitches turning left and right, never up and down; with Smooth
// throughout it doesn't. Not yet traced: input noise the spring hides, or something the game does
// to the yaw alone.
namespace {
    using EnumOptions = const std::vector<recomp::config::ConfigOptionEnumOption>;
    EnumOptions response = {
        {Response::Smooth, "Smooth", "Smooth"},
        {Response::Direct, "Direct", "Direct"},
    };
    EnumOptions invert = {
        {Invert::None, "None", "None"},
        {Invert::X, "InvertX", "Invert X"},
        {Invert::Y, "InvertY", "Invert Y"},
        {Invert::Both, "InvertBoth", "Invert Both"},
    };
    EnumOptions toggle = {
        {Toggle::On, "On", "On"},
        {Toggle::Off, "Off", "Off"},
    };
    const std::string about =
        "<br /><recomp-color primary>Smooth</recomp-color>: the view eases toward where you aim, as in the original game."
        "<br /><recomp-color primary>Direct</recomp-color>: the view follows it exactly, with no easing.";
}

// The Camera tab, its settings in alphabetical order (possess65).
void conker::look_aim::add_camera_options(recomp::config::Config& config) {
    // (possess65) in alphabetical order
    // (possess112) Aiming: Steady Camera has no setting now: it's always on, for every player online and offline (steady_camera)
    config.add_enum_option(options::aim_swap_sticks, "Aiming: Swap Sticks",
        "Controls aiming like a third-person shooter: while you aim, the right stick aims and the left stick moves, as "
        "the C-buttons do (e.g. the shotgun, the slingshot, the sniper scope, R-Look), in single player, and online with a gamepad (the SMG, the M4...). "
        "Conker keeps facing where you aim and walks the way you push, sideways and back too. With the mouse, the keys "
        "bound to the stick (W, A, S, D) move. With the "
        "shotgun, holding R alone shows the laser sight, so you can move and strafe with it; holding Z as well, the left "
        "stick only moves forward and back, as Z makes C-Left and C-Right turn the aim. "
        "Back to normal once you stop aiming. <recomp-color primary>Off</recomp-color> matches the original game, where "
        "the stick aims and the C-buttons move.",
        toggle, Toggle::Off);
    config.add_enum_option(options::camera_turn_invert, "Camera: Invert Turning",
        "Inverts the camera's left and right turning in single player, with the right stick or C-Left and C-Right. "
        "<recomp-color primary>None</recomp-color> matches the original game. Strafing in multiplayer isn't affected. "
        "Y inverts tilting up and down, with the right stick when Right Stick: Free Camera is on.",
        invert, Invert::None);
    config.add_number_option(options::camera_turn_speed, "Camera: Turning Speed",
        "Sets how fast the camera turns left and right in single player, with the right stick or C-Left and C-Right. "
        "Strafing in multiplayer isn't affected.",
        50.0, 300.0, 5.0, 0, true, 100.0);
    config.add_enum_option(options::stick_camera, "Right Stick: Free Camera",
        "A modern third-person camera: the right stick turns the camera around Conker and tilts it up "
        "and down, instead of pressing the C-buttons, and the left stick moves Conker. The C-buttons' other controls "
        "stay: by default C-Up (first person) on the right stick's click and C-Down on RB. <recomp-color primary>Off</recomp-color> matches the "
        "original game. Only the normal camera: in R-Look, aiming and cutscenes, the right stick is the C-buttons as usual.",
        toggle, Toggle::Off);
    // (possess58) Camera: Field of View taken out: every player sees the game's own field of view (field_of_view.cpp does nothing)

    config.add_enum_option(options::stick_response, "Stick: Aiming Response",
        "How the view follows the stick in R-Look (hold R and look around)." + about +
        " This also applies to the mouse and gyro, as the view catches up with the stick at once.",
        response, Response::Smooth);
    config.add_enum_option(options::stick_invert, "Stick: Invert Aiming",
        "Inverts the stick in R-Look (hold R and look around) and in the second aiming mode (e.g. the sniper scope, the magnum, throwables), separately from the mouse and gyro. <recomp-color primary>Invert Y</recomp-color> is the default and matches the original game: pushing the stick up looks down.",
        invert, Invert::Y);
    // (possess64, possess124, possess139) Aiming: Lock-On taken out of the launcher: the lock-on is off for everyone, online or not, every weapon (conker_aim_lock_on)
}

// The Mouse & Gyro tab. The sensitivities are RecompFrontend's options (same ids, so saved values carry
// over), added here instead of by its General tab to sit with their group; RecompFrontend reads them from
// here (find_option_config). Each input's other settings are hidden while its sensitivity is zero, which
// turns that input off.
void conker::look_aim::add_mouse_gyro_options(recomp::config::Config& config) {
    // (possess58) the mouse taken out for now: none of its settings, so its sensitivity reads as zero (RecompFrontend: no mouse
    // movement, the cursor left free) and its others as their defaults (the mouse turns no camera). Gyro stays.
    constexpr bool mouse_settings = false;
    if (mouse_settings) {
    config.add_percent_number_option(recompui::config::general::options::mouse_sensitivity, "Mouse: Sensitivity",
        "How fast the mouse turns the camera and aims, in R-Look (hold R and look around) and the second aiming mode "
        "(e.g. the sniper scope). <b>Zero turns mouse control off</b> and leaves the cursor free. "
        "Mouse buttons can be bound to controls with the keyboard's controls.",
        recompui::is_steam_deck() ? 50.0 : 0.0);
    config.add_enum_option(options::mouse_camera, "Mouse: Turn the Camera",
        "Whether the mouse turns the camera around Conker, outside R-Look and aiming. Needs Mouse: Sensitivity above zero. "
        "<recomp-color primary>Off</recomp-color> leaves that camera to the stick and C-buttons; the mouse still aims in "
        "R-Look and the second aiming mode (e.g. the sniper scope). Gyro that Steam Input or a controller's own software "
        "sends as mouse movement counts as the mouse.",
        toggle, Toggle::On);
    config.add_enum_option(options::mouse_response, "Mouse: Aiming Response",
        "How the view follows the mouse in R-Look (hold R and look around). Needs Mouse: Sensitivity above zero." + about,
        response, Response::Smooth);
    config.add_enum_option(options::mouse_invert, "Mouse: Invert Aiming",
        "Inverts the mouse in R-Look (hold R and look around) and the second aiming mode (e.g. the sniper scope), separately from the stick and gyro. With <recomp-color primary>None</recomp-color>, moving the mouse up looks up; <recomp-color primary>Invert Y</recomp-color> matches the game's stick, where up looks down.",
        invert, Invert::None);
    }

    config.add_percent_number_option(recompui::config::general::options::gyro_sensitivity, "Gyro: Sensitivity",
        "How strongly gyro aims in R-Look (hold R and look around) and the second aiming mode (e.g. the sniper scope), "
        "on controllers that have it. <b>Zero turns gyro off.</b>"
        "<br /><br /><b>To recalibrate gyro, set the controller down on a still, flat surface for 5 seconds.</b>",
        25.0);
    config.add_enum_option(options::gyro_response, "Gyro: Aiming Response",
        "How the view follows gyro in R-Look (hold R and look around). Needs Gyro: Sensitivity above zero." + about,
        response, Response::Smooth);
    config.add_enum_option(options::gyro_invert, "Gyro: Invert Aiming",
        "Inverts gyro in R-Look (hold R and look around) and the second aiming mode (e.g. the sniper scope), separately from the stick and the mouse. With <recomp-color primary>None</recomp-color>, the view turns the way the controller is turned.",
        invert, Invert::None);

    static std::vector<recomp::config::ConfigValueVariant> off = { 0.0 };
    for (const std::string& id : { options::mouse_camera, options::mouse_response, options::mouse_invert }) {
        if (mouse_settings) {
            config.add_option_hidden_dependency(id, recompui::config::general::options::mouse_sensitivity, off);
        }
    }
    for (const std::string& id : { options::gyro_response, options::gyro_invert }) {
        config.add_option_hidden_dependency(id, recompui::config::general::options::gyro_sensitivity, off);
    }
}
#endif

void conker::look_aim::on_input_poll() {
#if defined(CONKER_RT64)
    Movement m;
    recompinput::get_mouse_deltas(&m.mouse_x, &m.mouse_y);
    // (possess66) gyro taken out of the game: its movement is never read
    m.gyro_x = 0.0f;
    m.gyro_y = 0.0f;
    if (conker::netsession::active()) {
        // Online the game's state is the same on every machine, made from the controllers' buttons and sticks that
        // travel: this machine's mouse and gyro turn nothing in it.
        m = Movement{};
    }
    std::lock_guard lock{queue_mutex};
    queue.push_back(m);
    while (queue.size() > 2) {
        queue.pop_front();
    }
#endif
}

#if defined(CONKER_RT64)
bool conker::look_aim::mouse_turns_camera() {
    return option<Toggle>(options::mouse_camera) == Toggle::On;
}

// Aiming: Lock-On (issue #84). func_15063A38 turns the aim (+0x12 of $a0's +0x31C, an s16) by the stick's X
// each frame, and past +-9000 Conker himself. While +0x84 of that struct is set (an enemy to lock on to: the
// shotgun's zombies), it calls func_150639BC instead, which turns the aim straight toward the target found by
// func_15063390, whatever the stick does; the look mode works the camera's yaw out from the aim (state 0x3B),
// so the view swung with it. At 0x15063A80, $t1 holds that flag, about to be tested: with the setting Off it
// reads as clear, and the stick turns the aim as when there's no target.
extern "C" void conker_aim_lock_on(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    // (possess64, possess124) off for everyone, online or not, the setting gone from the launcher for now: every machine reads the flag
    // as clear for every player, so they all do the same (possess117 had turned it back on; possess124 undoes that).
    // (possess139) possess126's exception for the shotgun (Conker's state 0x3B) taken out: the co-op mansion's players are in 0x90, so it
    // never applied there, and the shotgun's jerks were the strafe's (possess128-132). Off for every weapon, every player, always.
    (void)rdram;
    ctx->r9 = 0;
#endif
}

// Online, a gun's aim across turns 1.5 times as fast (as R-Look's up and down, conker_look_stick_pitch): func_15063A38 adds the
// stick's X times 200, times D_800CC264, over 2000 to the aim (+0x12 of +0x31C) each frame, $t5 at 0x15063AE0. 0.9.54 sped up the
// look's own targets only, which the guns' looks don't turn across (their yaw is worked out from the aim). Whole numbers, the same on
// every machine for every player, from the frame's agreed stick.
extern "C" void conker_gun_aim_turn(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (!conker::netsession::active()) {
        return;
    }
    // (possess122) not the shotgun's aim (Conker's state, +0x4, 0x3B: the co-op mansion's): its turn as on a Nintendo 64, the
    // lock-on's with it ($a0 the character)
    if (((uint32_t)ctx->r4 & 0xFF000000u) == 0x80000000u && MEM_BU(0x4, ctx->r4) == 0x3B) {
        return;
    }
    const int32_t turn = (int32_t)ctx->r13;
    ctx->r13 = (gpr)(int32_t)(turn * 3 / 2);
}

// 0.10.8: hit in R-Look, R held, walking with Swap Sticks. The game knocks Conker back: the frame he's hit it sets his moving angle
// (+0x76 of his object) to his facing (+0x7A) plus half a turn, plays the hit's animation (+0x84) and stands him there while it plays,
// turning his facing toward the moving angle plus half a turn, 0xC00 a frame: his facing as it was, so on a Nintendo 64 he doesn't
// turn (goodrunlook.mp4). Online, with the sticks swapped, the walking (conker_look_targets, and the no weapon walking) set his moving
// angle again the next frame, the way pressed from the view: the game turned him toward its opposite, the view turned with him, the
// walking set the angle from the turned view, and so on: he spun round, 0xC00 a frame, through the whole hit (0.10.7's [look] lines:
// f=3173 the moving angle went from 4A79, the walking's, to D135, the facing 5135 plus half a turn; then 504A, 444A, 384A ... A84A).
// So: the frame the moving angle is found changed from what the walking left it at, by more than 0x1800, to the facing plus half a
// turn (within 0x800), the walking leaves his facing and moving angle to the game until the animation changes (the hit is over), R
// is let go or the look ends (or 90 frames). From the game's memory and the frame's agreed buttons, for every player on every machine.
namespace {
    struct HitHold {
        bool have_written = false;
        uint16_t written_move = 0;
        uint32_t written_frame = 0;
        bool holding = false;
        uint32_t hold_anim = 0;
        uint32_t hold_start = 0;
        bool checked = false;
        uint32_t checked_frame = 0;
        // 0.10.14: the moving angle the hit set, his place last frame, whether he has stood still since the hold began, and the
        // animation last looked at and when it changed
        uint16_t hold_move = 0;
        float last_pos[3] = {};
        bool have_pos = false;
        bool stood = false;
        uint32_t last_anim = 0;
        uint32_t anim_changed = 0;
        bool have_anim = false;
        // 0.10.95: his facing last frame, and how far it turned since
        uint16_t last_face = 0;
        bool have_face = false;
        int32_t turned = 0;
    };
    HitHold hit_hold_state[aim_players];   // (possess47: the 5th player's too)
    // (possess107) the way each player was last walked in the look, and the frames left to keep it after the stick is let go (conker_look_targets)
    uint16_t release_move[aim_players] = {};
    int release_frames[aim_players] = {};
    // (possess109) Aiming: Steady Camera's view, each player's (conker_look_view): its height eased after Conker's, the game's own distance
    // across from him kept slowly (game_distance) and the view's eased after that, his place, and the frame it was last built
    // (possess110: and his last 3 heights, the view's height smoothed from them; the game's own distance eased (base), how far the view's
    // line is clear behind him as it's eased (limit) and the look-at point's height over him last frame (look_over), for the collision's
    // probe; and the view as built this frame, its eye and look-at point and the frame, for conker_steady_render to draw)
    struct SteadyView {
        bool have = false; float height = 0.0f, distance = 0.0f, game_distance = 0.0f; uint32_t frame = 0; float last_x = 0.0f, last_z = 0.0f;
        float heights[3] = {};
        float base = 0.0f, limit = 0.0f, look_over = 106.5f;
        bool built = false; uint32_t built_frame = 0; float eye[3] = {}, at[3] = {};
    };
    SteadyView steady_view[aim_players];
    // (possess109) Aiming: Steady Camera's walking (conker_steady_walk): how fast each player's Conker was last moved each way across the
    // ground (vx, vz: the cosine and sine of the way, in his moving angle's units, times how far), that way and how far, and the frame
    struct SteadyWalk { bool have = false; uint32_t frame = 0; float vx = 0.0f, vz = 0.0f; uint16_t way = 0; float length = 0.0f; };
    SteadyWalk steady_walk[aim_players];
    // (possess109) Aiming: Steady Camera's view across (steady_view_check): each player's as the look left it (his facing less his aim
    // angle, conker_look_currents) and the frame, and the stick's turn of it since (func_15063A38) and the frame of that
    struct SteadyAim { bool have = false; uint16_t view = 0; uint32_t frame = 0; int32_t stick_turn = 0; uint32_t stick_frame = 0; };
    SteadyAim steady_aim[aim_players];
    // (possess109) offline, the game's frames so far (leap_frame), for what's worked out once a frame; online it's the session's frame
    uint32_t offline_frames = 0;
    uint32_t steady_frame_now() {
        return conker::netsession::active() ? conker::netsession::frame() : offline_frames;
    }
    // (possess110) Aiming: Steady Camera's view as it's drawn (conker_steady_render), each player's: the view drawn last frame by its shape
    // (its look-at point from Conker, and its eye from that point: how far across, which way across the ground, how high) and its way the
    // frame before that (for its turn), and the change between the game's own view and Steady Camera's: mode 0 the game's, 1 going over to
    // Steady Camera's, 2 Steady Camera's, 3 going back to the game's; the frames into the change, and how far off the view was as it began;
    // and the frame it was last drawn
    struct ViewShape { float at[3] = {}; float across = 0.0f, way = 0.0f, height = 0.0f; };
    struct SteadyRender {
        bool have_last = false;
        ViewShape last;
        bool have_way_before = false;
        float way_before = 0.0f;
        int mode = 0;
        int frames = 0;
        ViewShape gap;
        bool have_frame = false;
        uint32_t frame = 0;
        bool have_drawn = false;
        uint32_t drawn_frame = 0;
    };
    SteadyRender steady_render[aim_players];
    constexpr int steady_in_frames = 12, steady_out_frames = 10;
    // (possess110) Aiming: Steady Camera's eye through the game's camera collision (conker_steady_probe, conker_steady_probed), each
    // player's: the view's line this frame (its look-at point, a, and the eye's move for each unit across, u: straight back along the
    // aim's yaw, and up or down by its pitch), the eye placed on it at the full distance (reach, b) and not yet answered, and the answer:
    // how far along the line it's clear (clear), whether the collision met anything (met) and how far it kept the eye from where it was
    // placed (kept), the wall's plane's distance along the line (plane) and how far along the line the eye was stopped (along); and the
    // frame
    struct SteadyProbe {
        bool placed = false;
        bool have = false;
        uint32_t frame = 0;
        float a[3] = {}, u[3] = {}, b[3] = {};
        float reach = 0.0f;
        float clear = 0.0f;
        bool met = false;
        float kept = 0.0f, plane = 0.0f, along = 0.0f;
    };
    SteadyProbe steady_probe[aim_players];
    // (possess110) Steady Camera's distance across: the game's own (+0x374: 75 with a gun in the look, where the game's camera stands,
    // possess109's [camera] lines), 40 to 120; with no weapon 91 to 100 (the game's camera stood 91 across in that look and went out to
    // 100 walking, possess109's [steady] lines)
    float steady_base_distance(uint8_t* rdram, gpr camera) {
        uint32_t word = (uint32_t)MEM_W(0x374, camera);
        float value;
        std::memcpy(&value, &word, sizeof(value));
        if (!(value == value)) {
            value = 75.0f;
        }
        if (MEM_BU(0x23E, camera) == 0x00) {
            return std::clamp(value, 91.0f, 100.0f);
        }
        return std::clamp(value, 40.0f, 120.0f);
    }

    void note_move_written(int player, uint16_t move) {
        if (player >= 0 && player < aim_players) {
            HitHold& h = hit_hold_state[player];
            h.have_written = true;
            h.written_move = move;
            h.written_frame = conker::netsession::frame();
        }
    }

    // (recheck: looked at again this frame if not holding, where the game's own update runs, conker_look_stage_one and conker_look_targets,
    // in case the hit came in between; conker_look_view only takes what they found)
    bool hit_holding(uint8_t* rdram, gpr camera, int player, bool recheck) {
        if (!conker::netsession::active() || player < 0 || player >= aim_players) {
            return false;
        }
        HitHold& h = hit_hold_state[player];
        const uint32_t now = conker::netsession::frame();
        if (h.checked && h.checked_frame == now && (!recheck || h.holding)) {
            return h.holding;
        }
        const bool local = player == conker::netsession::local_player();
        const gpr walker = (gpr)(int32_t)MEM_W(0x3D0, camera);
        bool plugged = false;
        uint16_t agreed = 0;
        float ax = 0.0f, ay = 0.0f;
        const bool r_held = conker::netsession::input(conker::netsession::port_of_player(player), &plugged, &agreed, &ax, &ay) && (agreed & 0x0010) != 0;   // (0.10.94: the player's controller)
        if (((uint32_t)walker & 0xFF000000u) != 0x80000000u || MEM_H(0x73C, camera) == 0 || !r_held || !swap_state(player)) {
            if (h.holding && local) {
                conker::netsession::log("[look] the hit's hold ends: the look is over or R let go");
            }
            h = HitHold{};
            h.checked = true;
            h.checked_frame = now;
            return false;
        }
        const bool first_look_this_frame = !(h.checked && h.checked_frame == now);
        h.checked = true;
        h.checked_frame = now;
        const uint16_t move = (uint16_t)MEM_HU(0x76, walker);
        const uint16_t face = (uint16_t)MEM_HU(0x7A, walker);
        const uint32_t anim = (uint32_t)MEM_W(0x84, walker);
        // 0.10.14: his place, and the animation's changes, once a frame
        const float pos[3] = { read_float(rdram, walker, 0x14), read_float(rdram, walker, 0x18), read_float(rdram, walker, 0x1C) };
        float moved_by = 0.0f;
        if (first_look_this_frame) {
            if (h.have_pos) {
                moved_by = std::fabs(pos[0] - h.last_pos[0]) + std::fabs(pos[1] - h.last_pos[1]) + std::fabs(pos[2] - h.last_pos[2]);
            }
            std::memcpy(h.last_pos, pos, sizeof(pos));
            h.have_pos = true;
            if (h.have_anim && anim != h.last_anim) {
                h.anim_changed = now;
            }
            h.last_anim = anim;
            h.have_anim = true;
            h.turned = h.have_face ? (int32_t)(int16_t)(uint16_t)(face - h.last_face) : 0;
            h.last_face = face;
            h.have_face = true;
        }
        if (h.holding) {
            // 0.10.14: not over when the animation changes (a hit's can be followed by another: 0.10.13's logs, player 3 at f=3676, the
            // hit's then another, as he lost his gun, and he spun round in the second), but once the game moves him again (he walks:
            // having stood, he moves; or it turns his moving angle from the hit's), R is let go, the look ends, or 90 frames.
            if (first_look_this_frame && moved_by < 0.05f && now - h.hold_start >= 2) {
                h.stood = true;
            }
            const int32_t from_hit = (int16_t)(uint16_t)(move - h.hold_move);
            const bool walking_again = (h.stood && moved_by > 1.0f) || from_hit > 0x800 || from_hit < -0x800;
            if (walking_again || now - h.hold_start > 90) {
                h.holding = false;
                h.have_written = false;
                if (local) {
                    char line[160];
                    std::snprintf(line, sizeof(line), "[look] f=%u the hit's hold ends after %u frames: facing %04X, moving %04X, animation %X",
                        now, now - h.hold_start, (unsigned)face, (unsigned)move, (unsigned)anim);
                    conker::netsession::log(line);
                }
                return false;
            }
            if (local) {
                char line[140];
                std::snprintf(line, sizeof(line), "[look] f=%u hit, held %u: facing %04X moving %04X", now, now - h.hold_start, (unsigned)face, (unsigned)move);
                conker::netsession::log(line);
            }
            return true;
        }
        // 0.10.14: the hit's moving angle (his facing plus half a turn) with a new animation (the hit's, this frame or the one before),
        // where the walking didn't set it so: it hadn't set it lately (a gun's first stage, a look just started: 0.10.13's logs, player 3
        // at f=3676) or set it more than 0x1800 away
        {
            const int32_t changed = (int16_t)(uint16_t)(move - h.written_move);
            const int32_t from_back = (int16_t)(uint16_t)(move - (uint16_t)(face + 0x8000u));
            const bool walking_set = h.have_written && now - h.written_frame <= 4 && changed <= 0x1800 && changed >= -0x1800;
            const bool new_animation = now - h.anim_changed <= 1;
            // 0.10.95: or the game has already turned him once as a hit's knock back does (0xC00 a frame, toward the moving angle plus half
            // a turn) with a new animation, where the walking had just set the moving angle (0.10.94's logs, player 4 at f=6654: walking
            // in the plain look's first stage, the hit's animation 0x11D, his facing 3A07, 2E07, 2207 ... once round): taken back, his facing
            // as it was and his moving angle that plus half a turn, as the hit sets it on a Nintendo 64, so he doesn't turn.
            const int32_t turn = h.turned < 0 ? -h.turned : h.turned;
            // (only with a hit's animation, 0x11D or 0x25B, the logs' hits in the look: his own turning, the mouse's, never)
            const uint32_t anim_id = anim >> 16;
            if (!h.holding && new_animation && (anim_id == 0x11D || anim_id == 0x25B) && h.have_face && turn >= 0xB00 && turn <= 0xD00 &&
                    first_look_this_frame) {
                const uint16_t before = (uint16_t)(face - (uint16_t)h.turned);
                MEM_H(0x7A, walker) = (int16_t)before;
                MEM_H(0x76, walker) = (int16_t)(uint16_t)(before + 0x8000u);
                h.last_face = before;
                h.holding = true;
                h.hold_anim = anim;
                h.hold_start = now;
                h.hold_move = (uint16_t)(before + 0x8000u);
                h.stood = false;
                if (local) {
                    char line[200];
                    std::snprintf(line, sizeof(line), "[look] f=%u hit in the look, turned %d already: facing back to %04X, moving %04X (it was %04X), animation %X: the walking waits",
                        now, (int)h.turned, (unsigned)before, (unsigned)(uint16_t)(before + 0x8000u), (unsigned)move, (unsigned)anim);
                    conker::netsession::log(line);
                }
                return true;
            }
            // (possess130) not the shotgun's own walking (its look, kind 0x3B, the co-op mansion's): walking back there the game sets his moving
            // angle to his facing plus half a turn with its walk's animation (0x31B with Z, 0x224 without), a hit's to the letter, so the hold
            // stopped the walking for 6 to 19 frames each time he went back (the possess128 test, f=1840, 2380, 2813). There, only a hit's
            // own animation (0x11D, 0x25B, as above) counts.
            const bool shotgun_walk = MEM_BU(0x23E, camera) == 0x3B && anim_id != 0x11D && anim_id != 0x25B;
            if (!walking_set && new_animation && from_back > -0x800 && from_back < 0x800 && !shotgun_walk) {
                h.holding = true;
                h.hold_anim = anim;
                h.hold_start = now;
                h.hold_move = move;
                h.stood = false;
                if (local) {
                    char line[200];
                    std::snprintf(line, sizeof(line), "[look] f=%u hit in the look: moving angle %04X (facing %04X; the walking's was %04X), animation %X: the walking waits",
                        now, (unsigned)move, (unsigned)face, (unsigned)h.written_move, (unsigned)anim);
                    conker::netsession::log(line);
                }
                return true;
            }
        }
        return false;
    }
}

// Online, the look mode's first stage (+0x73C of the camera is 1: the camera moves in for 30 frames, a timer of +0x84A) at the
// start of func_15120158 ($a0 the camera). The camera's yaw there eases toward a target worked out once, when the mode starts
// (func_1511FC60), while Conker turns with the stick: the head turned and the view lagged behind (view_log: the camera still at
// the first direction, then 20 frames of catching up as the stage ended). The target is what the look mode works out each frame
// once it has started, from Conker's facing: kinds the game aims a gun in (the facing, +0x7A of his object, less the aim angle, +0x12
// of +0x3D4, and a quarter turn: 360 degrees to 65536 units), and from Conker's +0x40 less 180 degrees for the plain one
// (kind 0: gone straight to the second stage instead). Here the target and the current yaw are both set to it each frame, so the view is on the aim from the first frame.
extern "C" void conker_look_stage_one(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    // Online, for every player on every machine, from the game's memory (see below).
    constexpr bool snappy = false;
    if (!conker::netsession::active()) {
        return;
    }
    const gpr camera = (gpr)(int32_t)ctx->r4;
    // (0.10.8: a hit in the look, worked out here, at the look's start each frame, before any of the walking's writes: hit_holding)
    hit_holding(rdram, camera, camera_player(rdram, camera), true);
    {
        const int player = camera_player(rdram, camera);
        if (player >= 0 && eye_restore[player].pending) {
            EyeRestore& restore = eye_restore[player];
            restore.pending = false;
            if (conker::netsession::frame() == restore.frame + 1) {
                write_float(rdram, camera, 0x2F8, restore.eye[0]);
                write_float(rdram, camera, 0x2FC, restore.eye[1]);
                write_float(rdram, camera, 0x300, restore.eye[2]);
            }
        }
        if (player >= 0 && MEM_H(0x73C, camera) != 2) {
            look_settle[player] = 0.0f;
            look_stage_two_frames[player] = 0;
        }
        if (player >= 0 && MEM_H(0x73C, camera) != 1) {
            stage_one_followed[player] = false;
        }
    }
    if (MEM_H(0x73C, camera) != 1) {
        return;
    }
    const gpr conker = (gpr)(int32_t)MEM_W(0x3D0, camera);
    const gpr aim = (gpr)(int32_t)MEM_W(0x3D4, camera);
    if (((uint32_t)conker & 0xFF000000u) != 0x80000000u || ((uint32_t)aim & 0xFF000000u) != 0x80000000u) {
        return;
    }
    const uint8_t kind = MEM_BU(0x23E, camera);
    const bool gun_kind = kind == 0x09 || kind == 0x38 || kind == 0x39 || kind == 0x37 || kind == 0x3B || kind == 0x15 || kind == 0x26 || kind == 0x3A;
    if (!snappy) {
        // 0.9.57: the first stage runs as the game's (30 frames, its spring swinging the view in), but its target follows the aim each
        // frame (the second stage's own sum, 0x15120908: Conker's facing less the aim angle and a quarter turn) instead of staying where
        // it was when R was pressed: so the stick can aim from the start, the view swinging after it, and nothing is left to catch up
        // with a jump as the stage ends. Only the target: the view is the game's spring's.
        // 0.9.60: and the view turns with the aim as the stick turns it, its spring only swinging it in from where it was: following the
        // moving target alone, the spring was up to 30 degrees behind it with the stick held, and the second stage's first frame put the view
        // on the aim at once (0.9.59's [look] lines: 40 to 60 degrees in a frame, the jump). Each frame the target's turn since the last is
        // added to the view's yaw as well. Not the stage's first frame: the target there is the game's, worked out when R was pressed.
        if (gun_kind) {
            const int32_t units = (int32_t)MEM_HU(0x7A, conker) - (int32_t)MEM_H(0x12, aim) + 0xC000;
            const float target = (float)units * 0.005493164f;
            const int player = camera_player(rdram, camera);
            if (player >= 0 && stage_one_followed[player]) {
                const float turn = angle_difference(target, read_float(rdram, camera, 0x740 + target_yaw));
                write_float(rdram, camera, current_yaw, read_float(rdram, camera, current_yaw) + turn);
            }
            if (player >= 0) {
                stage_one_followed[player] = true;
            }
            write_float(rdram, camera, 0x740 + target_yaw, target);
        }
        return;
    }
    float yaw;
    if (gun_kind) {
        const int32_t units = (int32_t)MEM_HU(0x7A, conker) - (int32_t)MEM_H(0x12, aim) + 0xC000;
        yaw = (float)units * 0.005493164f;
    } else if (kind == 0x00) {
        // The plain R-look (no gun): the stick turns the view's own target, which only the second stage reads, so for the first
        // stage's 30 frames it did nothing. Go on to the second stage as the first does at its end (set +0x198 of +0x3D4, the stage,
        // and the target is where the view is): its springs go on moving the camera in.
        MEM_B(0x198, aim) = 1;
        MEM_H(0x73C, camera) = 2;
        write_float(rdram, camera, 0x740 + target_yaw, read_float(rdram, camera, current_yaw));
        return;
    } else {
        return;
    }
    write_float(rdram, camera, 0x740 + target_yaw, yaw);
    write_float(rdram, camera, current_yaw, yaw);
    write_float(rdram, camera, 0x740 + yaw_velocity, 0.0f);
#endif
}

// Aiming: Swap Sticks with the shotgun (issue #84): holding R alone shows the laser sight, and Conker walks with
// the gun up, as R and Z do. In his state 0x3B (aiming the shotgun, func_15065A5C's case at 0x15068BF0), Z held
// (0x15068D80) shows the laser, but also makes C-Left and C-Right turn the aim, so he can't strafe with it; R
// alone aims while he walks and strafes, without it. Found comparing the two stances (memory dumps, a tally of
// the functions run, then one at a time), with R and Z:
// - +0x8A of his +0x31C has 0x20 (a copy of the buttons' high byte): the laser's sound and the light on the gun;
// - D_800CC2B0 is set: func_15019130 runs the laser's update (func_150636F0, which works out where the beam ends)
//   only while it is (0x15019340);
// - +0x1B4 of +0x31C has 4: func_150636F0 runs the beam's update (func_151D57F8 with 0) for each player whose 4 is
//   set, then clears it (2 is the shot's, kept: set to 4 alone, the shot's mark went);
// - he walks with 0x31B (func_15065A5C, 0x15068FF8), which keeps the gun up where the laser comes from, where R
//   alone's walk is 0x224 (func_15064B94, its case for state 0x3B, 0x15065184; 0x223 below speed 20).
namespace {
    // Player 1 aiming with R and not Z, the sticks swapped (single player: D_800CC290 is theirs). Only the shotgun's
    // state (0x3B) runs these hooks and reads what's set.
    bool shotgun_laser_on_r(uint8_t* rdram) {
        if (!conker::look_aim::swap_sticks_now(0)) {
            return false;
        }
        const uint32_t held = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800CC290);
        return (held & 0x10) && !(held & 0x2000);
    }

    // As R and Z set them (above), for the laser on R alone.
    void show_shotgun_laser(uint8_t* rdram, gpr conker) {
        if (!shotgun_laser_on_r(rdram)) {
            return;
        }
        const gpr state = (gpr)(int32_t)MEM_W(0x31C, conker);
        MEM_B(0x8A, state) = (int8_t)(MEM_BU(0x8A, state) | 0x20);
        MEM_B(0, (gpr)(int32_t)0x800CC2B0) = 1;
        MEM_B(0x1B4, state) = (int8_t)(MEM_BU(0x1B4, state) | 4);
    }
}

// After R alone's movement in the shotgun's state (func_15063E84 returned, at 0x15069028; $s0 Conker). Also from
// the look mode (conker_look_targets): whichever runs before what draws the laser.
extern "C" void conker_shotgun_laser(uint8_t* rdram, recomp_context* ctx) {
    show_shotgun_laser(rdram, ctx->r16);
}

// R alone's walk, picked in func_15064B94 ($v1 at 0x150651FC, for the animation call at L_15065A10, its speed in
// $f14): 0x224 becomes R and Z's 0x31B. (Returned from the state's case instead, R's movement set 0x224 again each
// frame and the two restarted each other: Conker looked stuck in his walk.)
extern "C" void conker_shotgun_laser_walk(uint8_t* rdram, recomp_context* ctx) {
    if (shotgun_laser_on_r(rdram) && (uint32_t)ctx->r3 == 0x224) {
        ctx->r3 = 0x31B;
    }
}

// Aiming: Swap Sticks (issue #84): the view looks the way the aim does. func_1512C490 (building the view) has
// just copied the look-at point and the eye into the view's (+0x2E0, and +0x2EC from +0x2F8; $s0 the camera,
// before 0x1512C640). The aiming camera's eye follows Conker a little behind, while the look-at point moves with
// him at once: walking with the left stick, the line between them swung, and the view turned with the stick
// (logged: up to 2.4 degrees a frame walking back and to the side, the aim still). With the sticks swapped, the
// look-at point is turned about the eye to the aim's direction across the ground: the view's yaw (from the eye
// toward it, atan2 of z over x) is 270 degrees less the look mode's (+0x37C; measured, within 1.4 degrees
// standing still). Its distance and height stay, and so does the eye.
extern std::atomic<int> walk_dx[aim_players], walk_dy[aim_players];

#if defined(CONKER_RT64)
// (possess101) Aiming: Steady Camera, for a camera: R-Look's second stage, Conker's. (possess112: always on, for every player, online,
// in single player and offline alike; it was a setting, each player's own online, a bit of their look flags.)
// (possess119) Not for the shotgun's look (its kind, +0x23E, 0x3B: the co-op mansion's): the game's own R-Look there, as before Steady
// Camera (possess101), its view, its turning of Conker toward the lock-on's target, his walking and his width.
static bool steady_kind(uint8_t* rdram, gpr camera) {
    return MEM_BU(0x23E, camera) != 0x3B;
}
static bool steady_camera(uint8_t* rdram, gpr camera) {
    const gpr walker = (gpr)(int32_t)MEM_W(0x3D0, camera);
    return MEM_H(0x73C, camera) == 2 && ((uint32_t)walker & 0xFF000000u) == 0x80000000u && steady_kind(rdram, camera);
}

// (possess109) The look record (D_800DBFF0, 0x9A0 each) whose Conker (+0x3D0) an object is, with Aiming: Steady Camera's view on it
// (steady_camera) and its player's sticks swapped (swap_state): its address, or 0. From the game's memory and the frame's agreed input.
static gpr steady_record_of(uint8_t* rdram, gpr object) {
    const uint32_t looks = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
    if ((looks >> 24) != 0x80 || ((uint32_t)object & 0xFF000000u) != 0x80000000u) {
        return 0;
    }
    for (uint32_t i = 0; i < (uint32_t)aim_players; i++) {
        const gpr camera = (gpr)(int32_t)(looks + i * 0x9A0u);
        if ((uint32_t)MEM_W(0x3D0, camera) != (uint32_t)object) {
            continue;
        }
        const int player = camera_player(rdram, camera);
        if (player < 0 || player >= aim_players || !steady_camera(rdram, camera) || !swap_state(player)) {
            continue;
        }
        return camera;
    }
    return 0;
}

// (possess101) func_150495B0, at its start: the game's easing of one value toward a target ($a0 the value, $a1 the target, $a2 its
// speed), which the cameras follow Conker by: the eye (+0x2F8, +0x2FC, +0x300 of the camera) and the point the look-at point is
// worked out from (+0x2BC, +0x2C0, +0x2C4). With Aiming: Steady Camera, in R-Look's second stage, those take their target at once (as
// the function does itself when it would pass it: the value the target, its speed 0), so the camera moves with Conker and nothing
// else (possess97's [look-slope] lines: on a ramp the eye 16 behind his height, the look-at point 9, the aim tilted 4.4 degrees).
// Every camera of the look records (D_800DBFF0, 0x9A0 each), from the game's memory on every machine. (An int: the hook is C.)
extern "C" int conker_steady_ease(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t value = (uint32_t)ctx->r4;
    const uint32_t looks = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
    if ((looks >> 24) != 0x80 || value < looks + 0x2BCu || value > looks + (uint32_t)aim_players * 0x9A0u) {
        return 0;
    }
    const uint32_t record = (value - looks) / 0x9A0u, field = (value - looks) % 0x9A0u;
    const bool position = field == 0x2F8u || field == 0x2FCu || field == 0x300u || field == 0x2BCu || field == 0x2C0u || field == 0x2C4u;
    if (!position || record >= (uint32_t)aim_players) {
        return 0;
    }
    if (!steady_camera(rdram, (gpr)(int32_t)(looks + record * 0x9A0u))) {
        return 0;
    }
    MEM_W(0, ctx->r4) = (int32_t)(uint32_t)ctx->r5;
    MEM_W(0, ctx->r6) = 0;
    return 1;
}

#else
extern "C" int conker_steady_ease(uint8_t*, recomp_context*) {
    return 0;
}
#endif

extern "C" void conker_look_view(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    const gpr camera = ctx->r16;
    const int player = camera_player(rdram, camera);
    int previous_stage = 0;
    {
        // (possess45: the 5th player's look too, signal_player, for its machine's input; possess47: and what follows, for the game's memory)
        const int signal = signal_player(rdram, camera);
        if (signal >= 0) {
            previous_stage = look_stage_seen[signal];
            look_stage_seen[signal] = (int)MEM_H(0x73C, camera);
        }
    }
    // 0.10.1: knocked out of the look by a hit, R still held. The game then turns Conker, 0xC00 a frame, until he faces away from his
    // moving angle (+0x76 of his object; facing +0x7A): logged three times, the turn ending at the moving angle + 0x8000. Walking in the
    // look his moving angle is where he walks (Swap Sticks' strafe sets it from the view, conker_look_targets), so he spun round, nearly
    // half a turn; standing still he didn't turn. From the frame the look ends until it is back (or R is let go), his facing is kept
    // as it was and his moving angle is set away from it, so the turn has nowhere to go: as standing still. Each look record on its own
    // (a player can have two), by the game's memory and the frame's agreed buttons, on every machine.
    // (possess47: the 5th player's too, its look record the extra camera's, the 5th)
    if (conker::netsession::active() && player >= 0 && player < aim_players) {
        const uint32_t looks = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
        const uint32_t at = (uint32_t)camera;
        if ((looks >> 24) == 0x80 && at >= looks && (at - looks) % 0x9A0u == 0 && (at - looks) / 0x9A0u < (uint32_t)aim_players) {
            const uint32_t record = (at - looks) / 0x9A0u;
            static bool armed[aim_players] = {};
            static bool holding[aim_players] = {};
            static uint16_t kept_face[aim_players] = {};
            static uint32_t knock_generation = 0;
            if (knock_generation != conker::netsession::hook_generation()) {
                knock_generation = conker::netsession::hook_generation();
                for (int k = 0; k < aim_players; k++) {
                    armed[k] = false;
                    holding[k] = false;
                    kept_face[k] = 0;
                }
            }
            const int stage_now = (int)MEM_H(0x73C, camera);
            bool plugged = false;
            uint16_t agreed = 0;
            float ax = 0.0f, ay = 0.0f;
            const bool r_held = conker::netsession::input(conker::netsession::port_of_player(player), &plugged, &agreed, &ax, &ay) && (agreed & 0x0010) != 0;   // (0.10.94: the player's controller)
            const gpr knocked = (gpr)(int32_t)MEM_W(0x3D0, camera);
            const bool knocked_ok = ((uint32_t)knocked & 0xFF000000u) == 0x80000000u;
            if (!r_held || stage_now == 1) {
                armed[record] = false;
                holding[record] = false;
            } else if (stage_now == 2) {
                armed[record] = true;
                holding[record] = false;
            } else if (stage_now == 0 && armed[record] && knocked_ok) {
                if (!holding[record]) {
                    holding[record] = true;
                    kept_face[record] = (uint16_t)MEM_HU(0x7A, knocked);
                    if (player == conker::netsession::local_player()) {
                        char line[140];
                        std::snprintf(line, sizeof(line), "[look] f=%u knocked out of the look, R held: facing kept at %04X (it was moving at %04X)",
                            conker::netsession::frame(), (unsigned)kept_face[record], (unsigned)MEM_HU(0x76, knocked));
                        conker::netsession::log(line);
                    }
                }
                MEM_H(0x7A, knocked) = (int16_t)kept_face[record];
                MEM_H(0x76, knocked) = (int16_t)(uint16_t)(kept_face[record] + 0x8000u);
            }
        }
    }
    // 0.10.7: no weapon, the look's first stage, walking (no_weapon_walk gives the game C-Left or C-Right): his moving angle is the way
    // pressed from his facing. conker_look_targets sets it in the second stage, but the game doesn't run it in the first (0.10.6's
    // [look] lines: the moving angle turned 0xB00 a frame to the strafe's, a quarter turn left, and he strafed left all through the
    // stage). Set here, after the game's turn of it, as conker_look_targets is in the second stage. Every player's camera on every
    // machine, from the game's memory and the frame's agreed buttons.
    if (conker::netsession::active() && player >= 0 && player < aim_players && swap_state(player) && MEM_BU(0x23E, camera) == 0x00 && MEM_H(0x73C, camera) == 1
        && !hit_holding(rdram, camera, player, false)) {
        const int wdx = walk_dx[player].load();
        const int wdy = walk_dy[player].load();
        const gpr walker = (gpr)(int32_t)MEM_W(0x3D0, camera);
        if ((wdx != 0 || wdy != 0) && ((uint32_t)walker & 0xFF000000u) == 0x80000000u) {
            const int32_t offset = (int32_t)std::lround(conker::det::atan2_d(-(double)wdx, (double)wdy) * 32768.0 / 3.14159265358979);
            const uint16_t moving = (uint16_t)((uint32_t)MEM_HU(0x7A, walker) + (uint32_t)offset);
            MEM_H(0x76, walker) = (int16_t)moving;
            note_move_written(player, moving);
        }
    }
    // 0.9.62: online, R-Look's handover from its first stage to its second, for every player's camera on every machine (the view is built
    // from the game's memory alike on all of them). The eye (+0x2F8, and the view's copy of it, +0x2EC: its x and y are copied already,
    // its z after this):
    // - the frame the first stage ends, with Swap Sticks, it goes on moving as it did the frame before (the game skips the look's update
    //   that frame: the eye stood still, then moved twice as far the next, 0.9.61's [look] lines; the view's turn is carried on below);
    // - its lift after (stage_two_lift) is taken off.
    if (conker::netsession::active() && player >= 0) {
        const int stage = (int)MEM_H(0x73C, camera);
        float eye[3] = { read_float(rdram, camera, 0x2F8), read_float(rdram, camera, 0x2FC), read_float(rdram, camera, 0x300) };
        const float raw_height = eye[1] - read_float(rdram, camera, 0x2E4);
        EyeTrack& track = eye_track[player];
        const uint32_t frame_now = conker::netsession::frame();
        const bool continuing = track.have && frame_now == track.frame + 1;
        bool changed = false;
        if (continuing && previous_stage == 1 && stage == 2 && swap_state(player)) {
            for (int k = 0; k < 3; k++) {
                eye[k] = track.eye[k] + track.step[k];
            }
            changed = true;
        }
        LiftState& lift = lift_state[player];
        if (stage != 2) {
            lift.stage_two_frames = 0;
            lift.step = -1;
        } else {
            lift.stage_two_frames++;
            if (lift.step < 0 && lift.stage_two_frames <= 3 && lift.have_last && !steady_camera(rdram, camera)) {   // (possess101: not with Steady Camera)
                const float jump = raw_height - lift.last_height;
                if (jump > 6.5f && jump < 10.0f) {
                    lift.step = 0;
                    lift.scale = jump / stage_two_lift[0];
                }
            }
        }
        lift.last_height = raw_height;
        lift.have_last = true;
        if (lift.step >= 0) {
            eye[1] -= stage_two_lift[lift.step] * lift.scale;
            changed = true;
            lift.step++;
            if (lift.step >= (int)(sizeof(stage_two_lift) / sizeof(stage_two_lift[0]))) {
                lift.step = -1;
            }
        }
        if (changed) {
            EyeRestore& restore = eye_restore[player];
            restore.pending = true;
            restore.frame = frame_now;
            restore.eye[0] = read_float(rdram, camera, 0x2F8);
            restore.eye[1] = read_float(rdram, camera, 0x2FC);
            restore.eye[2] = read_float(rdram, camera, 0x300);
            write_float(rdram, camera, 0x2F8, eye[0]);
            write_float(rdram, camera, 0x2FC, eye[1]);
            write_float(rdram, camera, 0x300, eye[2]);
            write_float(rdram, camera, 0x2EC, eye[0]);
            write_float(rdram, camera, 0x2F0, eye[1]);
            // (the view's z is stored from $at in the branch's delay slot after this, loaded from +0x300 before)
            uint32_t z_bits;
            std::memcpy(&z_bits, &eye[2], sizeof(z_bits));
            ctx->r1 = (gpr)(int32_t)z_bits;
        }
        for (int k = 0; k < 3; k++) {
            track.step[k] = continuing ? eye[k] - track.eye[k] : 0.0f;
            track.eye[k] = eye[k];
        }
        track.frame = frame_now;
        track.have = true;
    }
    // Online, this machine's player's look, its first 45 frames each time (the view's movement after R, for the logs).
    if (conker::netsession::active() && player >= 0 && player == conker::netsession::local_player()) {
        static int frames_logged = -1;
        static int after_logged = 99;   // 0.9.99: the 30 frames after the look ends, to see a hit knock it out (Conker's facing and moving angle)
        static int last_stage = 0;
        const int stage = (int)MEM_H(0x73C, camera);
        if (stage == 0 && last_stage != 0) {
            after_logged = 0;
        }
        last_stage = stage;
        if (stage == 0 && after_logged < 30) {
            after_logged++;
            const gpr after_conker = (gpr)(int32_t)MEM_W(0x3D0, camera);
            if (((uint32_t)after_conker & 0xFF000000u) == 0x80000000u) {
                char line[200];
                std::snprintf(line, sizeof(line), "[look] f=%u after the look %d: kind=%02X face=%04X move=%04X yaw=%.2f state=%02X anim=%X", conker::netsession::frame(), after_logged,
                    (unsigned)MEM_BU(0x23E, camera), (unsigned)MEM_HU(0x7A, after_conker), (unsigned)MEM_HU(0x76, after_conker), read_float(rdram, camera, current_yaw),
                    (unsigned)MEM_BU(0x4, after_conker), (unsigned)(uint32_t)MEM_W(0x84, after_conker));
                conker::netsession::log(line);
            }
        }
        if (stage == 0) {
            frames_logged = -1;
        } else if (frames_logged < 45) {
            frames_logged = frames_logged < 0 ? 1 : frames_logged + 1;
            const gpr t0 = camera + 0x740;
            const gpr look_conker = (gpr)(int32_t)MEM_W(0x3D0, camera);
            const gpr look_aim_state = (gpr)(int32_t)MEM_W(0x3D4, camera);
            const bool look_conker_ok = ((uint32_t)look_conker & 0xFF000000u) == 0x80000000u;
            const bool look_aim_ok = ((uint32_t)look_aim_state & 0xFF000000u) == 0x80000000u;
            char line[500];
            std::snprintf(line, sizeof(line),
                "[look] f=%u n=%d stage=%d kind=%02X 23C=%u state=%02X anim=%X face=%04X aim=%d move=%04X yaw=%.2f->%.2f pitch=%.2f->%.2f a=%.1f->%.1f b=%.1f->%.1f eye=%.1f,%.1f,%.1f at=%.1f,%.1f,%.1f",
                conker::netsession::frame(), frames_logged, stage, (unsigned)MEM_BU(0x23E, camera), (unsigned)MEM_BU(0x23C, camera),
                look_conker_ok ? (unsigned)MEM_BU(0x4, look_conker) : 0u, look_conker_ok ? (unsigned)(uint32_t)MEM_W(0x84, look_conker) : 0u,
                look_conker_ok ? (unsigned)MEM_HU(0x7A, look_conker) : 0u, look_aim_ok ? (int)MEM_H(0x12, look_aim_state) : 0,
                look_conker_ok ? (unsigned)MEM_HU(0x76, look_conker) : 0u,
                read_float(rdram, camera, current_yaw), read_float(rdram, t0, target_yaw),
                read_float(rdram, t0, current_pitch), read_float(rdram, t0, target_pitch),
                read_float(rdram, camera, 0x18C), read_float(rdram, camera, 0x190), read_float(rdram, camera, 0x194), read_float(rdram, camera, 0x198),
                read_float(rdram, camera, 0x2F8), read_float(rdram, camera, 0x2FC), read_float(rdram, camera, 0x300),
                read_float(rdram, camera, 0x2E0), read_float(rdram, camera, 0x2E4), read_float(rdram, camera, 0x2E8));
            conker::netsession::log(line);
        }
    }
    // (possess103) Aiming: Steady Camera: the view's eye at the height the aim alone gives it. possess102's [look-slope] lines (the eye and the
    // look-at point eased at once since possess101): the look-at point stayed 106.5 over Conker, but the eye's height over it went with the
    // ground (on flat ground the view looked down by 0.81 of the look's pitch, 13.5 over the look-at point at 10.39 degrees and 93 across;
    // up a ramp 7 lower, the view tilted up; down one 7 higher, tilted down; standing on the ramp too), the game's camera placing its eye
    // by the ground somewhere not found yet. Here the view's eye (its y as the view has it, +0x2F0; the camera's own left to the game) is put
    // where flat ground has it: over the look-at point by its distance across times the tangent of 0.81 of the look's pitch. Only the aim
    // tilts the view then; Conker's height moves it up and down whole. Every machine alike for every player's camera (det_math).
    // (possess104) and the view's height eased after Conker's, the eye and the look-at point alike (so it never tilts): with both locked to
    // him (possess103) every step of his showed, uneven on a ramp (2.5 and 2.8 a frame by turns, possess103's [look-slope] lines) and
    // stopping dead at its top and bottom. 0.35 of the way a frame (about 5 behind him climbing a ramp), at once past 80
    // (a fall, a comeback). Once a frame for each player's camera, on every machine alike.
    // (possess108) the aim's yaw for Steady Camera's view, as the stick swap's below takes it
    float steady_yaw = (player >= 0 && player < aim_players && swap_view_yaw_set[player]) ? swap_view_yaw[player] : read_float(rdram, camera, current_yaw);
    // (possess105: the swap first, then Steady Camera's view, below)
    if (swap_state(player)) {
    // The eye from where it's copied (+0x2F8): the copy's z is stored after this, in the branch's delay slot.
    const float ex = read_float(rdram, camera, 0x2F8), ez = read_float(rdram, camera, 0x300);
    const float dx = read_float(rdram, camera, 0x2E0) - ex, dz = read_float(rdram, camera, 0x2E8) - ez;
    const float across = std::sqrt(dx * dx + dz * dz);
    if (across >= 1.0f) {
    constexpr float degrees_to_radians = 3.14159265358979f / 180.0f;
    float aim_yaw = swap_view_yaw_set[player] ? swap_view_yaw[player] : read_float(rdram, camera, current_yaw);
    swap_view_yaw_set[player] = false;
    // (online every machine does this for every player's camera, so it has to come out the same on all of them: det_math.hpp)
    const bool online = conker::netsession::active();
    // 0.9.61: online, the frame R-Look's first stage ends, the view goes on turning as it did the frame before. The game skips the look's
    // update that frame (0x151203FC), so with the stick held the view stood still or went back about 5 degrees, and the next frame jumped
    // 20 to 24 to the aim (0.9.60's [look] lines, every look with the stick held). The frame after, it's the game's again.
    {
        ViewTurn& turn = view_turn[player];
        const uint32_t frame_now = conker::netsession::frame();
        const bool continuing = turn.have && frame_now == turn.frame + 1;
        if (online && continuing && previous_stage == 1 && (int)MEM_H(0x73C, camera) == 2) {
            aim_yaw = turn.yaw + turn.step;
        }
        turn.step = continuing ? angle_difference(aim_yaw, turn.yaw) : 0.0f;
        turn.yaw = aim_yaw;
        turn.frame = frame_now;
        turn.have = true;
    }
    // (possess110) Steady Camera's view takes the aim's yaw from here too, so it goes on turning the frame the first stage ends: it stood
    // still that frame and turned twice as far the next (possess109's [steady] lines, the yaw 200.96, 200.96, 217.66 turning at 8.5 a frame)
    steady_yaw = aim_yaw;
    const float yaw = (270.0f - aim_yaw) * degrees_to_radians;
    write_float(rdram, camera, 0x2E0, ex + (online ? conker::det::cos(yaw) : std::cos(yaw)) * across);
    write_float(rdram, camera, 0x2E8, ez + (online ? conker::det::sin(yaw) : std::sin(yaw)) * across);
    }
    }
    // (possess108) Aiming: Steady Camera's view, built whole from Conker and the aim, as a shooter's camera is: the look-at point over Conker
    // (his place across the ground as it is, his height eased: 0.5 of the way a frame, at once past 80), the eye straight behind it along
    // the aim's direction across the ground (the look mode's yaw, as the view's: 270 degrees less it) at the camera's distance, and over it
    // by that distance times the tangent of 0.81 of the look's pitch (possess103: the view on flat ground). The distance is the game's own,
    // 94 across, brought in by the game's camera where it comes in (walls), less his step the frame before (the game's eye trails by it,
    // possess106), quickly in (0.5 of the way a frame) and slowly back out (0.15). possess105 and 106 moved the game's eye instead, and
    // possess106 by the look-at point's step after the stick swap had turned it about the eye: turning while walking, the eye went far out
    // and then came in on him (possess107's video). Nothing of the game's eye but its distance is used now. Once a frame for each player's
    // camera, on every machine alike (det_math).
    const bool steady_now = player >= 0 && player < aim_players && steady_camera(rdram, camera);
    if (player >= 0 && player < aim_players && !steady_now) {
        steady_view[player].have = false;
        steady_view[player].built = false;   // (possess110)
        steady_walk[player].have = false;   // (possess109)
        steady_aim[player].have = false;
    }
    if (steady_now) {
        SteadyView& sv = steady_view[player];
        const gpr walker = (gpr)(int32_t)MEM_W(0x3D0, camera);
        const float cx = read_float(rdram, walker, 0x14), cy = read_float(rdram, walker, 0x18), cz = read_float(rdram, walker, 0x1C);
        // the game's look-at point's height over him (106.5 standing), and its eye's distance across from it
        const float look_over = read_float(rdram, camera, 0x2E4) - cy;
        const float gex = read_float(rdram, camera, 0x2F8), gez = read_float(rdram, camera, 0x300);
        const float gax = cx, gaz = cz;
        const float game_across = std::sqrt((gex - gax) * (gex - gax) + (gez - gaz) * (gez - gaz));
        constexpr float degrees = 3.14159265358979f / 180.0f;
        const float yaw = (270.0f - steady_yaw) * degrees;
        const float fx = conker::det::cos(yaw), fz = conker::det::sin(yaw);   // across the ground, from the eye toward the look-at point
        const uint32_t frame_now = conker::netsession::active() ? conker::netsession::frame() : 0u;
        const bool new_frame = !conker::netsession::active() || frame_now != sv.frame;
        float step_along = 0.0f;
        if (sv.have) {
            const float sx = cx - sv.last_x, sz = cz - sv.last_z;
            if (sx * sx + sz * sz < 50.0f * 50.0f) {
                step_along = sx * fx + sz * fz;
            }
        }
        // (possess110) the distance: the game's own (+0x374, 75 with a gun, where the game's camera stands in the look; 91 to 100 with no
        // weapon, steady_base_distance), eased 0.1 of the way a frame, and no further than the game's own camera collision finds clear along the
        // view's line behind him this frame (conker_steady_probe: Steady Camera's eye put through it as the camera's): in to it at once,
        // so the eye never goes into a wall, and back out 0.15 of the way a frame. As the view drawn is Steady Camera's now
        // (conker_steady_render), possess109's distance went by the game's eye, which trails him (56 to 61 across walking back, 94
        // walking forward), so it came in and went out with every step, and for walls 14 further than where the game's camera stopped.
        const bool restart = !sv.have || std::fabs(cy - sv.height) > 80.0f;
        const float base_now = steady_base_distance(rdram, camera);
        const SteadyProbe& probe = steady_probe[player];
        const bool probed = probe.have && probe.frame == steady_frame_now();
        if (restart) {
            sv.base = base_now;
            sv.limit = probed ? std::min(base_now, probe.clear) : base_now;
        } else if (new_frame) {
            sv.base += (base_now - sv.base) * 0.1f;
            const float clear = probed ? std::min(sv.base, probe.clear) : sv.base;
            sv.limit = clear < sv.limit ? clear : sv.limit + (clear - sv.limit) * 0.15f;
        }
        const float wanted = std::max(8.0f, std::min(sv.base, sv.limit));
        sv.look_over = look_over;
        if (restart) {
            sv.have = true;
            sv.height = cy;
            sv.heights[0] = sv.heights[1] = sv.heights[2] = cy;
            sv.distance = wanted;
            sv.frame = frame_now;
        } else if (new_frame) {
            sv.frame = frame_now;
            // (possess110) his height smoothed over his last 4 frames, weighed 1 2 2 1: up and down a ramp the view starts and stops climbing
            // smoothly over 4 frames (0.8, 2.4, 4.0, 4.8 a frame as he went 5 a frame up one, possess109's [steady] lines), where half the way
            // a frame (possess104 to 109) started at half his climb at once (2.5, 3.5, 4.2) and stopped as sharply, and his uneven steps up
            // a ramp (4.5 and 5.0 by turns) are smoothed out. About 7 below him on a ramp, 5 before.
            sv.height = (cy + 2.0f * sv.heights[0] + 2.0f * sv.heights[1] + sv.heights[2]) / 6.0f;
            sv.heights[2] = sv.heights[1];
            sv.heights[1] = sv.heights[0];
            sv.heights[0] = cy;
            sv.distance = wanted;
        }
        if (new_frame) {
            sv.last_x = cx;
            sv.last_z = cz;
        }
        const float look_pitch = read_float(rdram, camera + 0x740, current_pitch);
        const float ly = sv.height + look_over;
        const float ex = cx - fx * sv.distance, ez = cz - fz * sv.distance;
        const float ey = ly + sv.distance * conker::det::tan(0.81f * look_pitch * degrees);
        // (possess110) as built this frame, for conker_steady_render to draw
        sv.built = true;
        sv.built_frame = steady_frame_now();
        sv.eye[0] = ex;
        sv.eye[1] = ey;
        sv.eye[2] = ez;
        sv.at[0] = cx;
        sv.at[1] = ly;
        sv.at[2] = cz;
        write_float(rdram, camera, 0x2E0, cx);
        write_float(rdram, camera, 0x2E4, ly);
        write_float(rdram, camera, 0x2E8, cz);
        write_float(rdram, camera, 0x2EC, ex);
        write_float(rdram, camera, 0x2F0, ey);
        // (the view's z is stored from $at in the branch's delay slot after this)
        uint32_t z_bits;
        std::memcpy(&z_bits, &ez, sizeof(z_bits));
        ctx->r1 = (gpr)(int32_t)z_bits;
        if (conker::netsession::active() && player == conker::netsession::local_player()) {   // (possess108 diagnostics)
            static uint32_t told = 0;
            static float told_x = 0.0f, told_y = 0.0f, told_z = 0.0f;
            const bool moved = std::fabs(cx - told_x) + std::fabs(cy - told_y) + std::fabs(cz - told_z) > 0.3f;
            if ((moved || (probed && probe.met)) && new_frame && told < 4000) {   // (possess110: and at a wall)
                told++;
                // (possess109: and the game's eye from its pivot, +0x2BC, and the pivot from him)
                const float px = read_float(rdram, camera, 0x2BC), pz = read_float(rdram, camera, 0x2C4);
                const float pivot_from = std::sqrt((px - cx) * (px - cx) + (pz - cz) * (pz - cz));
                const float eye_from_pivot = std::sqrt((gex - px) * (gex - px) + (gez - pz) * (gez - pz));
                // (possess110: the distance's parts: the game's own and as eased, how far the view's line is clear behind him, the collision's
                // answer to the probe, the limit as eased; the game's eye is the probe's now, on the view's line)
                char line[480];
                std::snprintf(line, sizeof(line), "[steady] f=%u conker %.1f %.1f %.1f, view height %.1f; across: the game's own %.1f (eased %.1f), clear %.1f (%s: placed %.1f, met %d, kept %.1f off, the wall's plane %.1f, stopped %.1f along), limit %.1f, the view's %.1f; the game's eye %.1f (%.1f from its pivot, %.1f from him); his step along %.1f; yaw %.2f pitch %.2f",
                    frame_now, cx, cy, cz, sv.height, read_float(rdram, camera, 0x374), sv.base, probed ? probe.clear : -1.0f, probed ? "probed" : "not probed",
                    probe.reach, probe.met ? 1 : 0, probe.kept, probe.plane, probe.along, sv.limit, sv.distance, game_across, eye_from_pivot, pivot_from, step_along, steady_yaw, look_pitch);
                conker::netsession::log(line);
            }
            told_x = cx;
            told_y = cy;
            told_z = cz;
        }
    }
#endif
}

#if defined(CONKER_RT64)
namespace {
    // (possess110) A view's shape about Conker (c): its look-at point from him, and its eye from that point across the ground (how far,
    // which way) and up. det_math, so every machine works it out alike.
    ViewShape view_shape(const float* eye, const float* at, const float* c) {
        ViewShape s;
        for (int i = 0; i < 3; i++) {
            s.at[i] = at[i] - c[i];
        }
        const double ex = (double)eye[0] - (double)at[0], ez = (double)eye[2] - (double)at[2];
        s.across = (float)std::sqrt(ex * ex + ez * ez);
        s.way = (float)conker::det::atan2_d(ez, ex);
        s.height = eye[1] - at[1];
        return s;
    }
    void shape_view(const ViewShape& s, const float* c, float* eye, float* at) {
        for (int i = 0; i < 3; i++) {
            at[i] = c[i] + s.at[i];
        }
        eye[0] = at[0] + s.across * (float)conker::det::sin_d((double)s.way + conker::det::pi / 2.0);
        eye[1] = at[1] + s.height;
        eye[2] = at[2] + s.across * (float)conker::det::sin_d((double)s.way);
    }
    // the shortest turn from one way to another, -pi to pi
    float turn_from(float to, float from) {
        return (float)std::remainder((double)to - (double)from, 2.0 * conker::det::pi);
    }
    ViewShape shape_gap(const ViewShape& from, const ViewShape& to) {
        ViewShape g;
        for (int i = 0; i < 3; i++) {
            g.at[i] = from.at[i] - to.at[i];
        }
        g.across = from.across - to.across;
        g.way = (from.across >= 1.0f && to.across >= 1.0f) ? turn_from(from.way, to.way) : 0.0f;
        g.height = from.height - to.height;
        return g;
    }
    ViewShape shape_plus(const ViewShape& to, const ViewShape& gap, float part) {
        ViewShape s;
        for (int i = 0; i < 3; i++) {
            s.at[i] = to.at[i] + gap.at[i] * part;
        }
        s.across = std::max(0.0f, to.across + gap.across * part);
        s.way = to.way + gap.way * part;
        s.height = to.height + gap.height * part;
        return s;
    }
    uint32_t render_told = 0, width_told = 0, width_told_frame = 0xFFFFFFFFu, width_told_why = 0;   // (possess111: and why)
}

#endif

// (possess110) Aiming: Steady Camera's view, drawn. func_1512C490 makes the view that's drawn (guLookAt, 0x1512C920) from its eye at +0xEC
// of its stack and look-at point at +0xE0, and those it takes from the game's own camera (+0x2F8, +0x2BC) after the game has moved it this
// frame, plus its own moves of the view (the third way to it, L_1512C6F0: up by +0x18C, 5 with a gun and 25 with no weapon, and +0x74C,
// +0x754, +0x194 across, the eye and the look-at point alike): conker_look_view's view (+0x2E0, +0x2EC) only goes back into the game's
// camera as next frame begins (func_15122C5C, 0x15122D2C). So what was drawn was the game's camera, moved on from Steady Camera's: its own
// distance (69 to 81 with a gun as he walked, possess109's [steady] lines, the zooming in and out), its tilt on ramps, and with no weapon its
// own altogether (101.8 across where Steady Camera's was 94), and walking into the look its eye 133 and 143 across and then 97 at once, the
// third frame of the second stage: the jump as the view went into the look (possess109's video, f=16039). Here, as the view is made,
// Steady Camera's view is put in with the game's own moves of it, so it's what's drawn and aimed along (the aim's line, +0x130 of +0x3D4,
// is worked out from these just after): the third way before 0x1512C804, as the view goes into the game's camera for what it works out
// from it (func_1512523C, func_15125594), and the other ways before 0x1512C844 (all of them pass there; done once a frame). Going into
// it, the view goes over from the one drawn the frame before (moved with Conker, its turn going on) in 12 frames, easing in and out;
// leaving it, back to the game's in 10. Every machine alike for every player's camera (det_math, the game's memory, the frame's agreed
// look flags), as the aim's line is the game's.
#if defined(CONKER_RT64)
static void steady_render_view(uint8_t* rdram, recomp_context* ctx, bool third_way) {
    const gpr camera = ctx->r16;
    const int player = camera_player(rdram, camera);
    if (player < 0 || player >= aim_players) {
        return;
    }
    SteadyRender& r = steady_render[player];
    const gpr walker = (gpr)(int32_t)MEM_W(0x3D0, camera);
    if (((uint32_t)walker & 0xFF000000u) != 0x80000000u) {
        r = SteadyRender{};
        return;
    }
    const gpr sp = ctx->r29;
    const float c[3] = { read_float(rdram, walker, 0x14), read_float(rdram, walker, 0x18), read_float(rdram, walker, 0x1C) };
    const float game_eye[3] = { read_float(rdram, sp, 0xEC), read_float(rdram, sp, 0xF0), read_float(rdram, sp, 0xF4) };
    const float game_at[3] = { read_float(rdram, sp, 0xE0), read_float(rdram, sp, 0xE4), read_float(rdram, sp, 0xE8) };
    const uint32_t now = steady_frame_now();
    const SteadyView& sv = steady_view[player];
    const bool ours_now = sv.built && sv.built_frame == now && steady_camera(rdram, camera);
    // (the third way's call drew it this frame already)
    if (!third_way && r.drawn_frame == now && r.have_drawn) {
        return;
    }
    // The game's own moves of the view this frame: the third way's, from its look-at point before them (the stack's +0xC8) to after; the
    // other ways have none.
    float moved[3] = { 0.0f, 0.0f, 0.0f };
    if (third_way) {
        moved[0] = game_at[0] - read_float(rdram, sp, 0xC8);
        moved[1] = game_at[1] - read_float(rdram, sp, 0xCC);
        moved[2] = game_at[2] - read_float(rdram, sp, 0xD0);
    }
    float our_eye[3] = { 0.0f, 0.0f, 0.0f }, our_at[3] = { 0.0f, 0.0f, 0.0f };
    if (ours_now) {
        for (int i = 0; i < 3; i++) {
            our_eye[i] = sv.eye[i] + moved[i];
            our_at[i] = sv.at[i] + moved[i];
        }
    }
    const bool new_frame = !r.have_frame || r.frame != now;
    const bool following = r.have_frame && now == r.frame + 1 && r.have_last;
    const int mode_before = r.mode;
    if (new_frame) {
        // the view drawn last frame, moved with Conker, its turn going on a frame
        ViewShape from = r.last;
        if (r.have_way_before) {
            from.way += turn_from(r.last.way, r.way_before);
        }
        if (ours_now) {
            if (r.mode == 0 || r.mode == 3) {
                r.gap = following ? shape_gap(from, view_shape(our_eye, our_at, c)) : ViewShape{};
                r.mode = 1;
                r.frames = 0;
            }
            if (r.mode == 1 && ++r.frames >= steady_in_frames) {
                r.mode = 2;
            }
        } else {
            if (r.mode == 1 || r.mode == 2) {
                r.gap = following ? shape_gap(from, view_shape(game_eye, game_at, c)) : ViewShape{};
                r.mode = 3;
                r.frames = 0;
            }
            if (r.mode == 3 && ++r.frames >= steady_out_frames) {
                r.mode = 0;
            }
        }
    }
    float eye[3], at[3];
    float part = 0.0f;
    if (r.mode == 2) {
        std::memcpy(eye, our_eye, sizeof(eye));
        std::memcpy(at, our_at, sizeof(at));
    } else if (r.mode == 1 || r.mode == 3) {
        const bool into = r.mode == 1;
        const float t = std::min(1.0f, (float)r.frames / (float)(into ? steady_in_frames : steady_out_frames));
        part = 1.0f - t * t * (3.0f - 2.0f * t);
        const ViewShape to = into ? view_shape(our_eye, our_at, c) : view_shape(game_eye, game_at, c);
        shape_view(shape_plus(to, r.gap, part), c, eye, at);
    } else {
        std::memcpy(eye, game_eye, sizeof(eye));
        std::memcpy(at, game_at, sizeof(at));
    }
    if (r.mode != 0) {
        write_float(rdram, sp, 0xEC, eye[0]);
        write_float(rdram, sp, 0xF0, eye[1]);
        write_float(rdram, sp, 0xF4, eye[2]);
        write_float(rdram, sp, 0xE0, at[0]);
        write_float(rdram, sp, 0xE4, at[1]);
        write_float(rdram, sp, 0xE8, at[2]);
    }
    r.drawn_frame = now;
    r.have_drawn = true;
    if (new_frame) {
        r.have_way_before = following;
        r.way_before = r.last.way;
        r.last = view_shape(eye, at, c);
        r.have_last = true;
        r.frame = now;
        r.have_frame = true;
    }
    // (possess110 diagnostics) this machine's player: the view drawn, the game's and Steady Camera's, as the view goes into the look and out,
    // and every 10th frame in it
    if (new_frame && conker::netsession::active() && player == conker::netsession::local_player() && render_told < 3000
        && (r.mode != mode_before || r.mode == 1 || r.mode == 3 || (r.mode == 2 && now % 10 == 0))) {
        render_told++;
        const ViewShape g = view_shape(game_eye, game_at, c);
        const ViewShape d = view_shape(eye, at, c);
        const ViewShape o = ours_now ? view_shape(our_eye, our_at, c) : ViewShape{};
        constexpr float to_degrees = (float)(180.0 / 3.14159265358979);
        char line[420];
        std::snprintf(line, sizeof(line),
            "[render] f=%u mode %d (%d frames, %.2f of the gap left) stage %d: the game's %.1f across, way %.1f, %.1f up, look-at %+.1f %+.1f %+.1f;"
            " ours %.1f across, way %.1f, %.1f up, look-at %+.1f %+.1f %+.1f; drawn %.1f across, way %.1f, %.1f up; moved %.1f %.1f %.1f (the %s way)",
            now, r.mode, r.frames, part, (int)MEM_H(0x73C, camera), g.across, g.way * to_degrees, g.height, g.at[0], g.at[1], g.at[2],
            o.across, o.way * to_degrees, o.height, o.at[0], o.at[1], o.at[2], d.across, d.way * to_degrees, d.height,
            moved[0], moved[1], moved[2], third_way ? "third" : "another");
        conker::netsession::log(line);
    }
}
#endif

// (possess110) before 0x1512C804, the third way to the view (conker_steady_render_view's)
extern "C" void conker_steady_render(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    steady_render_view(rdram, ctx, true);
#endif
}

// (possess110) before 0x1512C844, every way to the view: the others' (the third's done already)
extern "C" void conker_steady_render_other(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    steady_render_view(rdram, ctx, false);
#endif
}

// (possess110, possess111) Aiming: Steady Camera keeps Conker as wide as he is walking while he looks. func_1507C3E0 works out how wide
// and tall an object is for its collisions; for Conker (L_1507C574) 60 across ($f0), doubled to 120 ($f20 and +0x98 of its stack, the
// two widths it gives back) while his look record (+0x318, $v0) has its look going (+0x73C), and while his +0x31C's +0x197 or +0x1B3 is
// set (+0x1B3: walking with a gun in the look, func_1504CB98). possess110 took off only the first: standing by a wall in the look he
// stayed there, but the frame he started walking he was pushed 30 off it (possess110's [look] lines: 643.3 to 673.3 at the look's 18th
// frame, 643.1 to 673.0 at its 3rd, his walk starting both times), and walking in the look he stayed 30 further off the wall than
// walking (673 against 643, possess110's [pos] lines). Here, where every way through the doubling meets (0x1507C65C, $v1 his +0x31C),
// with Steady Camera on and his look going, both widths are put back to walking's. Every machine alike, from the game's memory and
// the frame's agreed look flags.
extern "C" void conker_steady_width(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    const gpr camera = ctx->r2;
    if (((uint32_t)camera & 0xFF000000u) != 0x80000000u || (uint32_t)MEM_W(0x3D0, camera) != (uint32_t)ctx->r4) {
        return;
    }
    const int stage = (int)MEM_H(0x73C, camera);
    if (stage == 0 || !steady_kind(rdram, camera)) {   // (possess112: Steady Camera always on; possess119: but not the shotgun's look)
        return;
    }
    const float walking = ctx->f0.fl;
    const float worked_out = ctx->f20.fl;
    if (!(worked_out > walking)) {
        return;
    }
    ctx->f20.fl = walking;
    write_float(rdram, ctx->r29, 0x98, walking);
    if (conker::netsession::active() && camera_player(rdram, camera) == conker::netsession::local_player()) {
        // (this machine's player: as each look begins, and each time what doubled it changes)
        const gpr aim = ctx->r3;
        const uint32_t why = (MEM_BU(0x197, aim) != 0 ? 1u : 0u) | (MEM_BU(0x1B3, aim) != 0 ? 2u : 0u);
        const uint32_t now = conker::netsession::frame();
        const bool begins = width_told_frame == 0xFFFFFFFFu || (now != width_told_frame && now != width_told_frame + 1);
        width_told_frame = now;
        if ((begins || why != width_told_why) && width_told < 300) {
            width_told++;
            char line[200];
            std::snprintf(line, sizeof(line), "[steady] f=%u his width kept as walking in the look (stage %d, +0x197 %u, +0x1B3 %u): %.0f, not %.0f, at %.1f %.1f",
                now, stage, (unsigned)MEM_BU(0x197, aim), (unsigned)MEM_BU(0x1B3, aim), walking, worked_out,
                read_float(rdram, ctx->r4, 0x14), read_float(rdram, ctx->r4, 0x1C));
            conker::netsession::log(line);
        }
        width_told_why = why;
    }
#endif
}

// (possess110) Aiming: Steady Camera's eye through the game's own camera collision. func_1512BB10 moves the camera from where it was last
// frame (+0x304: Steady Camera's eye in the look, its copy, +0x2EC, put back into the camera as the frame began, func_15122C5C) toward the
// eye it wants (+0x2F8), stopping it at walls and sliding it along them, and leaves the result in +0x2F8 (the mouse camera's orbit goes
// through it the same way). Since the view drawn is Steady Camera's (conker_steady_render) its eye has to keep out of walls itself: the
// game's camera only kept its own eye out, and that trails Conker (56 to 61 across walking back, 94 walking forward, possess109's [steady]
// lines), so a wall Steady Camera's eye went into was often further back than the game's had reached. Before the collision reads the eye
// (0x1512BD88), conker_steady_probe puts in Steady Camera's own on the view's line this frame (the aim's yaw and pitch as the look has
// them now, his place, his height as the view will have it), at the full distance and a little more; after the collision's sweep
// (func_15044380, 0x1512BEC4: its answer at +0x88 of the stack, $v0 whether it met anything) conker_steady_probed works out how far
// along the line it's clear: to the wall's plane where the line meets it (the plane across where the eye was stopped, facing back along
// what it was kept from: the slide along a slanted wall is off the line, so how far the eye got would go past the wall), and never
// further than how far along the line the eye got. The camera's eye is then put on the line there, so the game's camera (its yaw is
// worked out from its eye after this, func_15125330, and the look's current yaw is that) stays on the view's line. conker_look_view
// comes in to it at once and goes back out slowly. Every machine alike for every player's camera (the game's memory and collision,
// det_math, the frame's agreed look flags).
extern "C" void conker_steady_probe(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    const gpr camera = ctx->r16;
    const int player = camera_player(rdram, camera);
    if (player < 0 || player >= aim_players) {
        return;
    }
    SteadyProbe& probe = steady_probe[player];
    probe.placed = false;
    const gpr walker = (gpr)(int32_t)MEM_W(0x3D0, camera);
    if (((uint32_t)walker & 0xFF000000u) != 0x80000000u || !steady_camera(rdram, camera)) {
        return;
    }
    const SteadyView& sv = steady_view[player];
    const float cx = read_float(rdram, walker, 0x14), cy = read_float(rdram, walker, 0x18), cz = read_float(rdram, walker, 0x1C);
    // the aim's yaw as conker_look_view will take it (the stick swap's, set as the look's update ran, or the look's own)
    const float aim = swap_view_yaw_set[player] ? swap_view_yaw[player] : read_float(rdram, camera, current_yaw);
    constexpr float degrees = 3.14159265358979f / 180.0f;
    const float yaw = (270.0f - aim) * degrees;
    const float fx = conker::det::cos(yaw), fz = conker::det::sin(yaw);
    const float pitch = read_float(rdram, camera + 0x740, current_pitch);
    const float rise = conker::det::tan(0.81f * pitch * degrees);
    // his height as the view will have it this frame (conker_look_view's, from his last 3), and the look-at point over it as last frame
    const bool going = sv.have && std::fabs(cy - sv.height) <= 80.0f;
    const float height = going ? (cy + 2.0f * sv.heights[0] + 2.0f * sv.heights[1] + sv.heights[2]) / 6.0f : cy;
    const float over = going ? sv.look_over : 106.5f;
    const float base = steady_base_distance(rdram, camera);
    probe.reach = std::max(base, going ? std::max(sv.base, sv.distance) : 0.0f) + 6.0f;
    probe.a[0] = cx;
    probe.a[1] = height + over;
    probe.a[2] = cz;
    probe.u[0] = -fx;
    probe.u[1] = rise;
    probe.u[2] = -fz;
    for (int i = 0; i < 3; i++) {
        probe.b[i] = probe.a[i] + probe.u[i] * probe.reach;
    }
    write_float(rdram, camera, 0x2F8, probe.b[0]);
    write_float(rdram, camera, 0x2FC, probe.b[1]);
    write_float(rdram, camera, 0x300, probe.b[2]);
    probe.placed = true;
#endif
}

// (possess110) after the collision's sweep (conker_steady_probe's)
extern "C" void conker_steady_probed(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    const gpr camera = ctx->r16;
    const int player = camera_player(rdram, camera);
    if (player < 0 || player >= aim_players) {
        return;
    }
    SteadyProbe& probe = steady_probe[player];
    if (!probe.placed) {
        return;
    }
    probe.placed = false;
    const gpr sp = ctx->r29;
    // (the sweep's eye is higher by +0x18C with +0x5F0's 0x800 set, 0x1512BDB8)
    const float lift = ((uint32_t)MEM_W(0x5F0, camera) & 0x800u) != 0 ? read_float(rdram, camera, 0x18C) : 0.0f;
    const float px = read_float(rdram, sp, 0x88), pz = read_float(rdram, sp, 0x90);
    // across the ground only: the game's camera keeps its own height in the look (with +0x2C 0x100 the collision's is left, 0x1512BF54)
    const float kx = probe.b[0] - px, kz = probe.b[2] - pz;
    const float kept = std::sqrt(kx * kx + kz * kz);
    const float ox = px - probe.a[0], oz = pz - probe.a[2];
    const float along = ox * probe.u[0] + oz * probe.u[2];
    float plane = probe.reach;
    float clear = probe.reach;
    const bool met = ctx->r2 != 0 && kept >= 0.5f;
    if (met) {
        const float nx = kx / kept, nz = kz / kept;
        const float facing = probe.u[0] * nx + probe.u[2] * nz;
        if (facing > 0.1f) {
            plane = (ox * nx + oz * nz) / facing;
        }
        clear = std::min(plane, along);
    }
    clear = std::clamp(clear, 0.0f, probe.reach);
    probe.clear = clear;
    probe.met = met;
    probe.kept = kept;
    probe.plane = plane;
    probe.along = along;
    probe.have = true;
    probe.frame = steady_frame_now();
    // the camera's eye on the view's line, where it's clear
    write_float(rdram, sp, 0x88, probe.a[0] + probe.u[0] * clear);
    write_float(rdram, sp, 0x8C, probe.a[1] + probe.u[1] * clear + lift);
    write_float(rdram, sp, 0x90, probe.a[2] + probe.u[2] * clear);
#endif
}

bool conker::look_aim::swap_sticks_now(int player) {
    // (Online it's not this setting: see online_swap_enabled.)
    if (option<Toggle>(options::aim_swap_sticks) != Toggle::On || !recompinput::players::is_single_player_mode() || conker::netsession::active()) {
        return false;
    }
    if (player < 0 || player >= signal_players) {
        return false;
    }
    const int64_t since = std::chrono::steady_clock::now().time_since_epoch().count() - last_aim_ticks[player].load();
    return since >= 0 && std::chrono::steady_clock::duration(since) < aim_linger;
}

bool conker::look_aim::z_turns_aim(int player) {
    // (possess128) online the game's side does it (gun_side_walk: the shotgun's strafe as a walk), so the left stick's C-Left and C-Right
    // are sent as they are
    if (conker::netsession::active()) {
        return false;
    }
    return (player >= 0) && (player < signal_players) && shotgun_aiming[player].load();
}

bool conker::look_aim::online_swap_enabled() {
    return conker::netsession::active() && option<Toggle>(options::aim_swap_sticks) == Toggle::On;
}

// Aiming: Swap Sticks online, no weapon, R held (the plain R-look, kind 0, in its second stage): Conker stands still while the view
// looks around, and of the C-buttons the left stick presses only C-Left and C-Right move him (they strafe him: his moving angle,
// +0x76 of his object, a quarter turn from his facing, +0x7A, at speed 34), C-Up and C-Down do nothing. With the left stick (the C-buttons)
// pressed, the game gets C-Left (or C-Right when pressed right) and his facing is the view's, so he is drawn and animated as the
// game's own strafe; the way pressed is kept (walk_dx, walk_dy) and conker_look_targets sets his moving angle to it from the view,
// as with a gun. Everything it goes by is in the game's memory and the frame's agreed buttons.
std::atomic<int> walk_dx[aim_players], walk_dy[aim_players];
// A player holding a grenade that Z started in the no weapon R-look (C-Down held for it, below), from the frame's agreed buttons.
std::atomic<bool> grenade_held[aim_players];

namespace {
    // (possess47) the 5th player's input (slot 4, net_session.cpp's fifth_input) while its extra camera is there: its look record is
    // the extra camera's, the 5th of the block (the game's 4 players' are the first 4)
    bool fifth_walks(int player) {
        return player == 4 && conker::netsession::active() && conker::possess_test::online_extra_camera();
    }
}

void conker::look_aim::no_weapon_walk(uint8_t* rdram, int port, uint16_t* buttons) {
#if defined(CONKER_RT64)
    constexpr uint16_t c_buttons = 0x000F;
    // (0.10.92: the controller's player in the game, whose camera (+0x23D) and walking this is; port is that player from here on)
    port = conker::netsession::player_of_port(port);
    // (possess47: the 5th player's too)
    const bool fifth = fifth_walks(port);
    if (port < 0 || (port >= 4 && !fifth)) {
        return;
    }
    int dx = ((*buttons & 0x0001) ? 1 : 0) - ((*buttons & 0x0002) ? 1 : 0);
    int dy = ((*buttons & 0x0008) ? 1 : 0) - ((*buttons & 0x0004) ? 1 : 0);
    walk_dx[port] = 0;
    walk_dy[port] = 0;
    // Z throws a grenade here (the game holds and throws one with C-Down while R is up, and R-look has R held): from Z's press C-Down is held with R
    // (logged: R and C-Down together start the grenade in the look, with its zoomed view) for as long as Z is, so the grenade is held to blow
    // up in the hand, or thrown on Z's release.
    // Z itself is not passed on.
    static bool grenade_on[aim_players] = {};
    static bool prev_z[aim_players] = {};
    static uint32_t grenade_generation = 0;
    if (grenade_generation != conker::netsession::hook_generation()) {
        grenade_generation = conker::netsession::hook_generation();
        for (int k = 0; k < aim_players; k++) {
            grenade_on[k] = false;
            prev_z[k] = false;
        }
    }
    const bool z_now = (*buttons & 0x2000) != 0;
    const bool z_edge = z_now && !prev_z[port];
    prev_z[port] = z_now;
    grenade_held[port] = grenade_on[port];
    if (grenade_on[port]) {
        if (z_now) {
            *buttons = (uint16_t)((*buttons & ~(c_buttons | 0x2000 | 0x0F00)) | 0x0010 | 0x0004);
            return;
        }
        grenade_on[port] = false;
        grenade_held[port] = false;
        *buttons &= (uint16_t)~c_buttons;
    }
    const uint32_t base = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
    if ((base & 0xFF000000u) != 0x80000000u) {
        return;
    }
    // (possess47: the 5th player's look record is the extra camera's alone)
    for (uint32_t i = fifth ? 4u : 0u; i < (fifth ? 5u : 4u); i++) {
        const gpr camera = (gpr)(int32_t)(base + i * 0x9A0);
        if ((int)MEM_BU(0x23D, camera) != port) {
            continue;
        }
        if (MEM_BU(0x23E, camera) != 0x00) {
            return;
        }
        // (possess148) No weapon out (this look, kind 0: the M4's is 0x39, the sniper rifle's 0x24), R held or in the look: the D-pad's
        // way taken as the left stick's C-buttons' (dx, dy). His moving angle here is set from the way pressed (walk_dx, walk_dy: below
        // and conker_look_targets), the game's strafe going by it and not setting it itself, so the D-pad given to the game's strafe alone
        // (conker_dpad_strafe) left him walking the way the left stick last set (the possess147 test). It's kept from conker_dpad_strafe
        // here. (possess149: possess148 tested the weapon at the camera's +0x3D4's +0x78, not 0 here: it never ran.)
        if (((*buttons & 0x0010) != 0 || MEM_H(0x73C, camera) == 2) && (*buttons & 0x0F00u) != 0) {
            const uint16_t pad = (uint16_t)(*buttons & 0x0F00u);
            dx = ((*buttons & 0x0001) || (pad & 0x0100) ? 1 : 0) - ((*buttons & 0x0002) || (pad & 0x0200) ? 1 : 0);
            dy = ((*buttons & 0x0008) || (pad & 0x0800) ? 1 : 0) - ((*buttons & 0x0004) || (pad & 0x0400) ? 1 : 0);
            *buttons &= (uint16_t)~0x0F00u;
            static uint32_t told = 0;
            if (told < 6 && port == conker::netsession::player_of_port(conker::netsession::local_slot())) {
                told++;
                char line[160];
                std::snprintf(line, sizeof(line), "[strafe] f=%u player %d: no weapon, R-Look: the D-pad walks as the left stick's C-buttons do (%d,%d)",
                    (unsigned)conker::netsession::frame(), port + 1, dx, dy);
                conker::netsession::log(line);
            }
        }
        // 0.10.6: the look's first stage (about a second, as it zooms in), R held: he walks as in the second (below), the game given
        // C-Left or C-Right and his moving angle set from the way pressed (conker_look_targets), so the look doesn't stop him as he
        // goes into it (logged: he slowed to a stop and stood until the second stage; with a gun he walks on). His facing is left
        // as it is: the view is still turning in behind it. C-Down never goes to the game here (with R held it starts a grenade).
        // (0.10.8: a hit in the look playing, hit_holding: nothing from the walking, the game's knock back as it is)
        if (hit_hold_state[port].holding && hit_hold_state[port].checked_frame + 3 >= conker::netsession::frame() && (*buttons & 0x0010) != 0) {
            // (0.10.14: after 12 frames, the walking's C-Left or C-Right, so he walks again once the hit is over and the hold ends)
            const bool late = conker::netsession::frame() - hit_hold_state[port].hold_start >= 12;
            *buttons = (uint16_t)(*buttons & ~c_buttons);
            if (late && (dx != 0 || dy != 0)) {
                *buttons |= (uint16_t)(dx > 0 ? 0x0001 : 0x0002);
            }
            return;
        }
        if (MEM_H(0x73C, camera) == 1 && (*buttons & 0x0010) != 0) {
            if (dx == 0 && dy == 0) {
                *buttons &= (uint16_t)~c_buttons;
                return;
            }
            walk_dx[port] = dx;
            walk_dy[port] = dy;
            *buttons = (uint16_t)((*buttons & ~c_buttons) | (dx > 0 ? 0x0001 : 0x0002));
            return;
        }
        if (MEM_H(0x73C, camera) != 2) {
            // Not in the look yet (or leaving it): the left stick's C-Down with R held starts a grenade, so none is passed on.
            if ((*buttons & 0x0010) != 0) {
                *buttons &= (uint16_t)~c_buttons;
            }
            return;
        }
        const gpr conker = (gpr)(int32_t)MEM_W(0x3D0, camera);
        if (((uint32_t)conker & 0xFF000000u) != 0x80000000u) {
            return;
        }
        if (z_edge && (*buttons & 0x0010) != 0) {
            grenade_on[port] = true;
            grenade_held[port] = true;
            *buttons = (uint16_t)((*buttons & ~(c_buttons | 0x2000)) | 0x0010 | 0x0004);
            return;
        }
        if (z_now) {
            *buttons &= (uint16_t)~0x2000;
        }
        if (dx == 0 && dy == 0) {
            *buttons &= (uint16_t)~c_buttons;
            return;
        }
        walk_dx[port] = dx;
        walk_dy[port] = dy;
        const double yaw_units = (double)read_float(rdram, camera, current_yaw) * 65536.0 / 360.0;
        MEM_H(0x7A, conker) = (int16_t)(uint16_t)((uint32_t)((int64_t)std::llround(yaw_units) - 0xC000) & 0xFFFFu);
        *buttons = (uint16_t)((*buttons & ~c_buttons) | (dx > 0 ? 0x0001 : 0x0002));
        return;
    }
#endif
}


// (possess127) Aiming: Swap Sticks online, the shotgun aimed (the co-op mansion's, its look's kind 0x3B) with Z held: Z (the laser sight)
// makes C-Left and C-Right turn the aim, and with the sticks swapped those are the left stick's strafe, so walking sideways while shooting
// pulled the aim off the zombies in jerks, against the right stick (the possess126 test's inputs: R, Z and C-Right or C-Left together,
// 0x2011, 0x2012, 0x201A, reaching the game, the camera swapping between the shotgun's look and its laser's). The input thread was to
// keep them back (frontend.cpp, z_turns_aim) but goes by Conker's state, 0x3B, which the co-op mansion's players aren't always in. Here
// they're kept back where the game takes the frame's agreed buttons, by the player's own look (its camera's kind, +0x23E, or its
// character's state), the same on every machine. Forward and back (C-Up, C-Down) still walk him.
void conker::look_aim::shotgun_z_strafe(uint8_t* rdram, int port, uint16_t* buttons) {
#if defined(CONKER_RT64)
    if ((*buttons & 0x2000) == 0 || (*buttons & 0x0003) == 0) {
        return;
    }
    const int player = conker::netsession::player_of_port(port);
    const bool fifth = fifth_walks(player);
    if (player < 0 || (player >= 4 && !fifth)) {
        return;
    }
    const uint32_t base = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
    if ((base & 0xFF000000u) != 0x80000000u) {
        return;
    }
    for (uint32_t i = fifth ? 4u : 0u; i < (fifth ? 5u : 4u); i++) {
        const gpr camera = (gpr)(int32_t)(base + i * 0x9A0);
        if ((int)MEM_BU(0x23D, camera) != player) {
            continue;
        }
        const gpr character = (gpr)(int32_t)MEM_W(0x3D0, camera);
        const bool character_ok = ((uint32_t)character & 0xFF000000u) == 0x80000000u;
        const uint8_t kind = MEM_BU(0x23E, camera);
        const uint8_t state = character_ok ? MEM_BU(0x4, character) : 0;
        if (kind != shotgun_aim_state && state != shotgun_aim_state) {
            return;
        }
        static uint32_t told = 0;
        if (told < 12 && port == conker::netsession::local_slot()) {
            told++;
            char line[200];
            std::snprintf(line, sizeof(line), "[shotgun] f=%u player %d: Z held aiming the shotgun (its look's kind %02X, his state %02X): the left stick's "
                "C-%s kept from the game (it turned the aim)", (unsigned)conker::netsession::frame(), player + 1, (unsigned)kind, (unsigned)state,
                (*buttons & 0x0001) ? "Right" : "Left");
            conker::netsession::log(line);
        }
        *buttons &= (uint16_t)~0x0003u;
        return;
    }
#endif
}

// Aiming: Swap Sticks online, the tommy gun's R-Look (kinds 0x38 and 0x3A, The Vault): of the C-buttons the left stick presses only C-Up
// and C-Down walk him there, C-Left and C-Right don't (the test: forward and back, never to the side). Pressed to the side, the game
// gets C-Up instead (with C-Down when pressed back as well), so he walks, and the way pressed is kept (gun_walk_dx, gun_walk_dy) for
// conker_look_targets, which sets his moving angle to it from the view: he strafes, as with the other guns. From the frame's agreed
// buttons and the game's memory, the same on every machine.
std::atomic<int> gun_walk_dx[aim_players], gun_walk_dy[aim_players];
std::atomic<bool> gun_walk_set[aim_players];

void conker::look_aim::gun_side_walk(uint8_t* rdram, int port, uint16_t* buttons) {
#if defined(CONKER_RT64)
    port = conker::netsession::player_of_port(port);   // (0.10.92, as in no_weapon_walk)
    const bool fifth = fifth_walks(port);              // (possess47, as in no_weapon_walk)
    if (port < 0 || (port >= 4 && !fifth)) {
        return;
    }
    gun_walk_set[port] = false;
    // (possess149) and the D-pad's way, as the left stick's C-buttons' (the looks below: his moving angle is set from the way pressed,
    // conker_look_targets; the D-pad then not the game's strafe's as well)
    const uint16_t pad = (uint16_t)(*buttons & 0x0F00u);
    const int dx = ((*buttons & 0x0001) || (pad & 0x0100) ? 1 : 0) - ((*buttons & 0x0002) || (pad & 0x0200) ? 1 : 0);
    const int dy = ((*buttons & 0x0008) || (pad & 0x0800) ? 1 : 0) - ((*buttons & 0x0004) || (pad & 0x0400) ? 1 : 0);
    if (dx == 0) {
        return;
    }
    const uint32_t base = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
    if ((base & 0xFF000000u) != 0x80000000u) {
        return;
    }
    for (uint32_t i = fifth ? 4u : 0u; i < (fifth ? 5u : 4u); i++) {
        const gpr camera = (gpr)(int32_t)(base + i * 0x9A0);
        if ((int)MEM_BU(0x23D, camera) != port) {
            continue;
        }
        const uint8_t kind = MEM_BU(0x23E, camera);
        // (possess128) and the shotgun's look (0x3B, the co-op mansion's, in either of its stages): there C-Left and C-Right turn the aim
        // (the co-op players' state is 0x90, the possess127 test: Z or not), so the left stick's strafe yanked the aim off the zombies;
        // possess127 kept them from the game with Z held, and he couldn't strafe, stopping and starting as the stick went round. Now he
        // walks and strafes as with the M4: the game gets C-Up (C-Down going back) and his moving angle is the way pressed from the view
        // (conker_look_targets), the aim the right stick's alone.
        const bool shotgun = kind == 0x3B && MEM_H(0x73C, camera) != 0;
        if (!shotgun && ((kind != 0x38 && kind != 0x3A) || MEM_H(0x73C, camera) != 2)) {
            continue;
        }
        gun_walk_dx[port] = dx;
        gun_walk_dy[port] = dy;
        gun_walk_set[port] = true;
        *buttons = (uint16_t)((*buttons & ~0x0F0Fu) | (dy < 0 ? 0x0004u : 0x0008u));
        return;
    }
#endif
}

int conker::look_aim::look_stage(int player) {
    return (player >= 0 && player < signal_players) ? look_stage_seen[player].load() : 0;
}

bool conker::look_aim::aiming_recently(int player) {
    if (player < 0 || player >= signal_players) {
        return false;
    }
    const int64_t since = std::chrono::steady_clock::now().time_since_epoch().count() - last_aim_ticks[player].load();
    return since >= 0 && std::chrono::steady_clock::duration(since) < aim_linger;
}

namespace {
    std::atomic<bool> sampled_swap_flag{ false };
}
namespace {
    std::atomic<bool> entry_hold_flag{ false };
}
void conker::look_aim::set_entry_hold(bool hold) {
    entry_hold_flag = hold;
}
bool conker::look_aim::entry_hold() {
    return entry_hold_flag.load();
}
void conker::look_aim::set_sampled_swap(bool swapped) {
    sampled_swap_flag = swapped;
}
bool conker::look_aim::sampled_swap() {
    return sampled_swap_flag.load();
}

bool conker::look_aim::stick_free_camera() {
    return option<Toggle>(options::stick_camera) == Toggle::On && (recompinput::players::is_single_player_mode() || conker::netsession::active());
}

// The bank robbery's slow motion leaps (issue #85): standing on a B pad, B leaps (func_15065A5C, 0x15066FC4: the bank,
// 0x36 in D_800BE9F0, B pressed and +0x2EC of Conker's object 1), and Conker flies across the room in slow motion
// while the stick aims a crosshair. The leap has no aiming mode or camera of its own: Conker stays in the bank's state
// (0x96, +0x4 of his object) with the leap's animation (+0x84: 0x20F, or 0x20E one way round), the follow camera stays
// as it is when walking (its +0x84 0xC0001006, logged through two leaps), and the stick turns into his movement as
// ever (a direction and a speed: +0x4C of his +0x31C, +0x44), which turns him in the air and his guns with him: the
// crosshair is where they point. It moves with the stick, comes back to the middle once it's let go, and the camera
// barely moves (logged: the camera's yaw 5 degrees in a second of the stick, as it eased back behind him anyway).
// The C-buttons turn the camera, as they do when walking: the original game's (logged: 22 degrees in a second of
// C-Right), so with Right Stick: Free Camera off the right stick turns it too.
//
// The mouse turned the camera instead (the orbit, mouse_camera.cpp, runs where the C-buttons turn it) and never moved
// the crosshair. Now, in a leap, the mouse is the stick: its movement moves a tilt, which stays where the mouse leaves
// it (so the crosshair does), added to the stick's own for player 1 (frontend.cpp, leap_stick), and the orbit doesn't
// run (mouse_camera.cpp). The tilt goes back to the middle when the leap ends.
namespace {
    constexpr uint32_t conker_object = 0x800CC2D0;  // gObjects[0]
    constexpr uint8_t bank_state = 0x96;
    constexpr uint16_t leap_animation = 0x20F, leap_animation_other_way = 0x20E;
    // Mouse movement (with Mouse: Sensitivity) for the stick's full tilt: at 100%, 300 pixels of mouse.
    constexpr float leap_full_tilt = 300.0f;

    // From the game thread (leap_frame), for the input thread.
    std::atomic<bool> leaping = false;
    // The mouse's tilt, x and y (-1 to 1, up positive), from the input thread only.
    float leap_x = 0.0f, leap_y = 0.0f;
}

void conker::look_aim::leap_frame(uint8_t* rdram) {
    offline_frames++;   // (possess109)
    const gpr conker = (gpr)(int32_t)conker_object;
    const uint16_t animation = (uint16_t)MEM_HU(0x84, conker);
    leaping = MEM_BU(0x4, conker) == bank_state && (animation == leap_animation || animation == leap_animation_other_way)
        && recompinput::players::is_single_player_mode();
}

bool conker::look_aim::leaping_now() {
    return leaping;
}

void conker::look_aim::leap_stick(float& x, float& y) {
    if (!leaping) {
        leap_x = leap_y = 0.0f;
        return;
    }
    float mouse_x = 0.0f, mouse_y = 0.0f;
    recompinput::get_mouse_deltas(&mouse_x, &mouse_y);
    if (conker::netsession::active()) {
        mouse_x = mouse_y = 0.0f;
    }
    // The mouse's y grows downward and the stick's upward: without inverting, the mouse up is the stick up.
    apply_invert(option<Invert>(options::mouse_invert), mouse_x, mouse_y);
    leap_x += mouse_x / leap_full_tilt;
    leap_y -= mouse_y / leap_full_tilt;
    // Kept within the stick's circle, so moving the mouse back brings the crosshair back at once.
    const float length = std::sqrt(leap_x * leap_x + leap_y * leap_y);
    if (length > 1.0f) {
        leap_x /= length;
        leap_y /= length;
    }
    x += leap_x;
    y += leap_y;
}

float conker::look_aim::camera_field_of_view() {
    // None without the settings: the game's own field of view (field_of_view.cpp).
    return 0.0f;   // (possess58: the setting taken out, the game's own field of view for everyone)
}

void conker::look_aim::free_camera_invert(bool& x, bool& y) {
    const Invert invert = option<Invert>(options::camera_turn_invert);
    x = invert == Invert::X || invert == Invert::Both;
    y = invert == Invert::Y || invert == Invert::Both;
}
#endif

#if defined(CONKER_RT64)
namespace {
    // The game's own stick is Invert Y, so X turns around when the setting has X, and Y when the
    // setting lacks it.
    bool own_turn_stick_x() {
        const Invert invert = option<Invert>(options::stick_invert);
        return invert == Invert::X || invert == Invert::Both;
    }
    bool own_turn_stick_y() {
        const Invert invert = option<Invert>(options::stick_invert);
        return invert == Invert::None || invert == Invert::X;
    }

    // The aiming settings that change what the game does with a player's sticks, as bits for its input online (see
    // look_flag): online they were this machine's for every player, so a player whose settings differed from the other
    // machine's (Stick: Aiming Response Direct here, Smooth there) aimed differently on each machine: aiming the tommy gun
    // (whose look the SMG's and the M4's fix-ups don't cover) the games drifted apart every second (0.9.46 test).
    constexpr uint8_t look_turn_x = 1, look_turn_y = 2, look_direct = 4, look_steady = 8;   // (look_steady: possess101, Aiming: Steady Camera)

    // Whether the look mode's camera's player has the bit: online from the frame's agreed input (that player's own settings),
    // offline this machine's.
    bool look_flag(uint8_t* rdram, gpr camera, uint8_t bit) {
        if (conker::netsession::active()) {
            const int player = (int)MEM_BU(0x23D, camera);
            return (conker::netsession::look_flags(player) & bit) != 0;
        }
        if (bit == look_turn_x) {
            return own_turn_stick_x();
        }
        if (bit == look_turn_y) {
            return own_turn_stick_y();
        }
        return option<Response>(options::stick_response) == Response::Direct;
    }
}
#endif

uint8_t conker::look_aim::sampled_look_flags() {
#if defined(CONKER_RT64)
    return (uint8_t)((own_turn_stick_x() ? look_turn_x : 0) | (own_turn_stick_y() ? look_turn_y : 0) |
        (option<Response>(options::stick_response) == Response::Direct ? look_direct : 0) |
        look_steady);   // (possess112: always, Aiming: Steady Camera being always on; nothing goes by it now)
#else
    return 0;
#endif
}

// The stick's yaw: target - stick x * scale, stored at 0x15120A14 (one state, which then goes on
// through the next one too) and at 0x15120A44 (every state). Turned around, it's a +.
#if defined(CONKER_RT64)
namespace {
    // Online, how much faster the stick turns the aim in R-Look than the game's own (4 degrees a frame across and 1.9 up and down at full
    // tilt): only the target the stick moves, so the camera still eases after it as on a Nintendo 64. The same for every player on every
    // machine (the game's state has to be), so it's not a setting.
    constexpr float online_look_speed = 1.5f;
    // (possess122) but the shotgun's look (its kind, +0x23E, 0x3B: the co-op mansion's) as on a Nintendo 64, at the game's own speed
    float look_speed(uint8_t* rdram, gpr camera) {
        if (((uint32_t)camera & 0xFF000000u) == 0x80000000u && MEM_BU(0x23E, camera) == 0x3B) {
            return 1.0f;
        }
        return conker::netsession::active() ? online_look_speed : 1.0f;
    }
}
#endif

extern "C" void conker_look_stick_yaw_a(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    // 0x15120A10: sub.s $f6, $f8, $f18 (the target less the stick's turn; turned around, plus)
    const float turn = ctx->f18.fl * look_speed(rdram, ctx->r16);
    ctx->f6.fl = look_flag(rdram, ctx->r16, look_turn_x) ? ctx->f8.fl + turn : ctx->f8.fl - turn;
#endif
}

extern "C" void conker_look_stick_yaw_b(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    // 0x15120A40: sub.s $f18, $f4, $f10
    const float turn = ctx->f10.fl * look_speed(rdram, ctx->r16);
    ctx->f18.fl = look_flag(rdram, ctx->r16, look_turn_x) ? ctx->f4.fl + turn : ctx->f4.fl - turn;
#endif
}

// The stick's pitch: target + stick y * scale, stored at 0x15120A98. Turned around, it's a -.
extern "C" void conker_look_stick_pitch(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    {
        // 0x15120A94: add.s $f10, $f6, $f8 (the target plus the stick's turn; turned around, less)
        const float turn = ctx->f8.fl;
        ctx->f10.fl = look_flag(rdram, ctx->r16, look_turn_y) ? ctx->f6.fl - turn : ctx->f6.fl + turn;
    }
    if (gun_aim_online(rdram, ctx->r16)) {
        // ($f6 the target before the stick's turn, $f10 after it.)
        ctx->f10.fl = ctx->f6.fl + (ctx->f10.fl - ctx->f6.fl) * gun_pitch_scale;
    }
#endif
}

// Camera: Invert Turning. At 0x1512D3F0 func_1512D390 ($s0 the camera) has just stored the
// C-buttons' turn direction, 1 (C-Right) or -1 (C-Left), at + 0x6B0.
extern "C" void conker_camera_turn_invert(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (conker::netsession::active()) {
        return;   // (online the game's own, the same on every machine: the setting is for single player)
    }
    const Invert invert = option<Invert>(options::camera_turn_invert);
    if (invert == Invert::X || invert == Invert::Both) {
        MEM_W(0x6B0, ctx->r16) = -MEM_W(0x6B0, ctx->r16);
    }
#endif
}

#if defined(CONKER_RT64)
float conker::look_aim::camera_turn_speed() {
    return (float)(number_option(options::camera_turn_speed, 100.0) / 100.0);
}
#endif

// Camera: Turning Speed. The frame's turn, about to be passed to func_1508EF80: in $f18 while
// C-Left or C-Right is held (0x1512D4C4), in $f4 while the turn glides to a stop (0x1512D53C).
// Only the turn applied is scaled, not the speed the game keeps, so it eases in and out as before.
extern "C" void conker_camera_turn_speed_held(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (conker::netsession::active()) {
        return;   // (online the game's own, the same on every machine: the setting is for single player; the free camera's turn, which
                  // goes with the input, keeps it)
    }
    ctx->f18.fl *= conker::look_aim::camera_turn_speed();
#endif
}

extern "C" void conker_camera_turn_speed_released(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (conker::netsession::active()) {
        return;
    }
    ctx->f4.fl *= conker::look_aim::camera_turn_speed();
#endif
}

#if defined(CONKER_RT64)
namespace {
    // The mouse and keyboard are player 1's. The camera (struct108, the look mode's and the second
    // aiming mode's $s0) keeps its index into the cameras (D_800BE628) at + 0x23D, 0 for player 1;
    // in multiplayer the aiming code runs for each player's camera in turn.
    bool is_player_one(uint8_t* rdram, gpr camera) {
        return MEM_BU(0x23D, camera) == 0;
    }

    // One poll's mouse and gyro movement as a turn of the view, in degrees: the yaw grows to the left
    // and the pitch downward (see conker_look_targets). False if there was none.
    bool take_movement(float& mouse_yaw, float& mouse_pitch, float& gyro_yaw, float& gyro_pitch) {
        Movement m;
        {
            std::lock_guard lock{queue_mutex};
            if (queue.empty()) {
                return false;
            }
            m = queue.front();
            queue.pop_front();
        }
        mouse_yaw = -m.mouse_x * mouse_degrees_per_pixel;
        mouse_pitch = m.mouse_y * mouse_degrees_per_pixel;
        gyro_yaw = m.gyro_y * gyro_scale;
        gyro_pitch = -m.gyro_x * gyro_scale;
        apply_invert(option<Invert>(options::mouse_invert), mouse_yaw, mouse_pitch);
        apply_invert(option<Invert>(options::gyro_invert), gyro_yaw, gyro_pitch);
        return true;
    }
}
#endif

// The second aiming mode's stick (func_15126378, at 0x15126EA0; $s0 is the camera): the yaw's turn in
// $f14 is about to be subtracted and the pitch's in $f12 added, in degrees, the same directions as
// the look mode's stick (X normal, Y inverted), so the same setting turns them around. Some states
// have scaled or clamped them (symmetrically) already.
//
// The mouse and gyro turn it here too (issue #61: the sniper scope didn't follow the mouse at all).
// This mode turns the aim at once, with no spring to ease it. Zoomed in, the stick turns slower;
// the mouse and gyro turn by the same part of the view: their turn is scaled by the zoom, the
// tangent of the field of view in use (the camera's + 0x74) against the unzoomed one's (+ 0x6C),
// as func_1510B128 sets it.
namespace {
    // (0.10.9: the frame each player's camera last ran the second aiming mode, online; second_aim_recent)
    // (possess46: the 5th player's too, its extra camera's, signal_player: net_session.cpp's input for its slot goes by it, on every
    // machine alike, as the game runs that camera on all of them)
    bool second_aim_seen[signal_players] = {};
    uint32_t second_aim_frame[signal_players] = {};
}

bool conker::look_aim::second_aim_recent(int player) {
    if (player < 0 || player >= signal_players || !second_aim_seen[player]) {
        return false;
    }
    const uint32_t now = conker::netsession::frame();
    return now >= second_aim_frame[player] && now - second_aim_frame[player] <= 3;
}

extern "C" void conker_aim_stick(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (conker::netsession::active()) {
        const int aim_player = signal_player(rdram, ctx->r16);
        if (aim_player >= 0 && aim_player < signal_players) {
            second_aim_seen[aim_player] = true;
            second_aim_frame[aim_player] = conker::netsession::frame();
        }
    }
    // Aiming: Swap Sticks: this camera's player is aiming.
    note_aiming(rdram, ctx->r16, signal_player(rdram, ctx->r16));
    if (look_flag(rdram, ctx->r16, look_turn_x)) {
        ctx->f14.fl = -ctx->f14.fl;
    }
    if (look_flag(rdram, ctx->r16, look_turn_y)) {
        ctx->f12.fl = -ctx->f12.fl;
    }

    const gpr camera = ctx->r16;
    if (!is_player_one(rdram, camera)) {
        return;
    }
    float mouse_yaw, mouse_pitch, gyro_yaw, gyro_pitch;
    if (!take_movement(mouse_yaw, mouse_pitch, gyro_yaw, gyro_pitch)) {
        return;
    }
    const gpr view = (gpr)(int32_t)((uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE628) + MEM_BU(0x23D, camera) * 0x180);
    constexpr float half_degrees_to_radians = 3.14159265358979f / 360.0f;
    const float fov = read_float(rdram, view, 0x74), unzoomed = read_float(rdram, view, 0x6C);
    float zoom = 1.0f;
    if (fov > 0.0f && unzoomed > 0.0f && fov < 180.0f && unzoomed < 180.0f) {
        zoom = std::clamp(std::tan(fov * half_degrees_to_radians) / std::tan(unzoomed * half_degrees_to_radians), 0.02f, 2.0f);
    }
    // The yaw's turn is subtracted, the pitch's added.
    ctx->f14.fl -= (mouse_yaw + gyro_yaw) * zoom;
    ctx->f12.fl += (mouse_pitch + gyro_pitch) * zoom;
#endif
}

// At the start of the two paths where the look mode works its yaw target out each frame from the way
// Conker faces ($s0 + 0x3D0 is his object, its + 0x7A his facing) less an aiming angle (+ 0x12 of
// $s0 + 0x3D4, an s16 the stick turns: 65536 to a turn, scaled by 0.35, D_800A33B4, on the second
// path). A yaw added to the target alone was thrown away the next frame: the view sprang back.
extern "C" void conker_look_yaw_from_facing(uint8_t* rdram, recomp_context* ctx) {
    aim_units_per_degree = 65536.0f / 360.0f;
}

extern "C" void conker_look_yaw_from_facing_scaled(uint8_t* rdram, recomp_context* ctx) {
    aim_units_per_degree = 65536.0f / 360.0f / 0.35f;
}

// (possess109) Aiming: Steady Camera keeps the game from turning Conker in the look. func_150599C8 turns an object's facing (+0x7A of $a0)
// toward a way ($a2) by up to $a1 times 0x100 a frame, and as he slows from walking the game turns him with it toward his moving angle,
// 0xC00 a frame (func_150585F0). With the sticks swapped his facing is the view's (conker_look_targets), so the view turned with him: 16.9
// degrees in one frame as he went from walking back to forward (possess108's [steady] lines, f=1487, 1592, 1958, 2235), and letting go
// after walking back, 16.9 a frame for 7 frames running (f=2659), half a turn. His facing is kept as the function starts and put back as
// it ends, its answer (how far there was to turn) as it was. Every machine alike, from the game's memory and the frame's agreed input.
namespace {
    struct TurnKeep { bool on = false; uint32_t object = 0; uint16_t facing = 0, toward = 0; uint32_t rate = 0, from = 0; int player = -1; };
    TurnKeep turn_keep;
    // The stick's turn of the aim being measured (conker_steady_aim_turn_start, _end).
    struct AimTurnKeep { bool on = false; uint32_t object = 0; uint16_t view = 0; int player = -1; };
    AimTurnKeep aim_turn_keep;
    uint32_t turns_kept_told = 0, walk_told = 0, view_check_told = 0;
    bool kseg0(gpr address) {
        return ((uint32_t)address & 0xFF000000u) == 0x80000000u;
    }
}

// (possess132) The shotgun's look too (kind 0x3B, the co-op mansion's, either stage, the sticks swapped), which possess119 took off Steady
// Camera: walking it with the left stick (possess130) sets his moving angle the way pressed from the view, and the game turned his facing
// toward it (up to 0xC00 a frame), the shotgun's view (his facing less the aim) with it: the aim yanked off the zombies whenever he moved
// (bad.mkv), never standing still (good.mkv). Only this turn is kept from him there; the rest of its look is the game's, as possess119 has it.
static gpr shotgun_turn_record_of(uint8_t* rdram, gpr object) {
    const uint32_t looks = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
    if ((looks >> 24) != 0x80 || ((uint32_t)object & 0xFF000000u) != 0x80000000u) {
        return 0;
    }
    for (uint32_t i = 0; i < (uint32_t)aim_players; i++) {
        const gpr camera = (gpr)(int32_t)(looks + i * 0x9A0u);
        if ((uint32_t)MEM_W(0x3D0, camera) != (uint32_t)object || MEM_BU(0x23E, camera) != 0x3B || MEM_H(0x73C, camera) == 0) {
            continue;
        }
        const int player = camera_player(rdram, camera);
        if (player < 0 || player >= aim_players || !swap_state(player)) {
            continue;
        }
        return camera;
    }
    return 0;
}

extern "C" void conker_steady_turn_start(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    turn_keep.on = false;
    gpr camera = steady_record_of(rdram, ctx->r4);
    if (camera == 0) {
        camera = shotgun_turn_record_of(rdram, ctx->r4);
    }
    if (camera == 0) {
        return;
    }
    turn_keep.on = true;
    turn_keep.object = (uint32_t)ctx->r4;
    turn_keep.facing = (uint16_t)MEM_HU(0x7A, ctx->r4);
    turn_keep.toward = (uint16_t)ctx->r6;
    turn_keep.rate = (uint32_t)ctx->r5 & 0xFFu;
    turn_keep.from = (uint32_t)ctx->r31;
    turn_keep.player = camera_player(rdram, camera);
#endif
}

extern "C" void conker_steady_turn_end(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (!turn_keep.on) {
        return;
    }
    turn_keep.on = false;
    if ((uint32_t)ctx->r4 != turn_keep.object) {
        return;
    }
    const uint16_t turned_to = (uint16_t)MEM_HU(0x7A, ctx->r4);
    if (turned_to == turn_keep.facing) {
        return;
    }
    MEM_H(0x7A, ctx->r4) = (int16_t)turn_keep.facing;
    if (conker::netsession::active() && turn_keep.player == conker::netsession::local_player() && turns_kept_told < 400) {
        turns_kept_told++;
        char line[220];
        std::snprintf(line, sizeof(line), "[steady] f=%u kept from turning: facing %04X (the game turned it to %04X, toward %04X at %02X00 a frame, from %08X)",
            conker::netsession::frame(), (unsigned)turn_keep.facing, (unsigned)turned_to, (unsigned)turn_keep.toward, (unsigned)turn_keep.rate,
            (unsigned)turn_keep.from);
        conker::netsession::log(line);
    }
#endif
}

// (possess109) The stick's turn of the aim (func_15063A38, $a0 Conker: his aim angle, +0x12 of his +0x31C, and past +-9000 his facing too),
// measured as the turn of his facing less the angle, the view across, for Aiming: Steady Camera's check of the view (steady_view_check).
extern "C" void conker_steady_aim_turn_start(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    aim_turn_keep.on = false;
    const gpr camera = steady_record_of(rdram, ctx->r4);
    if (camera == 0) {
        return;
    }
    const gpr aim = (gpr)(int32_t)MEM_W(0x31C, ctx->r4);
    if (!kseg0(aim)) {
        return;
    }
    aim_turn_keep.on = true;
    aim_turn_keep.object = (uint32_t)ctx->r4;
    aim_turn_keep.view = (uint16_t)((uint32_t)MEM_HU(0x7A, ctx->r4) - (uint32_t)(uint16_t)MEM_H(0x12, aim));
    aim_turn_keep.player = camera_player(rdram, camera);
#endif
}

extern "C" void conker_steady_aim_turn_end(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (!aim_turn_keep.on) {
        return;
    }
    aim_turn_keep.on = false;
    const int player = aim_turn_keep.player;
    const gpr aim = (gpr)(int32_t)MEM_W(0x31C, ctx->r4);
    if ((uint32_t)ctx->r4 != aim_turn_keep.object || player < 0 || player >= aim_players || !kseg0(aim)) {
        return;
    }
    const uint16_t view = (uint16_t)((uint32_t)MEM_HU(0x7A, ctx->r4) - (uint32_t)(uint16_t)MEM_H(0x12, aim));
    SteadyAim& sa = steady_aim[player];
    const uint32_t now = steady_frame_now();
    if (sa.stick_frame != now) {
        sa.stick_frame = now;
        sa.stick_turn = 0;
    }
    sa.stick_turn += (int16_t)(uint16_t)((uint32_t)view - (uint32_t)aim_turn_keep.view);
#endif
}

#if defined(CONKER_RT64)
// (possess109) Aiming: Steady Camera: the view across should turn with the aim alone. Checked where the look has just worked it out (its
// yaw target, 0x15120908: Conker's facing less his aim angle), against the view as the look left it the frame before and the stick's
// turn since (func_15063A38): anything else that turned it (the game turning him, kept from him by conker_steady_turn_start now) is
// logged, for the next test. The guns' looks, whose yaw is the facing less the angle as it is (units_per_degree's first path).
static void steady_view_check(uint8_t* rdram, recomp_context* ctx, float units_per_degree) {
    const gpr camera = ctx->r16;
    const int player = camera_player(rdram, camera);
    if (player < 0 || player >= aim_players) {
        return;
    }
    const SteadyAim& sa = steady_aim[player];
    const gpr conker = (gpr)(int32_t)MEM_W(0x3D0, camera);
    const gpr aim = (gpr)(int32_t)MEM_W(0x3D4, camera);
    const uint32_t now = steady_frame_now();
    if (!sa.have || now != sa.frame + 1 || units_per_degree != 65536.0f / 360.0f || !kseg0(conker) || !kseg0(aim) || !steady_camera(rdram, camera)) {
        return;
    }
    const uint16_t view = (uint16_t)((uint32_t)MEM_HU(0x7A, conker) - (uint32_t)(uint16_t)MEM_H(0x12, aim));
    const bool stick_now = sa.stick_frame == now;
    const int32_t stick = stick_now ? sa.stick_turn : 0;
    const int32_t off = (int16_t)(uint16_t)((uint32_t)view - (uint32_t)sa.view - (uint32_t)stick);
    if ((off > 0x40 || off < -0x40) && conker::netsession::active() && player == conker::netsession::local_player() && view_check_told < 400) {
        view_check_told++;
        char line[200];
        std::snprintf(line, sizeof(line), "[steady] f=%u the view turned %d units (%.2f degrees) not by the stick (the stick's %d%s), facing %04X aim %d",
            now, (int)off, off * (360.0f / 65536.0f), (int)stick, stick_now ? "" : ", none measured this frame", (unsigned)MEM_HU(0x7A, conker),
            (int)MEM_H(0x12, aim));
        conker::netsession::log(line);
    }
}

// (possess109) The view across as the look leaves it this frame (after the stick swap's walking and the mouse), for the next frame's check.
static void steady_view_note(uint8_t* rdram, gpr camera) {
    const int player = camera_player(rdram, camera);
    if (player < 0 || player >= aim_players) {
        return;
    }
    SteadyAim& sa = steady_aim[player];
    const gpr conker = (gpr)(int32_t)MEM_W(0x3D0, camera);
    const gpr aim = (gpr)(int32_t)MEM_W(0x3D4, camera);
    if (!kseg0(conker) || !kseg0(aim) || !steady_camera(rdram, camera) || !swap_state(player)) {
        sa.have = false;
        return;
    }
    sa.view = (uint16_t)((uint32_t)MEM_HU(0x7A, conker) - (uint32_t)(uint16_t)MEM_H(0x12, aim));
    sa.frame = steady_frame_now();
    sa.have = true;
}
#endif

// (possess109) Aiming: Steady Camera's walking. func_15059C84 moves Conker each frame; here ($s0 his object) the way he goes (+0x76 of its
// stack) and how far ($f12) have been worked out: his moving angle and speed (+0x76, +0x3C), or a frame his animation takes its own step
// (+0x1FC bit 0) his facing and that step. With the sticks swapped the walking sets his moving angle from the way pressed (conker_look_targets)
// only after this, and meanwhile the game moved him its own way: letting go he went a step the way he faced and then on the way he had
// walked (possess108's [steady] lines, strafing back down the ramp: 9.7 back, 8.6 forward, 6.6 back, 7.1 forward), and pushed the other way
// he went it at full speed at once (9.8 back, 7.9 back, 8.3 forward). The view is locked to him, so it did all of that too. Here the way he
// goes is set as he's moved: the way pressed from the view (as conker_look_targets has it), or let go the way he was going, at the game's
// speed; and how fast he goes each way changes by at most 3 a frame (a tenth of his speed when that's more; walking it's about 10 a frame):
// he slows down, turns and comes round the other way smoothly, about 7 frames to go right round, 5 from forward to the side. Not while a
// hit holds him (hit_holding), with a grenade, or while the game turns his way itself (+0x46 of +0x31C). Once a frame for each player's
// Conker, from the game's memory and the frame's agreed buttons, on every machine alike (det_math).
extern "C" void conker_steady_walk(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    const gpr object = ctx->r16;
    const gpr camera = steady_record_of(rdram, object);
    if (camera == 0) {
        return;
    }
    const int player = camera_player(rdram, camera);
    SteadyWalk& w = steady_walk[player];
    const uint8_t kind = MEM_BU(0x23E, camera);
    const gpr aim = (gpr)(int32_t)MEM_W(0x3D4, camera);
    const gpr own = (gpr)(int32_t)MEM_W(0x31C, object);
    const bool game_turning = MEM_BU(0x127, object) != 0xFF && kseg0(own) && MEM_H(0x46, own) > 0;
    const float speed = read_float(rdram, object, 0x3C);
    if (kind == 0x26 || kind == 0x14 || grenade_held[player].load() || hit_hold_state[player].holding || game_turning || !(speed >= 0.0f && speed < 100.0f)) {
        w.have = false;
        return;
    }
    const uint16_t game_way = (uint16_t)MEM_HU(0x76, ctx->r29);
    const float game_length = ctx->f12.fl;
    if (!(game_length >= 0.0f && game_length < 200.0f)) {
        w.have = false;
        return;
    }
    // The way pressed, from the view (his facing less his aim angle; with no weapon, his facing, which no_weapon_walk set to the view).
    int dx = 0, dy = 0;
    uint16_t view = (uint16_t)MEM_HU(0x7A, object);
    if (kind == 0x00) {
        dx = walk_dx[player].load();
        dy = walk_dy[player].load();
    } else {
        constexpr uint32_t c_up = 0x0008, c_down = 0x0004, c_left = 0x0002, c_right = 0x0001;
        const gpr held_at = (gpr)(int32_t)MEM_W(0x36C, camera);
        const uint32_t held = kseg0(held_at) ? (uint32_t)MEM_HU(0, held_at) : 0u;
        // (possess149: the D-pad's way too, as the C-buttons': the game's strafe there goes by his moving angle, set from it here)
        dx = ((held & (c_right | 0x0100u)) ? 1 : 0) - ((held & (c_left | 0x0200u)) ? 1 : 0);
        dy = ((held & (c_up | 0x0800u)) ? 1 : 0) - ((held & (c_down | 0x0400u)) ? 1 : 0);
        if (conker::netsession::active() && gun_walk_set[player].load()) {
            dx = gun_walk_dx[player].load();
            dy = gun_walk_dy[player].load();
        }
        if (kseg0(aim)) {
            view = (uint16_t)((uint32_t)view - (uint32_t)(uint16_t)MEM_H(0x12, aim));
        }
    }
    const bool pressed = dx != 0 || dy != 0;
    const uint32_t now = steady_frame_now();
    if (!w.have || now != w.frame) {
        constexpr double unit_radians = 3.14159265358979 / 32768.0;
        if (!w.have) {
            // (from the way and how far the game would move him this frame)
            const double a = (double)game_way * unit_radians;
            w.vx = (float)((double)game_length * conker::det::sin_d(a + conker::det::pi / 2));
            w.vz = (float)((double)game_length * conker::det::sin_d(a));
            w.way = game_way;
            w.length = game_length;
            w.have = true;
        }
        uint16_t want = w.way;
        if (pressed) {
            const int32_t offset = (int32_t)std::lround(conker::det::atan2_d(-(double)dx, (double)dy) * 32768.0 / 3.14159265358979);
            want = (uint16_t)((uint32_t)view + (uint32_t)offset);
        }
        const double a = (double)want * unit_radians;
        const float tx = (float)((double)speed * conker::det::sin_d(a + conker::det::pi / 2)), tz = (float)((double)speed * conker::det::sin_d(a));
        float ex = tx - w.vx, ez = tz - w.vz;
        const float gap = std::sqrt(ex * ex + ez * ez);
        const float most = std::max(3.0f, 0.3f * speed);
        const bool there = gap <= most;
        if (!there) {
            ex *= most / gap;
            ez *= most / gap;
        }
        w.vx += ex;
        w.vz += ez;
        w.length = std::sqrt(w.vx * w.vx + w.vz * w.vz);
        if (there) {
            // (on the way wanted: exactly it, so walking straight on it never wavers by a unit)
            w.way = want;
            w.vx = tx;
            w.vz = tz;
            w.length = speed;
        } else if (w.length < 0.01f) {
            // (passing through a stop, turning right round: no way of its own)
            w.way = want;
        } else {
            w.way = (uint16_t)(int32_t)std::lround(conker::det::atan2_d((double)w.vz, (double)w.vx) / unit_radians);
        }
        w.frame = now;
        if (conker::netsession::active() && player == conker::netsession::local_player() && walk_told < 5000 && (pressed || speed > 0.05f || w.length > 0.05f)) {
            walk_told++;
            char line[260];
            std::snprintf(line, sizeof(line), "[walk] f=%u %s %+d,%+d speed %.2f, facing %04X view %04X; the game's way %04X %.2f%s; ours %04X %.2f",
                now, pressed ? "pressed" : "let go", dx, dy, speed, (unsigned)MEM_HU(0x7A, object), (unsigned)view, (unsigned)game_way, game_length,
                std::fabs(game_length - speed) > 0.001f ? " (his animation's step)" : "", (unsigned)w.way, w.length);
            conker::netsession::log(line);
        }
    }
    MEM_H(0x76, ctx->r29) = (int16_t)w.way;
    ctx->f12.fl = w.length;
    // His moving angle as the game keeps it: the way he goes while pressed; let go, his facing, as the game itself has it then. As he
    // slows below 2 the game turns him toward his moving angle (func_150585F0, kept from him now: conker_steady_turn_start), so let go
    // after walking back it would keep trying to turn him round.
    const uint16_t moving = pressed ? w.way : (uint16_t)MEM_HU(0x7A, object);
    MEM_H(0x76, object) = (int16_t)moving;
    note_move_written(player, moving);
#endif
}

// Before the pitch clamp: $s0 is the look state, $t0 the targets (reloaded at 0x15120B40).
extern "C" void conker_look_targets(uint8_t* rdram, recomp_context* ctx) {
    frame = Frame{};
#if defined(CONKER_RT64)
    // Aiming: Swap Sticks: this camera's player is aiming (the look mode, R-Look too: the stick aims, not moves).
    note_aiming(rdram, ctx->r16, signal_player(rdram, ctx->r16));
    if (is_player_one(rdram, ctx->r16)) {
        show_shotgun_laser(rdram, (gpr)(int32_t)MEM_W(0x3D0, ctx->r16));
    }
#endif
    float units_per_degree = aim_units_per_degree;
    aim_units_per_degree = 0.0f;
    // (possess130) the shotgun's look (kind 0x3B, the co-op mansion's) works its yaw out from his facing less the aim angle (+0x12 of +0x3D4)
    // at 65536 units a turn, a quarter turn off (the possess128 test's [look] lines: facing DF01, the yaw 223.6; the aim 90 turning it 0.49
    // degrees), on a path neither hook above is on, so the walking below never ran for it (its moving angle never written: the possess128
    // test's "the walking's was 0000"): pushed to the side he walked ahead of himself (C-Up, gun_side_walk) instead of strafing.
    if (units_per_degree == 0.0f && MEM_BU(0x23E, ctx->r16) == 0x3B) {
        units_per_degree = 65536.0f / 360.0f;
    }
#if defined(CONKER_RT64)
    steady_view_check(rdram, ctx, units_per_degree);   // (possess109)
#endif
#if defined(CONKER_RT64)
    // Aiming: Swap Sticks moves Conker like a shooter's character (issue #84): he keeps facing where the view looks,
    // so the aim stays on the crosshair, and walks the way the left stick (or W, A, S, D) points from the view's
    // forward: forward, back, sideways or between. Here the view's yaw is his facing (+0x7A of +0x3D0) less the
    // aiming angle (+0x12 of +0x3D4, times 0.35 on the scaled path). Each frame the whole angle goes into his
    // facing (the view stays where it is), and his moving angle (+0x76), which the game walks him along, is set to
    // the way pressed. His legs play the walk forward whichever way he goes: the game has no strafe.
    // Left to the game, he turned his body toward the way he walked, the aim making up the difference, but only
    // to +-9000 units (about 49 degrees): strafing came out as a diagonal, walking back as forward, and with the
    // mouse turning the aim held at its limit on the turn's side, so he went the wrong way or ran on the spot.
    // (Not in the grenade's look, kind 0x26: C-Down is held for it (Z, no_weapon_walk), and walking back turned his facing, and the view
    // with it, every frame.)
    // The grenade's look is 0x26 in War and 0x14 in The Vault: there, with no weapon, R and Z had Conker spin on the spot. Whatever its
    // kind, not while a grenade Z started is held.
    const int look_player = camera_player(rdram, ctx->r16);
    const bool holding_grenade = look_player >= 0 && look_player < aim_players && grenade_held[look_player].load();
    const uint8_t look_kind = MEM_BU(0x23E, ctx->r16);
    // (0.10.8: not while a hit in the look plays, hit_holding)
    const bool hit_hold_now = hit_holding(rdram, ctx->r16, look_player, true);
    // (possess107) The way he was last walked, kept for the frames after the stick is let go: the game slows him to a stop along his moving
    // angle (+0x76), which only the walking below sets, so let go it went back to his facing and he slowed to a stop that way instead:
    // strafing back down a ramp and letting go, he went 4.2, 3.1, 2.0, 0.9 forward (possess106's [steady] lines), the view lurching with
    // him. Kept 12 frames (he stops in 5), not while a hit plays. Every player's camera on every machine, from the game's memory and the
    // frame's agreed buttons.
    if (look_player >= 0 && look_player < aim_players && hit_hold_now) {
        release_frames[look_player] = 0;
    }
    if ((units_per_degree != 0.0f) && swap_state(look_player) && look_kind != 0x26 && look_kind != 0x14 && !holding_grenade && !hit_hold_now) {
        constexpr uint32_t c_up = 0x0008, c_down = 0x0004, c_left = 0x0002, c_right = 0x0001;
        // The buttons this camera's player holds (+0x36C points at them).
        const uint32_t held = (uint32_t)MEM_HU(0, (gpr)(int32_t)MEM_W(0x36C, ctx->r16));
        // (possess149: the D-pad's way too, as the C-buttons': in a gun's look (the M4's, 0x39) the game's strafe goes by his moving angle
        // and doesn't set it itself, so the D-pad given to the game's strafe alone walked him the way the left stick last set, one way only)
        int dx = ((held & (c_right | 0x0100u)) ? 1 : 0) - ((held & (c_left | 0x0200u)) ? 1 : 0);
        int dy = ((held & (c_up | 0x0800u)) ? 1 : 0) - ((held & (c_down | 0x0400u)) ? 1 : 0);
        // (the tommy gun's: the way pressed, not the C-Up it was turned into, gun_side_walk)
        if (conker::netsession::active() && look_player >= 0 && look_player < aim_players && gun_walk_set[look_player].load()) {
            dx = gun_walk_dx[look_player].load();
            dy = gun_walk_dy[look_player].load();
        }
        if (dx != 0 || dy != 0) {
            // The way pressed from the view's forward, in facing units: 65536 to a turn, growing to the left, as the
            // view's yaw does (45 degrees right of the view is less 8192).
            const int32_t offset = (int32_t)std::lround(conker::det::atan2_d(-(double)dx, (double)dy) * 32768.0 / 3.14159265358979);
            const gpr aim = (gpr)(int32_t)MEM_W(0x3D4, ctx->r16);
            const gpr conker = (gpr)(int32_t)MEM_W(0x3D0, ctx->r16);
            // His facing's units to the aiming angle's: the view's yaw is his facing less the angle times this.
            const double facing_per_angle = (65536.0 / 360.0) / units_per_degree;
            const int32_t angle = (int32_t)MEM_H(0x12, aim);
            const uint16_t view = (uint16_t)((uint32_t)MEM_HU(0x7A, conker) - (uint32_t)std::lround(angle * facing_per_angle));
            const uint16_t moving = (uint16_t)((uint32_t)view + (uint32_t)offset);
            // 0.9.64: online, at most 14 degrees of the angle a frame goes into his facing, the rest next frame (the view stays put either way:
            // his facing less what's left of the angle). Through R-Look's first stage the stick turns the angle alone (to +-9000, 49 degrees),
            // so walking with the stick held the whole of it went into his body the second stage's first frame: he snapped round. A frame's
            // turn of the stick (8.6 degrees at most) still goes in at once. Whole numbers, the same on every machine.
            int32_t moved = angle;
            if (conker::netsession::active()) {
                const int32_t most = (int32_t)std::lround(14.0 * (double)units_per_degree);
                moved = std::clamp(angle, -most, most);
            }
            MEM_H(0x7A, conker) = (int16_t)(uint16_t)((uint32_t)MEM_HU(0x7A, conker) - (uint32_t)std::lround(moved * facing_per_angle));
            MEM_H(0x12, aim) = (int16_t)(angle - moved);
            MEM_H(0x76, conker) = (int16_t)moving;
            note_move_written(look_player, moving);
            aim_remainder = 0.0f;
            if (look_player >= 0 && look_player < aim_players) {
                release_move[look_player] = moving;
                release_frames[look_player] = 12;
            }
        } else if (look_player >= 0 && look_player < aim_players && release_frames[look_player] > 0 && !steady_camera(rdram, ctx->r16)) {   // (possess109: Steady Camera's walking keeps his way, conker_steady_walk)
            release_frames[look_player]--;
            const gpr conker = (gpr)(int32_t)MEM_W(0x3D0, ctx->r16);
            if (((uint32_t)conker & 0xFF000000u) == 0x80000000u) {
                MEM_H(0x76, conker) = (int16_t)release_move[look_player];
            }
        }
    } else if (look_player >= 0 && look_player < aim_players && look_kind != 0x00) {
        release_frames[look_player] = 0;
    }
#endif
#if defined(CONKER_RT64)
    // The no weapon walking's other half (see no_weapon_walk): his moving angle is the way pressed from the view, as with a gun.
    // (0.10.6: in the look's first stage too, from his facing as it is)
    if (conker::netsession::active() && swap_state(camera_player(rdram, ctx->r16)) && MEM_BU(0x23E, ctx->r16) == 0x00
        && (MEM_H(0x73C, ctx->r16) == 1 || MEM_H(0x73C, ctx->r16) == 2) && !hit_hold_now) {
        const int player = camera_player(rdram, ctx->r16);
        const int dx = player >= 0 && player < aim_players ? walk_dx[player].load() : 0;
        const int dy = player >= 0 && player < aim_players ? walk_dy[player].load() : 0;
        const gpr conker = (gpr)(int32_t)MEM_W(0x3D0, ctx->r16);
        if ((dx != 0 || dy != 0) && ((uint32_t)conker & 0xFF000000u) == 0x80000000u) {
            const int32_t offset = (int32_t)std::lround(conker::det::atan2_d(-(double)dx, (double)dy) * 32768.0 / 3.14159265358979);
            const uint16_t moving = (uint16_t)((uint32_t)MEM_HU(0x7A, conker) + (uint32_t)offset);
            MEM_H(0x76, conker) = (int16_t)moving;
            note_move_written(player, moving);
            if (player >= 0 && player < aim_players) {   // (possess107, as above)
                release_move[player] = moving;
                release_frames[player] = 12;
            }
        } else if (player >= 0 && player < aim_players && release_frames[player] > 0 && ((uint32_t)conker & 0xFF000000u) == 0x80000000u
                && !steady_camera(rdram, ctx->r16)) {   // (possess109, as above)
            release_frames[player]--;
            MEM_H(0x76, conker) = (int16_t)release_move[player];
        }
    }
#endif
#if defined(CONKER_RT64)
    // Directions, all measured by playing: the yaw grows to the left and the pitch downward (the
    // game's stick is inverted: up looks down). The mouse's x grows to the right and its y
    // downward. recompinput's gyro gives the controller's turn on y (positive turning left) and
    // its tilt on x (positive tilting it back, toward you). Without inverting, the mouse and gyro
    // look the way they move: mouse or controller up looks up (take_movement).
    if (!is_player_one(rdram, ctx->r16)) {
        return;
    }
    float mouse_yaw, mouse_pitch, gyro_yaw, gyro_pitch;
    if (!take_movement(mouse_yaw, mouse_pitch, gyro_yaw, gyro_pitch)) {
        return;
    }

    const float yaw = mouse_yaw + gyro_yaw, pitch = mouse_pitch + gyro_pitch;
    if (yaw == 0.0f && pitch == 0.0f) {
        return;
    }
    const gpr targets = ctx->r8;
    write_float(rdram, targets, target_yaw, read_float(rdram, targets, target_yaw) + yaw);
    if (units_per_degree != 0.0f) {
        // Turn the aiming angle as far, for the next frame's target, which grows as it shrinks.
        const gpr aim = (gpr)(int32_t)MEM_W(0x3D4, ctx->r16);
        const float units = yaw * units_per_degree + aim_remainder;
        const int32_t whole = (int32_t)units;
        aim_remainder = units - (float)whole;
        // As the stick's turn does (func_15063A38, 0x15063AEC): past +-9000 units the aim stops there and Conker
        // turns by the rest (his facing, +0x7A, and +0x76 with it), so the view (facing less the aim) turns the
        // same and his body follows. The mouse turned the aim alone: swung fast, it went past 9000 and his body
        // stayed behind, and walking forward (the C-buttons, by his body) came out as a strafe (issue #84).
        constexpr int32_t aim_limit = 9000;
        int32_t angle = (int32_t)MEM_H(0x12, aim) - whole;
        if (angle < -aim_limit || angle > aim_limit) {
            const int32_t limit = (angle < 0) ? -aim_limit : aim_limit;
            const gpr conker = (gpr)(int32_t)MEM_W(0x3D0, ctx->r16);
            const uint16_t facing = (uint16_t)((uint32_t)MEM_HU(0x7A, conker) - (uint32_t)(angle - limit));
            MEM_H(0x7A, conker) = (int16_t)facing;
            MEM_H(0x76, conker) = (int16_t)facing;
            angle = limit;
        }
        MEM_H(0x12, aim) = (int16_t)angle;
    }
    frame.pitch_target_set = read_float(rdram, targets, target_pitch) + pitch;
    write_float(rdram, targets, target_pitch, frame.pitch_target_set);
    frame.targets_moved = true;
    if (option<Response>(options::mouse_response) == Response::Direct) {
        frame.direct_yaw += mouse_yaw;
        frame.direct_pitch += mouse_pitch;
    }
    if (option<Response>(options::gyro_response) == Response::Direct) {
        frame.direct_yaw += gyro_yaw;
        frame.direct_pitch += gyro_pitch;
    }
#endif
}

// After the springs: $s0 is the look state, $t0 the targets (reloaded at 0x15120E28 or 0x15120E88).
extern "C" void conker_look_currents(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    const gpr state = ctx->r16, targets = ctx->r8;
    steady_view_note(rdram, state);   // (possess109)
    // Aiming: Swap Sticks (issue #84): the view's yaw is the aim's. Walking back and to the side (C-Down with
    // C-Left or C-Right, which the left stick presses) swung the look mode's current yaw up to 27 degrees off its
    // target, the aim still (logged), and the spring then pulled it back: the camera turned with the left stick.
    // The current yaw is set to the target with no speed left, and the target is kept for the view
    // (conker_look_view), which is turned to it whatever moves the current yaw after this.
    const int player = camera_player(rdram, state);
    // Online, how far the look has settled (see look_settle): 1 offline.
    float settle = 1.0f;
    if (conker::netsession::active() && player >= 0) {
        if (look_settle[player] < 1.0f) {
            look_stage_two_frames[player]++;
            const float off_yaw = std::fabs(angle_difference(read_float(rdram, targets, target_yaw), read_float(rdram, state, current_yaw)));
            const float off_pitch = std::fabs(angle_difference(read_float(rdram, targets, target_pitch), read_float(rdram, targets, current_pitch)));
            if (off_yaw < 1.0f && (off_pitch < 1.0f || !gun_aim_online(rdram, state))) {
                look_settle[player] = 1.0f;
            } else if (look_stage_two_frames[player] > 20) {
                look_settle[player] = std::min(1.0f, look_settle[player] + 0.1f);
            }
        }
        settle = look_settle[player];
    }
    // 0.9.65: Swap Sticks' view on the aim from the second stage's first frame (up and down still waits for the settle): R-Look's first stage
    // ends with the view following the aim now (conker_look_stage_one), so nothing is stopped dead. Waiting for it to settle, a player whose
    // Stick: Aiming Response was Smooth kept turning and it never did: the view trailed the aim by 50 to 90 degrees for most of a second,
    // his body facing the aim and seen turned aside (0.9.64's player 2; player 1, Direct, had the view on the aim anyway).
    const float yaw_settle = (conker::netsession::active() && player >= 0) ? 1.0f : settle;
    if (swap_state(player) && yaw_settle > 0.0f) {
        const float target = read_float(rdram, targets, target_yaw);
        const float current = read_float(rdram, state, current_yaw);
        const float yaw = yaw_settle >= 1.0f ? target : current + angle_difference(target, current) * yaw_settle;
        write_float(rdram, state, current_yaw, yaw);
        write_float(rdram, targets, yaw_velocity, read_float(rdram, targets, yaw_velocity) * (1.0f - yaw_settle));
        if (yaw_settle >= 1.0f) {
            swap_view_yaw[player] = yaw;
            swap_view_yaw_set[player] = true;
        }
    }
    // Online, aiming a gun in R-Look (the SMG, the M4): up and down follows the target at once, as left and right does
    // (see gun_aim_online).
    if (gun_aim_online(rdram, state) && settle > 0.0f) {
        const float pitch_target = read_float(rdram, targets, target_pitch);
        const float current = read_float(rdram, targets, current_pitch);
        float pitch = settle >= 1.0f ? pitch_target : current + angle_difference(pitch_target, current) * settle;
        // (the game keeps this angle from 0 to 360 where the target is from -45 to 80)
        if (pitch < 0.0f) pitch += 360.0f;
        if (pitch >= 360.0f) pitch -= 360.0f;
        write_float(rdram, targets, current_pitch, pitch);
        write_float(rdram, targets, pitch_velocity, read_float(rdram, targets, pitch_velocity) * (1.0f - settle));
    }
    if (look_flag(rdram, state, look_direct)) {
        // The view is where the targets are, and the spring keeps no speed to overshoot with.
        write_float(rdram, state, current_yaw, read_float(rdram, targets, target_yaw));
        write_float(rdram, targets, current_pitch, read_float(rdram, targets, target_pitch));
        write_float(rdram, targets, yaw_velocity, 0.0f);
        write_float(rdram, targets, pitch_velocity, 0.0f);
        return;
    }
    if (!frame.targets_moved) {
        return;
    }
    write_float(rdram, state, current_yaw, read_float(rdram, state, current_yaw) + frame.direct_yaw);
    // The clamp may have held the pitch target back; the current pitch mustn't pass it either.
    const float target = read_float(rdram, targets, target_pitch);
    const float clamped = target - frame.pitch_target_set;
    float pitch = read_float(rdram, targets, current_pitch) + frame.direct_pitch;
    if ((clamped < 0.0f && pitch > target) || (clamped > 0.0f && pitch < target)) {
        pitch = target;
    }
    write_float(rdram, targets, current_pitch, pitch);
#endif
}

// 0.10.13: the online hooks' own state between frames, back to how it starts: on every machine at the same frame with a resync (net_session.cpp,
// hook_reset_frame), and on the machine loading the host's copy. (The function-local ones start over by conker::netsession::hook_generation.)
void conker::look_aim::reset_online_state() {
    for (int k = 0; k < aim_players; k++) {   // (possess107)
        release_frames[k] = 0;
        release_move[k] = 0;
    }
    for (int k = 0; k < aim_players; k++) {   // (possess109)
        steady_view[k] = SteadyView{};
        steady_walk[k] = SteadyWalk{};
        steady_aim[k] = SteadyAim{};
        steady_render[k] = SteadyRender{};   // (possess110)
        steady_probe[k] = SteadyProbe{};
    }
    turn_keep = TurnKeep{};
    aim_turn_keep = AimTurnKeep{};
    aim_remainder = 0.0f;
    for (int p = 0; p < aim_players; p++) {
        look_settle[p] = 0.0f;
        stage_one_followed[p] = false;
        view_turn[p] = ViewTurn{};
        eye_track[p] = EyeTrack{};
        eye_restore[p] = EyeRestore{};
        lift_state[p] = LiftState{};
        look_stage_two_frames[p] = 0;
        swap_view_yaw[p] = 0.0f;
        swap_view_yaw_set[p] = false;
    }
    for (int p = 0; p < aim_players; p++) {
        hit_hold_state[p] = HitHold{};
        walk_dx[p] = 0;
        walk_dy[p] = 0;
        grenade_held[p] = false;
        gun_walk_dx[p] = 0;
        gun_walk_dy[p] = 0;
        gun_walk_set[p] = false;
    }
    for (int p = 0; p < signal_players; p++) {
        second_aim_seen[p] = false;
        second_aim_frame[p] = 0;
    }
}


// (possess137) The co-op mansion's camera at Conker's height for every player (possess134's moving of the shotgun's view by player 1's
// height taken out). func_15124C38 works out how high over a character its camera looks (its look-at point's height, $f14 where the ways
// meet at 0x15124E1C): the character's +0xCC plus its +0x3D4's +0x114 (its model's height) times the camera's part of it (0.75, or the
// look's own). The co-op players are the race's characters (12 to 15): the caveman models are taller than Conker, so their cameras sat
// higher, in their heads aiming the shotgun (player 2's, 173 over his feet to Conker's 106.5, the possess133 test). Here each player's is
// worked out with Conker's two (the shortest of the stage's players, Conker's): the same camera over every one of them, in every mode.
// Every camera on every machine alike, from the game's memory. [camera] lines once a player's is changed.
extern "C" void conker_coop_camera_height(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (!conker::netsession::active() || !conker::netmenu::coop_running(rdram)) {
        return;
    }
    const gpr camera = ctx->r16;
    const gpr walker = (gpr)(int32_t)MEM_W(0x3D0, camera);
    const gpr aim = (gpr)(int32_t)MEM_W(0x3D4, camera);
    if (!kseg0(walker) || !kseg0(aim)) {
        return;
    }
    const float own_base = (float)MEM_H(0xCC, walker), own_height = (float)MEM_H(0x114, aim);
    if (own_height <= 0.0f) {
        return;
    }
    // Conker's: the shortest of the players' characters (the 4 cameras' own), by their models' heights
    const uint32_t looks = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
    if ((looks >> 24) != 0x80) {
        return;
    }
    float base = own_base, height = own_height;
    for (uint32_t i = 0; i < 4; i++) {
        const gpr other = (gpr)(int32_t)(looks + i * 0x9A0u);
        const gpr w = (gpr)(int32_t)MEM_W(0x3D0, other), a = (gpr)(int32_t)MEM_W(0x3D4, other);
        if (!kseg0(w) || !kseg0(a)) {
            continue;
        }
        const float h = (float)MEM_H(0x114, a);
        if (h > 0.0f && h < height) {
            height = h;
            base = (float)MEM_H(0xCC, w);
        }
    }
    if (height >= own_height) {
        return;
    }
    const float was = ctx->f14.fl;
    // (the first way, +0x4E's 1, is the model's height times the part alone, with no +0xCC)
    const bool height_alone = (MEM_BU(0x4E, aim) & 0xFu) == 1u;
    ctx->f14.fl = height_alone ? was * (height / own_height) : base + (was - own_base) * (height / own_height);
    static uint32_t told = 0;
    if (told < 8 && camera_player(rdram, camera) == conker::netsession::local_player()) {
        told++;
        char line[200];
        std::snprintf(line, sizeof(line), "[camera] f=%u player %d's camera at Conker's height: %.1f over him, not %.1f (his model %d high over %d, Conker's %d over %d)",
            conker::netsession::frame(), camera_player(rdram, camera) + 1, ctx->f14.fl, was, (int)own_height, (int)own_base, (int)height, (int)base);
        conker::netsession::log(line);
    }
#endif
}

// ---- (possess146) The D-pad's strafe ----
// The game's own strafe is in each character's movement (func_1504CB98, 0x1504DC3C to 0x1504DE70): on the ground, in the first or the
// second aiming mode (+0x31C's +0x11A 0 or 2), and in a multiplayer arena with any weapon (D_800BE616; elsewhere the M4's, the
// bazooka's and the shotgun's only), the C-buttons held are masked to C-Left and C-Right (all four with a weapon out, not scoped), and
// if any is held: the way they make (D_8009917C's halfwords: C-Up the way he faces, C-Left a quarter turn to the left...; the stick's
// forward or back added when not scoped) is where he walks, at 34 (sp+0x1BC, sp+0x1B4), his facing kept (the stick's X turns it), and
// his strafe's flag set (+0x31C's +0x52, 2). possess145 gave the game the D-pad as the C-buttons, so everything else that reads them
// (the look's zoom, a grenade on C-Down, the shotgun's aim with Z) read them too. Here the D-pad is that strafe's alone: at 0x1504DD1C
// ($a0 the C-buttons held masked by $v0, $a1 the buttons held, $s0 the character) its directions (up C-Up, down C-Down, left C-Left,
// right C-Right) are added to $a0 under the same mask, and nothing else in the game sees them as C-buttons. Online, in the arenas and
// the co-op campaign, for the players' characters (the first 5). From the frame's agreed buttons, the same on every machine.
extern "C" void conker_dpad_strafe(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (!conker::netsession::active()) {
        return;
    }
    const uint32_t held = (uint32_t)ctx->r5 & 0xFFFFu;
    if ((held & 0x0F00u) == 0) {
        return;
    }
    if (MEM_BU(0, (gpr)(int32_t)0x800BE616) == 0 && !conker::netmenu::coop_running(rdram)) {
        return;
    }
    const uint32_t c = (uint32_t)ctx->r16;
    constexpr uint32_t characters = 0x800CC2D0, character_size = 0x32C;
    if (c < characters || (c - characters) % character_size != 0 || (c - characters) / character_size >= 5) {
        return;
    }
    uint32_t pad = 0;
    if (held & 0x0800u) {
        pad |= 0x0008u;   // up: C-Up, forward
    }
    if (held & 0x0400u) {
        pad |= 0x0004u;   // down: C-Down, back
    }
    if (held & 0x0200u) {
        pad |= 0x0002u;   // left: C-Left
    }
    if (held & 0x0100u) {
        pad |= 0x0001u;   // right: C-Right
    }
    const uint32_t mask = (uint32_t)ctx->r2 & 0xFu;
    const uint32_t before = (uint32_t)ctx->r4 & 0xFFFFu;
    const uint32_t now = before | (pad & mask);
    ctx->r4 = (gpr)(int32_t)now;
    static uint32_t told = 0;
    static uint32_t told_mask[5] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    const uint32_t k = (c - characters) / character_size;
    if (told < 20 && told_mask[k] != mask) {
        told++;
        told_mask[k] = mask;
        const uint32_t aim = (uint32_t)MEM_W(0x31C, (gpr)(int32_t)c);
        const bool aim_ok = (aim >> 24) == 0x80 && (aim & 0x00FFFFFFu) < 0x007F0000u;
        const unsigned weapon = aim_ok ? (unsigned)MEM_BU(0x78, (gpr)(int32_t)aim) : 0u, scoped = aim_ok ? (unsigned)MEM_BU(0x197, (gpr)(int32_t)aim) : 0u;
        char line[220];
        std::snprintf(line, sizeof(line), "[strafe] f=%u character %u: the D-pad strafes (%s%s%s%s), the game's strafe taking %s of the C-buttons "
            "(weapon %02X, scoped %u)", (unsigned)conker::netsession::frame(), (unsigned)k, (held & 0x0800u) ? "up " : "", (held & 0x0400u) ? "down " : "",
            (held & 0x0200u) ? "left " : "", (held & 0x0100u) ? "right" : "", mask == 0xF ? "all four" : "C-Left and C-Right",
            weapon, scoped);
        conker::netsession::log(line);
    }
#else
    (void)rdram;
    (void)ctx;
#endif
}

// ---- (possess150) The Grenade button ----
// A button of its own for the grenade (Controls: Grenade, bound like any other: G on the keyboard to begin with), that the game's
// grenade alone reads, as the D-pad is the strafe's alone (conker_dpad_strafe). The game holds and throws a grenade with C-Down: in a
// character's movement (func_1504CB98, 0x1504CF34) C-Down pressed this frame (D_800CC288, the buttons pressed, from D_800BE710's), with
// neither A nor B, no weapon aimed (+0x31C's +0x78 0) and grenades left (+0x19A), in the arenas (and the levels D_800BE9F0 names there),
// starts one (+0x31C's +0x19B 1, or 5 there); then the weapons' code (func_15065A5C) holds it while C-Down is held (D_800CC290, the
// buttons held: 0x1506A040 for the grenade, 0x1506A2F0 for the other kind) and throws it once it's let go. The Grenade button is the
// controller's 0x0040 (no button of the Nintendo 64's: the game reads it nowhere), sent with the frame's other buttons, so every
// machine has it alike; at those three reads it's taken as C-Down as well, and C-Down itself goes on doing everything else it does.
namespace {
    constexpr uint32_t grenade_button = 0x0040u, grenade_c_down = 0x0004u;
    constexpr uint32_t grenade_characters = 0x800CC2D0u, grenade_character_size = 0x32Cu;

    int grenade_character(uint32_t c) {
        if (c < grenade_characters || (c - grenade_characters) % grenade_character_size != 0 || (c - grenade_characters) / grenade_character_size >= 25) {
            return -1;
        }
        return (int)((c - grenade_characters) / grenade_character_size);
    }

    void grenade_told(const char* what, uint32_t c) {
        static uint32_t told = 0;
        if (told < 24) {
            told++;
            char line[160];
            std::snprintf(line, sizeof(line), "[grenade] f=%u character %d: the Grenade button %s", (unsigned)conker::netsession::frame(),
                grenade_character(c), what);
            conker::netsession::log(line);
        }
    }
}

// func_1504CB98 at 0x1504CF38 ($v0 the buttons pressed this frame, D_800CC288; $s0 the character): the Grenade button pressed is C-Down's
extern "C" void conker_grenade_start(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    const uint32_t pressed = (uint32_t)ctx->r2;
    if ((pressed & grenade_button) == 0) {
        return;
    }
    ctx->r2 = (gpr)(int32_t)(pressed | grenade_c_down);
    grenade_told("pressed: taken as C-Down's press by the grenade's start", (uint32_t)ctx->r16);
}

// func_15065A5C at 0x1506A04C ($t7 the buttons held, D_800CC290; $s0 the character): the grenade held while the Grenade button is
extern "C" void conker_grenade_hold(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    const uint32_t held = (uint32_t)ctx->r15;
    if ((held & grenade_button) == 0) {
        return;
    }
    ctx->r15 = (gpr)(int32_t)(held | grenade_c_down);
    static uint32_t frames_held = 0;
    if (frames_held++ % 60u == 0) {
        grenade_told("held: the grenade held in the hand (let go, it's thrown)", (uint32_t)ctx->r16);
    }
}

// func_15065A5C at 0x1506A2F8 ($t5 the buttons held; $s0 the character): the same for the other kind (+0x19B 6)
extern "C" void conker_grenade_hold_other(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    const uint32_t held = (uint32_t)ctx->r13;
    if ((held & grenade_button) == 0) {
        return;
    }
    ctx->r13 = (gpr)(int32_t)(held | grenade_c_down);
    static uint32_t frames_held = 0;
    if (frames_held++ % 60u == 0) {
        grenade_told("held (the other kind's): held in the hand", (uint32_t)ctx->r16);
    }
}
