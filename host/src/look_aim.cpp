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
    // mode, from the game thread (steady clock ticks), for the input thread (swap_sticks_now). Like the reticle's,
    // it counts as aiming a little over two game frames after, so a frame the game takes longer over doesn't swap
    // the sticks back for a poll. Each player's, so in split screen each controller swaps as its player aims.
    constexpr int aim_players = 4;
    std::atomic<int64_t> last_aim_ticks[aim_players] = {};
    // The aim's yaw for each player's view this frame, from the look mode (conker_look_currents), while swapped.
    std::atomic<int> look_stage_seen[aim_players] = {};
    // Online, how far each player's look has settled from its start (0 swinging in, 1 settled): what Swap Sticks' view on the aim
    // (conker_look_currents) and the guns' up and down at once (gun_aim_online) are mixed in by. They put the view on the aim every
    // frame, so from the look's second stage its swing in had stopped dead: 0.9.56's snap, and 0.9.53's when the stick was held.
    // Now the game's springs swing the view in, and once it's within a degree of the aim (or 20 frames into the second stage, over
    // the next 10) the view is on the aim again. Worked out from the game's memory on every machine for every player's camera, frame
    // by frame, so the same on all of them.
    float look_settle[aim_players] = {};
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
        return (player < aim_players) ? player : -1;
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
    std::atomic<bool> shotgun_aiming[aim_players] = {};
    void note_aiming(uint8_t* rdram, gpr camera, int player) {
        if (player >= 0 && player < aim_players) {
            last_aim_ticks[player] = std::chrono::steady_clock::now().time_since_epoch().count();
            const gpr character = (gpr)(int32_t)MEM_W(0x3D0, camera);
            shotgun_aiming[player] = (((uint32_t)character & 0xFF000000u) == 0x80000000u) && (MEM_BU(0x4, character) == shotgun_aim_state);
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
        const std::string aim_reticle = "aim_reticle";
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

// The Camera tab: the normal camera, then aiming with the stick (R-Look and the second aiming mode) and
// the reticle. Grouped by what they're about, each group's names starting alike.
void conker::look_aim::add_camera_options(recomp::config::Config& config) {
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
    config.add_number_option(options::camera_fov, "Camera: Field of View",
        "How wide the normal camera sees, as its vertical field of view (the same whatever the aspect ratio). "
        "<recomp-color primary>50\xC2\xB0</recomp-color> matches the original game (60.6\xC2\xB0 across at 4:3). More shows more "
        "around Conker, less brings the view in closer; the camera stays as far away. Only the normal camera: R-Look, "
        "aiming (so the scope's zoom), cutscenes and other special cameras stay as the game has them.",
        35.0, 80.0, 1.0, 0, false, 50.0);
    recompui::set_number_option_suffix(options::camera_fov, "\xC2\xB0");

    config.add_enum_option(options::stick_response, "Stick: Aiming Response",
        "How the view follows the stick in R-Look (hold R and look around)." + about +
        " This also applies to the mouse and gyro, as the view catches up with the stick at once.",
        response, Response::Smooth);
    config.add_enum_option(options::stick_invert, "Stick: Invert Aiming",
        "Inverts the stick in R-Look (hold R and look around) and in the second aiming mode (e.g. the sniper scope, the magnum, throwables), separately from the mouse and gyro. <recomp-color primary>Invert Y</recomp-color> is the default and matches the original game: pushing the stick up looks down.",
        invert, Invert::Y);
    config.add_enum_option(options::aim_reticle, "Aiming: Reticle",
        "Shows a green ring in the middle of the screen while aiming the slingshot, the throwing knives or the magnum, "
        "like Conker: Live & Reloaded's, where what you aim at is. Not in plain R-Look, zoomed in (the sniper scope has "
        "its own crosshair) or in split screen. Online it's shown for your own view, and for the SMG and the M4 as well. "
        "<recomp-color primary>Off</recomp-color> matches the original game, where you aim by eye.",
        toggle, Toggle::Off);
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
    config.add_enum_option(options::aim_lock_on, "Aiming: Lock-On",
        "Whether aiming locks on to enemies, as with the shotgun once the zombies come: the game turns your aim (and the "
        "camera with it) toward one, whatever the stick does. <recomp-color primary>On</recomp-color> matches the "
        "original game. Off leaves the aim to you.",
        toggle, Toggle::On);
}

// The Mouse & Gyro tab. The sensitivities are RecompFrontend's options (same ids, so saved values carry
// over), added here instead of by its General tab to sit with their group; RecompFrontend reads them from
// here (find_option_config). Each input's other settings are hidden while its sensitivity is zero, which
// turns that input off.
void conker::look_aim::add_mouse_gyro_options(recomp::config::Config& config) {
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
        config.add_option_hidden_dependency(id, recompui::config::general::options::mouse_sensitivity, off);
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
    recompinput::get_gyro_deltas(0, &m.gyro_x, &m.gyro_y);
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
bool conker::reticle::enabled() {
    return option<Toggle>(options::aim_reticle) == Toggle::On;
}

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
    // (online every machine has to do the same for every player: the lock-on is the game's own there, as it is with the setting On;
    // the setting is for single player)
    if (conker::netsession::active()) {
        return;
    }
    if (option_config(options::aim_lock_on) != nullptr && option<Toggle>(options::aim_lock_on) == Toggle::Off) {
        ctx->r9 = 0;
    }
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
    const int32_t turn = (int32_t)ctx->r13;
    ctx->r13 = (gpr)(int32_t)(turn * 3 / 2);
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
    {
        const int player = camera_player(rdram, camera);
        if (player >= 0 && MEM_H(0x73C, camera) != 2) {
            look_settle[player] = 0.0f;
            look_stage_two_frames[player] = 0;
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
        if (gun_kind) {
            const int32_t units = (int32_t)MEM_HU(0x7A, conker) - (int32_t)MEM_H(0x12, aim) + 0xC000;
            write_float(rdram, camera, 0x740 + target_yaw, (float)units * 0.005493164f);
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
extern "C" void conker_look_view(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    const gpr camera = ctx->r16;
    const int player = camera_player(rdram, camera);
    if (player >= 0) {
        look_stage_seen[player] = (int)MEM_H(0x73C, camera);
    }
    // Online, this machine's player's look, its first 45 frames each time (the view's movement after R, for the logs).
    if (conker::netsession::active() && player >= 0 && player == conker::netsession::local_slot()) {
        static int frames_logged = -1;
        const int stage = (int)MEM_H(0x73C, camera);
        if (stage == 0) {
            frames_logged = -1;
        } else if (frames_logged < 45) {
            frames_logged = frames_logged < 0 ? 1 : frames_logged + 1;
            const gpr t0 = camera + 0x740;
            char line[400];
            std::snprintf(line, sizeof(line),
                "[look] f=%u n=%d stage=%d kind=%02X 23C=%u yaw=%.2f->%.2f pitch=%.2f->%.2f a=%.1f->%.1f b=%.1f->%.1f eye=%.1f,%.1f,%.1f at=%.1f,%.1f,%.1f",
                conker::netsession::frame(), frames_logged, stage, (unsigned)MEM_BU(0x23E, camera), (unsigned)MEM_BU(0x23C, camera),
                read_float(rdram, camera, current_yaw), read_float(rdram, t0, target_yaw),
                read_float(rdram, t0, current_pitch), read_float(rdram, t0, target_pitch),
                read_float(rdram, camera, 0x18C), read_float(rdram, camera, 0x190), read_float(rdram, camera, 0x194), read_float(rdram, camera, 0x198),
                read_float(rdram, camera, 0x2F8), read_float(rdram, camera, 0x2FC), read_float(rdram, camera, 0x300),
                read_float(rdram, camera, 0x2E0), read_float(rdram, camera, 0x2E4), read_float(rdram, camera, 0x2E8));
            conker::netsession::log(line);
        }
    }
    if (!swap_state(player)) {
        return;
    }
    // The eye from where it's copied (+0x2F8): the copy's z is stored after this, in the branch's delay slot.
    const float ex = read_float(rdram, camera, 0x2F8), ez = read_float(rdram, camera, 0x300);
    const float dx = read_float(rdram, camera, 0x2E0) - ex, dz = read_float(rdram, camera, 0x2E8) - ez;
    const float across = std::sqrt(dx * dx + dz * dz);
    if (across < 1.0f) {
        return;
    }
    constexpr float degrees_to_radians = 3.14159265358979f / 180.0f;
    const float aim_yaw = swap_view_yaw_set[player] ? swap_view_yaw[player] : read_float(rdram, camera, current_yaw);
    swap_view_yaw_set[player] = false;
    const float yaw = (270.0f - aim_yaw) * degrees_to_radians;
    // (online every machine does this for every player's camera, so it has to come out the same on all of them: det_math.hpp)
    const bool online = conker::netsession::active();
    write_float(rdram, camera, 0x2E0, ex + (online ? conker::det::cos(yaw) : std::cos(yaw)) * across);
    write_float(rdram, camera, 0x2E8, ez + (online ? conker::det::sin(yaw) : std::sin(yaw)) * across);
#endif
}

bool conker::look_aim::swap_sticks_now(int player) {
    // (Online it's not this setting: see online_swap_enabled.)
    if (option<Toggle>(options::aim_swap_sticks) != Toggle::On || !recompinput::players::is_single_player_mode() || conker::netsession::active()) {
        return false;
    }
    if (player < 0 || player >= aim_players) {
        return false;
    }
    const int64_t since = std::chrono::steady_clock::now().time_since_epoch().count() - last_aim_ticks[player].load();
    return since >= 0 && std::chrono::steady_clock::duration(since) < aim_linger;
}

bool conker::look_aim::z_turns_aim(int player) {
    return (player >= 0) && (player < aim_players) && shotgun_aiming[player].load();
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
std::atomic<int> walk_dx[4], walk_dy[4];
// A player holding a grenade that Z started in the no weapon R-look (C-Down held for it, below), from the frame's agreed buttons.
std::atomic<bool> grenade_held[4];

void conker::look_aim::no_weapon_walk(uint8_t* rdram, int port, uint16_t* buttons) {
#if defined(CONKER_RT64)
    constexpr uint16_t c_buttons = 0x000F;
    if (port < 0 || port >= 4) {
        return;
    }
    const int dx = ((*buttons & 0x0001) ? 1 : 0) - ((*buttons & 0x0002) ? 1 : 0);
    const int dy = ((*buttons & 0x0008) ? 1 : 0) - ((*buttons & 0x0004) ? 1 : 0);
    walk_dx[port] = 0;
    walk_dy[port] = 0;
    // Z throws a grenade here (the game holds and throws one with C-Down while R is up, and R-look has R held): from Z's press C-Down is held with R
    // (logged: R and C-Down together start the grenade in the look, with its zoomed view) for as long as Z is, so the grenade is held to blow
    // up in the hand, or thrown on Z's release.
    // Z itself is not passed on.
    static bool grenade_on[4] = {};
    static bool prev_z[4] = {};
    const bool z_now = (*buttons & 0x2000) != 0;
    const bool z_edge = z_now && !prev_z[port];
    prev_z[port] = z_now;
    grenade_held[port] = grenade_on[port];
    if (grenade_on[port]) {
        if (z_now) {
            *buttons = (uint16_t)((*buttons & ~(c_buttons | 0x2000)) | 0x0010 | 0x0004);
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
    for (uint32_t i = 0; i < 4; i++) {
        const gpr camera = (gpr)(int32_t)(base + i * 0x9A0);
        if ((int)MEM_BU(0x23D, camera) != port) {
            continue;
        }
        if (MEM_BU(0x23E, camera) != 0x00) {
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


int conker::look_aim::look_stage(int player) {
    return (player >= 0 && player < aim_players) ? look_stage_seen[player].load() : 0;
}

bool conker::look_aim::aiming_recently(int player) {
    if (player < 0 || player >= aim_players) {
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
    return (float)number_option(options::camera_fov, 0.0);
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
    constexpr uint8_t look_turn_x = 1, look_turn_y = 2, look_direct = 4;

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
        (option<Response>(options::stick_response) == Response::Direct ? look_direct : 0));
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
    float look_speed() {
        return conker::netsession::active() ? online_look_speed : 1.0f;
    }
}
#endif

extern "C" void conker_look_stick_yaw_a(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    // 0x15120A10: sub.s $f6, $f8, $f18 (the target less the stick's turn; turned around, plus)
    const float turn = ctx->f18.fl * look_speed();
    ctx->f6.fl = look_flag(rdram, ctx->r16, look_turn_x) ? ctx->f8.fl + turn : ctx->f8.fl - turn;
#endif
}

extern "C" void conker_look_stick_yaw_b(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    // 0x15120A40: sub.s $f18, $f4, $f10
    const float turn = ctx->f10.fl * look_speed();
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
extern "C" void conker_aim_stick(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    // Aiming: Reticle (reticle.cpp): this camera aims this frame.
    conker::reticle::aim_frame(rdram, ctx->r16);
    // Aiming: Swap Sticks: this camera's player is aiming.
    note_aiming(rdram, ctx->r16, camera_player(rdram, ctx->r16));
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

// Before the pitch clamp: $s0 is the look state, $t0 the targets (reloaded at 0x15120B40).
extern "C" void conker_look_targets(uint8_t* rdram, recomp_context* ctx) {
    frame = Frame{};
#if defined(CONKER_RT64)
    // Aiming: Reticle (reticle.cpp): the look mode's state ($v0 saved at 0xA0($sp) as it starts,
    // what picks its paths) says whether it's aiming something.
    conker::reticle::look_frame(rdram, ctx->r16, (uint32_t)MEM_W(0xA0, ctx->r29));
    // Aiming: Swap Sticks: this camera's player is aiming (the look mode, R-Look too: the stick aims, not moves).
    note_aiming(rdram, ctx->r16, camera_player(rdram, ctx->r16));
    if (is_player_one(rdram, ctx->r16)) {
        show_shotgun_laser(rdram, (gpr)(int32_t)MEM_W(0x3D0, ctx->r16));
    }
#endif
    const float units_per_degree = aim_units_per_degree;
    aim_units_per_degree = 0.0f;
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
    const bool holding_grenade = look_player >= 0 && look_player < 4 && grenade_held[look_player].load();
    const uint8_t look_kind = MEM_BU(0x23E, ctx->r16);
    if ((units_per_degree != 0.0f) && swap_state(look_player) && look_kind != 0x26 && look_kind != 0x14 && !holding_grenade) {
        constexpr uint32_t c_up = 0x0008, c_down = 0x0004, c_left = 0x0002, c_right = 0x0001;
        // The buttons this camera's player holds (+0x36C points at them).
        const uint32_t held = (uint32_t)MEM_HU(0, (gpr)(int32_t)MEM_W(0x36C, ctx->r16));
        const int dx = ((held & c_right) ? 1 : 0) - ((held & c_left) ? 1 : 0);
        const int dy = ((held & c_up) ? 1 : 0) - ((held & c_down) ? 1 : 0);
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
            MEM_H(0x7A, conker) = (int16_t)view;
            MEM_H(0x12, aim) = 0;
            MEM_H(0x76, conker) = (int16_t)moving;
            aim_remainder = 0.0f;
        }
    }
#endif
#if defined(CONKER_RT64)
    // The no weapon walking's other half (see no_weapon_walk): his moving angle is the way pressed from the view, as with a gun.
    if (conker::netsession::active() && swap_state(camera_player(rdram, ctx->r16)) && MEM_BU(0x23E, ctx->r16) == 0x00 && MEM_H(0x73C, ctx->r16) == 2) {
        const int player = camera_player(rdram, ctx->r16);
        const int dx = player >= 0 && player < 4 ? walk_dx[player].load() : 0;
        const int dy = player >= 0 && player < 4 ? walk_dy[player].load() : 0;
        const gpr conker = (gpr)(int32_t)MEM_W(0x3D0, ctx->r16);
        if ((dx != 0 || dy != 0) && ((uint32_t)conker & 0xFF000000u) == 0x80000000u) {
            const int32_t offset = (int32_t)std::lround(conker::det::atan2_d(-(double)dx, (double)dy) * 32768.0 / 3.14159265358979);
            MEM_H(0x76, conker) = (int16_t)(uint16_t)((uint32_t)MEM_HU(0x7A, conker) + (uint32_t)offset);
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
    if (swap_state(player) && settle > 0.0f) {
        const float target = read_float(rdram, targets, target_yaw);
        const float current = read_float(rdram, state, current_yaw);
        const float yaw = settle >= 1.0f ? target : current + angle_difference(target, current) * settle;
        write_float(rdram, state, current_yaw, yaw);
        write_float(rdram, targets, yaw_velocity, read_float(rdram, targets, yaw_velocity) * (1.0f - settle));
        if (settle >= 1.0f) {
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
