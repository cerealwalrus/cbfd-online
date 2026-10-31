// Camera: Field of View (Camera tab, issue #65): widens or narrows the normal third-person
// camera's view, since with Right Stick: Free Camera the stick no longer zooms.
//
// func_1510B128 sets a camera's field of view each frame (D_800BE628, 0x180-byte cameras): the
// horizontal one in use (+0x74) and the vertical (+0x78), from the base ones (+0x6C, +0x70) plus
// its zoom and a global offset (D_800C3648). It then works out the projection (func_1510B5F8)
// and more from them, and the frustum the world is culled against (func_1501B22C) and the sky's
// sectors (widescreen.cpp) read them later. conker.toml calls conker_field_of_view just after
// both are stored: both are widened there by as much as the setting's vertical field of view is
// wider than the camera's base one (+0x70, 50 degrees; the horizontal base is 60.6 at 4:3), so
// the game's own zooming carries on around it, and everything after follows. The setting is the
// vertical field of view as it stays the same whatever the window's aspect ratio.
//
// The one other thing that depends on them is the scale the level's culls compare view-space
// x and y with (updateCullScales_1510B958): the game moves it linearly with the field of view,
// close enough for its own zooms but not for a much wider view, where the level's pieces at the
// sides were culled while in view. So at its return, with the setting in use, it's worked out
// from the game's own field of view instead, divided by how much wider the view is
// (conker::field_of_view::adjust_cull_scales, before widescreen.cpp widens it for the window).
//
// Only the normal camera (where the C-buttons turn it: not R-Look, aiming, cutscenes or other
// special cameras, so the scope's zoom and cutscenes' framing are the game's). Changing between
// them eases the change over a few frames.
//
// (0.11.4) Online too: every camera with its own player's setting, which travels in that player's
// input each frame (net_session.cpp), so every machine widens every camera alike and their games
// stay the same. Online alone: single player keeps the game's own view. 40 to 75
// degrees tall (the game's own is 50), clamped on every machine whatever a player sends.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "recomp.h"

#include "conker.hpp"

namespace {
    constexpr uint32_t cameras = 0x800BE628; // D_800BE628, a pointer to the cameras
    constexpr uint32_t camera_size = 0x180;
    constexpr uint32_t cull_scales = 0x800D35E0; // cullScaleX_800D35E0, then y
    constexpr uint32_t game_cameras = 0x800DBFF0; // D_800DBFF0, the game's cameras (0x9A0 each), +0x23D the player
    constexpr uint32_t game_camera_size = 0x9A0;
    constexpr float degrees_to_radians = 3.14159265358979f / 180.0f;
    constexpr float max_fov = 150.0f;
    // Each frame, the widening moves this fraction of the way to the setting's.
    constexpr float ease = 0.25f;
    // (0.11.4) every camera's own (online each player's; offline the one controller 1 plays): players 1 to 4 and the 5th's extra one
    constexpr int view_cameras = 5;

    struct View {
        float widening = 1.0f;      // how much this view is widened now (tan of the half angle)
        float game_fov[2] = {};     // the game's own fields of view (x, y) before them
        float widened[2] = { 1.0f, 1.0f }; // how much wider (tan of the half angle) they made them
    };
    View views[view_cameras];

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

    float half_tan(float degrees) {
        return std::tan(degrees * 0.5f * degrees_to_radians);
    }

    // The degrees whose half angle's tangent is t.
    float from_half_tan(float t) {
        return 2.0f * std::atan(t) / degrees_to_radians;
    }

    // (0.11.4) The vertical field of view camera i is to have, or 0 for the game's own. Online every camera's player's own, from the
    // frame's agreed input (net_session.cpp's field_of_view, the same on every machine, so every machine's game holds the same
    // cameras); offline none: the setting is for online alone.
    float setting_degrees(uint8_t* rdram, int i) {
#if defined(CONKER_RT64)
        if (conker::netsession::active()) {
            const uint32_t base = (uint32_t)MEM_W(0, (gpr)(int32_t)game_cameras);
            if ((base & 0xFF000000u) != 0x80000000u) {
                return 0.0f;
            }
            const int player = (int)MEM_BU(0x23D, (gpr)(int32_t)(base + (uint32_t)i * game_camera_size));
            return player >= 0 && player < 5 ? (float)conker::netsession::field_of_view(player) : 0.0f;
        }
        return 0.0f;   // (0.11.4: the setting is for online alone; single player keeps the game's own view)
#else
        (void)rdram;
        (void)i;
        return 0.0f;
#endif
    }
}

// func_1510B128, at 0x1510B1A8: both fields of view are stored, $v0 is the camera and $s0 its
// offset in the array.
extern "C" void conker_field_of_view(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t offset = (uint32_t)ctx->r16;
    if (offset % camera_size != 0 || offset / camera_size >= (uint32_t)view_cameras) {
        return;
    }
    const int i = (int)(offset / camera_size);
    View& v = views[i];
    const gpr camera = ctx->r2;
    const float base_y = read_float(rdram, camera, 0x70);
    const float setting = setting_degrees(rdram, i);
    float target = 1.0f;
    if (conker::mouse_camera::camera_normal(i) && setting > 0.0f && base_y > 0.0f && base_y < 180.0f) {
        target = half_tan(setting) / half_tan(base_y);
    }
    v.widening += (target - v.widening) * ease;
    if (std::fabs(v.widening - target) < 0.001f) {
        v.widening = target;
    }
    const float x = read_float(rdram, camera, 0x74);
    const float y = read_float(rdram, camera, 0x78);
    v.game_fov[0] = x;
    v.game_fov[1] = y;
    v.widened[0] = v.widened[1] = 1.0f;
    if (v.widening == 1.0f || x <= 0.0f || y <= 0.0f || x >= 180.0f || y >= 180.0f) {
        return;
    }
    const float wide_x = std::min(from_half_tan(half_tan(x) * v.widening), max_fov);
    const float wide_y = std::min(from_half_tan(half_tan(y) * v.widening), max_fov);
    write_float(rdram, camera, 0x74, wide_x);
    write_float(rdram, camera, 0x78, wide_y);
    v.widened[0] = half_tan(wide_x) / half_tan(x);
    v.widened[1] = half_tan(wide_y) / half_tan(y);
}

void conker::field_of_view::adjust_cull_scales(uint8_t* rdram, uint64_t camera_address) {
    const gpr camera = (gpr)camera_address;
    const uint32_t first = (uint32_t)MEM_W(0, (gpr)(int32_t)cameras);
    const uint32_t at = (uint32_t)camera;
    if (first == 0 || at < first || (at - first) % camera_size != 0 || (at - first) / camera_size >= (uint32_t)view_cameras) {
        return;
    }
    const View& v = views[(at - first) / camera_size];
    if (v.widened[0] == 1.0f && v.widened[1] == 1.0f) {
        return;
    }
    // The game's scales at its own fields of view (as updateCullScales_1510B958 works them out),
    // then as much smaller as the view is wider.
    const gpr scales = (gpr)(int32_t)cull_scales;
    for (int i = 0; i < 2; i++) {
        const float base_fov = read_float(rdram, camera, 0x6C + i * 4);
        const float base_scale = read_float(rdram, camera, 0x64 + i * 4);
        if (base_fov <= 0.0f) {
            continue;
        }
        const float scale = base_scale - (v.game_fov[i] / base_fov - 1.0f);
        write_float(rdram, scales, i * 4, scale / v.widened[i]);
    }
}

void conker::field_of_view::reset() {
    for (View& v : views) {
        v = View{};
    }
}
