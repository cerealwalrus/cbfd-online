// (0.11.5) Banjo moved by Banjo-Kazooie's own animations, played as Banjo-Kazooie plays them.
//
// His model is Banjo-Tooie's own (tools/bkchar/banjo.py: assets/banjo/banjo.bin), each of its bones on one of the game's 28; its
// skeleton is the one Banjo-Kazooie's animations were made for (bones the same, at the same places), so they move him as they moved
// him there. assets/banjo/bk_anims.bin holds that skeleton (its bones' pivots, ids and parents), which of its bones each of the
// game's 28 follows, and his animations as Banjo-Kazooie has them (each bone's turns, scales and moves, by frame, with its own curves).
// They're read (core2/animationfile.c), the skeleton's matrices made (code_630D0.c, animMtxList_setBoned) and blended from one
// animation into the next (code_B3580.c) as Banjo-Kazooie does; which animation, how fast, and where in it, is Banjo-Kazooie's own
// player code's choice (core2/bs/*.c: stand, walk, jump, fall, crouch, turn, ow, die) made from what the game's character does: its
// speed picks creeping, walking or running and how long each cycle takes (bswalk.c's speeds and times), a jump plays his jump's rise,
// its top and its landing (bsjump.c's ranges and times), and so on. His eyes blink when Banjo-Kazooie's do (code_90E0.c, baeyes.c).
// Here only his pose in his own model's space; char_anim.cpp puts it in the world (and gives his arms the game's aim when he holds a
// weapon).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "recomp.h"
#include "conker.hpp"
#if defined(CONKER_RT64)
#include "recompui/recompui.h"
#include "util/file.h"
#endif

namespace {
    constexpr double kPi = 3.141592654;   // (Banjo-Kazooie's own, BAD_PI)

    // ---- the pack ----
    struct Key { int frame; bool spline_in, spline_out; double v; };   // (unk0_15: from the next key's curve; unk0_14: the last's)
    struct Track { int bone; int type; std::vector<Key> keys; };       // (type: 0-2 turns x y z (degrees), 3-5 scales, 6-8 moves)
    struct Anim { int start = 0, end = 1; std::vector<Track> tracks; };
    struct Bone { double p[3]; int id; int parent; };
    struct Pack {
        bool tried = false, ok = false;
        double F = 1.5, K = 10.0;
        std::vector<Bone> bones;
        int slot_bk[28] = {};
        double B[28][3][3] = {};
        std::map<int, Anim> anims;
    } pack;

    uint32_t be32(const std::vector<uint8_t>& b, size_t at) { return ((uint32_t)b[at] << 24) | ((uint32_t)b[at + 1] << 16) | ((uint32_t)b[at + 2] << 8) | b[at + 3]; }
    int16_t be16(const std::vector<uint8_t>& b, size_t at) { return (int16_t)(((uint16_t)b[at] << 8) | b[at + 1]); }
    double bef(const std::vector<uint8_t>& b, size_t at) { const uint32_t w = be32(b, at); float f; std::memcpy(&f, &w, 4); return f; }

    bool parse_anim(const std::vector<uint8_t>& b, size_t at, size_t size, Anim& a) {
        if (size < 8 || at + size > b.size()) return false;
        const size_t end = at + size;
        a.start = be16(b, at);
        a.end = be16(b, at + 2);
        const int n = be16(b, at + 4);
        if (a.end <= a.start) return false;
        size_t p = at + 8;
        for (int i = 0; i < n; i++) {
            if (p + 4 > end) return false;
            const uint16_t w = (uint16_t)be16(b, p);
            const int cnt = be16(b, p + 2);
            p += 4;
            Track t;
            t.bone = w >> 4;
            t.type = w & 0xF;
            if (cnt <= 0 || t.type > 8 || p + 4 * (size_t)cnt > end) return false;
            for (int k = 0; k < cnt; k++) {
                const uint16_t f = (uint16_t)be16(b, p);
                Key key;
                key.spline_in = ((f >> 15) & 1) != 0;
                key.spline_out = ((f >> 14) & 1) != 0;
                key.frame = f & 0x3FFF;
                key.v = be16(b, p + 2) / 64.0;
                t.keys.push_back(key);
                p += 4;
            }
            a.tracks.push_back(std::move(t));
        }
        return true;
    }

    void load_pack() {
        if (pack.tried) return;
        pack.tried = true;
        std::vector<uint8_t> b;
#if defined(CONKER_RT64)
        std::ifstream f(recompui::file::get_asset_path("banjo/bk_anims.bin"), std::ios::binary);
#else
        const char* path = std::getenv("BK_ANIMS");
        std::ifstream f(path != nullptr ? path : "bk_anims.bin", std::ios::binary);
#endif
        if (f) b.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (b.size() < 20 || std::memcmp(b.data(), "BKAN", 4) != 0 || be32(b, 4) != 1) return;
        pack.F = bef(b, 8);
        pack.K = bef(b, 12);
        const uint32_t nb = be32(b, 16);
        size_t at = 20;
        if (nb == 0 || nb > 256 || at + 16 * (size_t)nb + 28 * 40 + 4 > b.size()) return;
        for (uint32_t i = 0; i < nb; i++, at += 16) {
            Bone bone;
            for (int j = 0; j < 3; j++) bone.p[j] = bef(b, at + 4 * j);
            bone.id = be16(b, at + 12);
            bone.parent = be16(b, at + 14);
            if (bone.parent >= (int)i || bone.id < 0 || bone.id >= 128) return;   // (parents before their children)
            pack.bones.push_back(bone);
        }
        for (int s = 0; s < 28; s++, at += 40) {
            pack.slot_bk[s] = (int32_t)be32(b, at);
            if (pack.slot_bk[s] < 0 || pack.slot_bk[s] >= (int)nb) return;
            for (int i = 0; i < 9; i++) pack.B[s][i / 3][i % 3] = bef(b, at + 4 + 4 * i);
        }
        const uint32_t na = be32(b, at);
        at += 4;
        if (na > 512 || at + 12 * (size_t)na > b.size()) return;
        for (uint32_t i = 0; i < na; i++, at += 12) {
            Anim a;
            if (parse_anim(b, be32(b, at + 4), be32(b, at + 8), a)) pack.anims[(int)be32(b, at)] = std::move(a);
        }
        pack.ok = !pack.anims.empty();
    }

    // ---- an animation at a time (core2/animationfile.c) ----
    double catmull_rom(double x, const double k[4]) {
        x = std::clamp(x, 0.0, 1.0);
        const double a2 = -0.5 * k[0] + 1.5 * k[1] - 1.5 * k[2] + 0.5 * k[3];
        const double a1 = 1.0 * k[0] - 2.5 * k[1] + 2.0 * k[2] - 0.5 * k[3];
        const double a0 = -0.5 * k[0] + 0.5 * k[2];
        return ((a2 * x + a1) * x + a0) * x + k[1];
    }
    constexpr double kDefault[9] = { 0, 0, 0, 1, 1, 1, 0, 0, 0 };
    double track_at(const Anim& a, const Track& t, double time) {
        const std::vector<Key>& K = t.keys;
        const int n = (int)K.size();
        const int it = (int)time;
        if (it < K[0].frame) {
            const double k[4] = { kDefault[t.type], kDefault[t.type], K[0].v, (K[0].spline_in && n >= 2) ? K[1].v : K[0].v };
            return catmull_rom((time - a.start) / std::max(1e-9, (double)(K[0].frame - a.start)), k);
        }
        if (it >= K[n - 1].frame) {
            const double k1 = K[n - 1].v;
            const double k0 = (K[n - 1].spline_out && n >= 2) ? K[n - 2].v : k1;
            const double k[4] = { k0, k1, k1, k1 };
            return catmull_rom(time - K[n - 1].frame, k);
        }
        int lo = 0, hi = n - 1;
        int i = lo + 1;
        while (i < hi) {
            i = lo + (hi - lo) / 2;
            if (K[i].frame <= it) lo = i;
            else hi = i;
            i = lo + 1;
        }
        const double k1 = K[lo].v, k2 = K[hi].v;
        const double f = (time - K[lo].frame) / std::max(1e-9, (double)(K[hi].frame - K[lo].frame));
        if (!K[lo].spline_out && !K[hi].spline_in) return k1 + (k2 - k1) * f;
        const double k0 = (K[lo].spline_out && lo - 1 >= 0) ? K[lo - 1].v : k1;
        const double k3 = (K[hi].spline_in && hi + 1 < n) ? K[hi + 1].v : k2;
        const double k[4] = { k0, k1, k2, k3 };
        return catmull_rom(f, k);
    }

    // a bone's turn (a quaternion), scale and move
    struct Quat { double x = 0, y = 0, z = 0, w = 1; };
    struct Xf { Quat q; double s[3] = { 1, 1, 1 }; double t[3] = { 0, 0, 0 }; };
    constexpr int kIds = 128;
    struct Pose { Xf b[kIds]; };

    // the turn of Banjo-Kazooie's angles (x, then y, then z: Rz Ry Rx, as func_80345CD4 builds it) as a quaternion
    void mat_of_euler(double rx, double ry, double rz, double R[3][3]) {
        const double d = kPi / 180.0;
        const double cx = std::cos(rx * d), sx = std::sin(rx * d), cy = std::cos(ry * d), sy = std::sin(ry * d);
        const double cz = std::cos(rz * d), sz = std::sin(rz * d);
        const double Rx[3][3] = { { 1, 0, 0 }, { 0, cx, -sx }, { 0, sx, cx } };
        const double Ry[3][3] = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };
        const double Rz[3][3] = { { cz, -sz, 0 }, { sz, cz, 0 }, { 0, 0, 1 } };
        double T[3][3];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) T[i][j] = Ry[i][0] * Rx[0][j] + Ry[i][1] * Rx[1][j] + Ry[i][2] * Rx[2][j];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i][j] = Rz[i][0] * T[0][j] + Rz[i][1] * T[1][j] + Rz[i][2] * T[2][j];
    }
    Quat quat_of(const double m[3][3]) {
        Quat q;
        const double tr = m[0][0] + m[1][1] + m[2][2];
        if (tr > 0) {
            const double s = std::sqrt(tr + 1.0) * 2;
            q.w = 0.25 * s; q.x = (m[2][1] - m[1][2]) / s; q.y = (m[0][2] - m[2][0]) / s; q.z = (m[1][0] - m[0][1]) / s;
        } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
            const double s = std::sqrt(std::max(1e-12, 1.0 + m[0][0] - m[1][1] - m[2][2])) * 2;
            q.w = (m[2][1] - m[1][2]) / s; q.x = 0.25 * s; q.y = (m[0][1] + m[1][0]) / s; q.z = (m[0][2] + m[2][0]) / s;
        } else if (m[1][1] > m[2][2]) {
            const double s = std::sqrt(std::max(1e-12, 1.0 + m[1][1] - m[0][0] - m[2][2])) * 2;
            q.w = (m[0][2] - m[2][0]) / s; q.x = (m[0][1] + m[1][0]) / s; q.y = 0.25 * s; q.z = (m[1][2] + m[2][1]) / s;
        } else {
            const double s = std::sqrt(std::max(1e-12, 1.0 + m[2][2] - m[0][0] - m[1][1])) * 2;
            q.w = (m[1][0] - m[0][1]) / s; q.x = (m[0][2] + m[2][0]) / s; q.y = (m[1][2] + m[2][1]) / s; q.z = 0.25 * s;
        }
        return q;
    }
    void mat_of_quat(Quat q, double m[3][3]) {
        const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (n < 1e-12) { for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m[i][j] = i == j ? 1.0 : 0.0; return; }
        q.x /= n; q.y /= n; q.z /= n; q.w /= n;
        m[0][0] = 1 - 2 * (q.y * q.y + q.z * q.z); m[0][1] = 2 * (q.x * q.y - q.z * q.w); m[0][2] = 2 * (q.x * q.z + q.y * q.w);
        m[1][0] = 2 * (q.x * q.y + q.z * q.w); m[1][1] = 1 - 2 * (q.x * q.x + q.z * q.z); m[1][2] = 2 * (q.y * q.z - q.x * q.w);
        m[2][0] = 2 * (q.x * q.z - q.y * q.w); m[2][1] = 2 * (q.y * q.z + q.x * q.w); m[2][2] = 1 - 2 * (q.x * q.x + q.y * q.y);
    }
    // (func_803458E4: the shorter way round, then func_80345650's slerp)
    Quat slerp(Quat a, Quat b, double t) {
        double d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (d < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; d = -d; }
        double wa = 1.0 - t, wb = t;
        if (1.0 - d > 1e-5) {
            const double th = std::acos(std::clamp(d, -1.0, 1.0)), s = std::sin(th);
            if (s != 0.0) { wa = std::sin((1.0 - t) * th) / s; wb = std::sin(t * th) / s; }
        }
        Quat q;
        q.x = wa * a.x + wb * b.x; q.y = wa * a.y + wb * b.y; q.z = wa * a.z + wb * b.z; q.w = wa * a.w + wb * b.w;
        return q;
    }

    // the pose of animation `a` at `progress` (0 to 1): each bone it moves, the rest as they stand
    void eval(const Anim& a, double progress, Pose& out) {
        out = Pose{};
        const double time = a.start + progress * (a.end - a.start);
        double v[kIds][9];
        bool set[kIds] = {};
        for (const Track& t : a.tracks) {
            if (t.bone < 0 || t.bone >= kIds || t.keys.empty()) continue;
            if (!set[t.bone]) {
                for (int i = 0; i < 9; i++) v[t.bone][i] = kDefault[i];
                set[t.bone] = true;
            }
            v[t.bone][t.type] = track_at(a, t, time);
        }
        for (int id = 0; id < kIds; id++) {
            if (!set[id]) continue;
            double R[3][3];
            mat_of_euler(v[id][0], v[id][1], v[id][2], R);
            out.b[id].q = quat_of(R);
            for (int i = 0; i < 3; i++) { out.b[id].s[i] = v[id][3 + i]; out.b[id].t[i] = v[id][6 + i]; }
        }
    }
    void blend(const Pose& a, const Pose& b, double t, Pose& out) {
        for (int id = 0; id < kIds; id++) {
            out.b[id].q = slerp(a.b[id].q, b.b[id].q, t);
            for (int i = 0; i < 3; i++) {
                out.b[id].s[i] = a.b[id].s[i] + (b.b[id].s[i] - a.b[id].s[i]) * t;
                out.b[id].t[i] = a.b[id].t[i] + (b.b[id].t[i] - a.b[id].t[i]) * t;
            }
        }
    }

    // ---- an animation playing (core2/anctrl.c): how far through, how long a loop takes, once or round and round, and the blend in
    // from the pose it started from ----
    enum class Mode { Loop, Once, Stopped };
    struct Ctrl {
        int anim = 0x6F;
        double timer = 0, duration = 5.5, sub_end = 1.0;
        Mode mode = Mode::Loop;
        double blend = 1.0, trans = 0.2;
        bool smooth = false;
        Pose from;
    };
    void current_pose(const Ctrl& c, Pose& out) {
        auto it = pack.anims.find(c.anim);
        Pose target;
        if (it != pack.anims.end()) eval(it->second, c.timer, target);
        if (c.smooth && c.blend < 1.0) blend(c.from, target, c.blend, out);
        else out = target;
    }
    void start(Ctrl& c, int anim, double duration, Mode mode, double start_at = 0.0, double trans = 0.2, bool smooth = true, double sub_end = 1.0) {
        if (smooth) current_pose(c, c.from);
        c.anim = pack.anims.count(anim) ? anim : 0x6F;
        c.duration = std::max(0.01, duration);
        c.mode = mode;
        c.timer = std::clamp(start_at, 0.0, 0.999999);
        c.trans = std::max(0.01, trans);
        c.smooth = smooth;
        c.blend = smooth ? 0.0 : 1.0;
        c.sub_end = sub_end;
    }
    void advance(Ctrl& c, double dt) {
        if (c.smooth && c.blend < 1.0) c.blend = std::min(1.0, c.blend + dt / c.trans);
        if (c.mode == Mode::Loop) {
            double t = c.timer + dt / c.duration;
            t -= std::floor(t);
            c.timer = t;
        } else if (c.mode == Mode::Once) {
            double t = c.timer + dt / c.duration;
            if (t > c.sub_end || t > 0.999999) {
                t = std::min(t, c.sub_end);
                if (t > 0.999999) t = 0.9999989867210388;
                c.mode = Mode::Stopped;
            }
            c.timer = t;
        }
    }

    // ---- Banjo, each character (its state as Banjo-Kazooie's player has one) ----
    enum class St { Idle, Walk, Skid, Jump, Fall, Land, Crouch, Ow, Die };
    enum class Gait { None, Creep, Slow, Walk, Fast };
    struct Banjo {
        bool have = false;
        uint32_t frame = 0;
        St st = St::Idle;
        Gait gait = Gait::None;
        int jump_phase = 0;
        double st_time = 0;
        double ground_y = 0;       // (where it took off: world)
        uint16_t game_anim = 0xFFFF;
        bool was_air = false;
        Ctrl c;
        // the eyes (baeyes.c: 0 open .. 1 shut, each blink there and back) and the blinks' times (code_90E0.c)
        double eye = 0, eye_dur = 0.1;
        int eye_state = 0;
        int blink_at = 0;
        double blink_t = 2.0;
    };
    Banjo banjos[16];

    // Banjo-Kazooie's blinks while he stands: (wait, then blink for)
    struct Blink { double wait; double dur; };
    constexpr Blink kBlinks[] = { { 2.0, 0.1 }, { 5.0, 0.06 }, { 0.2, 0.09 }, { 1.0, 0.1 }, { 5.0, 0.1 } };

    bool is_any(uint16_t a, std::initializer_list<int> list) {
        for (int x : list) if ((int)a == x) return true;
        return false;
    }

    // bswalk.c: each gait's speeds and its cycle's times at the slowest and fastest of them (clamped to 0.3 .. 1.5 seconds)
    struct GaitSpec { int anim; double v_lo, v_hi, d_slow, d_fast; };
    GaitSpec spec_of(Gait g) {
        switch (g) {
            case Gait::Creep: return { 0x02, 30.0, 80.0, 1.8, 1.2 };
            case Gait::Slow:  return { 0x03, 80.0, 150.0, 1.3, 0.6 };
            case Gait::Walk:  return { 0x0C, 150.0, 225.0, 0.92, 0.58 };
            case Gait::Fast:  return { 0x0C, 225.0, 500.0, 0.54, 0.44 };
            default:          return { 0x6F, 0, 1, 5.5, 5.5 };
        }
    }
    double gait_duration(Gait g, double v) {
        const GaitSpec s = spec_of(g);
        const double d = (v - s.v_lo) / (s.v_hi - s.v_lo) * (s.d_fast - s.d_slow) + s.d_slow;
        return std::clamp(d, 0.3, 1.5);
    }
    Gait gait_for(double v, Gait now) {
        // (a little either side of each line, so it doesn't flicker between two)
        const double h = 0.08;
        auto above = [&](double line, Gait g) { return v > line * ((now >= g) ? 1.0 - h : 1.0 + h); };
        if (above(225.0, Gait::Fast)) return Gait::Fast;
        if (above(150.0, Gait::Walk)) return Gait::Walk;
        if (above(80.0, Gait::Slow)) return Gait::Slow;
        if (above(18.0, Gait::Creep)) return Gait::Creep;
        return Gait::None;
    }

    void to_idle(Banjo& b) {
        b.st = St::Idle;
        b.gait = Gait::None;
        start(b.c, 0x6F, 5.5, Mode::Loop);
    }
    void to_walk(Banjo& b, Gait g, double v) {
        const GaitSpec s = spec_of(g);
        const bool from_skid = b.st == St::Skid;
        const bool keep = b.st == St::Walk && (b.c.anim == 0x03 || b.c.anim == 0x0C || b.c.anim == 0x02);
        const double at = keep ? b.c.timer : 0.0;
        const double trans = g == Gait::Fast ? 0.1 : g == Gait::Walk ? 0.14 : 0.2;
        if (b.st == St::Walk && b.c.anim == s.anim) {
            b.gait = g;   // (the same animation, faster or slower)
            return;
        }
        b.st = St::Walk;
        b.gait = g;
        start(b.c, s.anim, gait_duration(g, v), Mode::Loop, at, trans, !from_skid);
    }
}

namespace conker::bk_banjo {

bool ready() {
    load_pack();
    return pack.ok;
}

double model_scale() {
    load_pack();
    return pack.F;
}

void bind_turn(int slot, double out[3][3]) {
    load_pack();
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) out[i][j] = slot >= 0 && slot < 28 ? pack.B[slot][i][j] : (i == j ? 1.0 : 0.0);
}

void bind_vector(int from, int to, double out[3]) {
    load_pack();
    for (int i = 0; i < 3; i++) out[i] = 0;
    if (!pack.ok || from < 0 || from >= 28 || to < 0 || to >= 28) return;
    const double* a = pack.bones[(size_t)pack.slot_bk[from]].p;
    const double* c = pack.bones[(size_t)pack.slot_bk[to]].p;
    for (int i = 0; i < 3; i++) {
        out[i] = pack.F * (pack.B[from][i][0] * (c[0] - a[0]) + pack.B[from][i][1] * (c[1] - a[1]) + pack.B[from][i][2] * (c[2] - a[2]));
    }
}

std::string state_of(int k) {
    if (k < 0 || k >= 16 || !banjos[k].have) return "-";
    static const char* names[] = { "idle", "walk", "skid", "jump", "fall", "land", "crouch", "ow", "die" };
    const Banjo& b = banjos[k];
    char line[160];
    std::snprintf(line, sizeof(line), "%s (Banjo-Kazooie's %04X at %.3f, %.2fs a loop, blended %.2f, jump %d, ground %.1f)", names[(int)b.st],
        (unsigned)b.c.anim, b.c.timer, b.c.duration, b.c.smooth ? b.c.blend : 1.0, b.jump_phase, b.ground_y);
    return line;
}

bool pose(int k, const Input& in, SlotXf out[28], int* eye_slot) {
    load_pack();
    if (!pack.ok) return false;
    Banjo dummy;
    Banjo& b = (k >= 0 && k < 16 && in.have) ? banjos[k] : dummy;
    double dt;
    if (!in.have) {
        // (no match: the select screen and such: standing, its time the clock's)
        static const auto t0 = std::chrono::steady_clock::now();
        const double select_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        b.c.anim = 0x6F;
        b.c.duration = 5.5;
        b.c.timer = std::fmod(select_time / 5.5, 1.0);
        b.c.smooth = false;
        dt = 0;
    } else {
        if (!b.have) {
            b = Banjo{};
            b.have = true;
            b.frame = in.frame;
            b.ground_y = in.y;
            b.was_air = in.airborne;
            to_idle(b);
            b.c.smooth = false;
        }
        const int frames = (int)(in.frame - b.frame);
        dt = std::clamp(frames, 0, 6) / 30.0;
        b.frame = in.frame;
    }

    if (in.have && dt > 0) {
        const double v = in.speed_bk;            // (Banjo-Kazooie's units a second, horizontal)
        const uint16_t ga = in.game_anim;
        const bool dying = in.life != 0 || is_any(ga, { 0x0058, 0x0059, 0x0013, 0x017E, 0x016F });
        const bool hurt = is_any(ga, { 0x011D, 0x019C, 0x00F3, 0x00F4 });   // (hit; set on fire)
        const bool crouch = ga == 0x0016;
        const bool skid = in.skid && ga == 0x007A;
        const bool jumped = is_any(ga, { 0x0032, 0x0037 }) && (ga != b.game_anim);
        const bool air = in.airborne;
        const bool took_off = (air && !b.was_air) || jumped;
        const bool landed = !air && b.was_air;
        if (!air && !b.was_air) b.ground_y = in.y;                 // (where it last stood)
        const double height = (in.y - b.ground_y) * in.to_bk;   // (above that, Banjo-Kazooie's units)
        b.st_time += dt;
        auto set = [&](St s) { b.st = s; b.st_time = 0; };

        if (dying) {
            if (b.st != St::Die) {
                set(St::Die);
                b.jump_phase = 0;
                start(b.c, 0x09, 2.0, Mode::Once, 0.0, 0.2, false, 0.3356);   // (bsdie.c: the hit and the first fall)
                b.eye_state = 5; b.eye_dur = 0.09;
            } else if (b.jump_phase == 0 && b.c.mode == Mode::Stopped) {
                b.jump_phase = 1;
                b.c.sub_end = 1.0;      // (the bounce, then over)
                b.c.mode = Mode::Once;
            } else if (b.jump_phase == 1 && b.c.timer >= 0.6538) {
                b.jump_phase = 2;
                b.c.duration = 4.0;
            }
        } else if (b.st == St::Die) {
            to_idle(b);                 // (back again, somewhere else: standing at once)
            b.c.smooth = false;
            b.c.blend = 1.0;
            b.eye_state = 4; b.eye_dur = 0.034;
        } else if (hurt && b.st != St::Ow) {
            set(St::Ow);
            start(b.c, 0x4D, 1.7, Mode::Once, 0.0, 0.2, false, 0.5344);    // (bsow.c)
            b.eye_state = 5; b.eye_dur = 0.09;
        } else if (b.st == St::Ow) {
            if (b.c.timer >= 0.3 && b.eye_state == 0 && b.eye > 0.5) { b.eye_state = 4; b.eye_dur = 0.034; }
            if (b.c.mode == Mode::Stopped || (!hurt && b.st_time > 0.5)) {
                const Gait g = gait_for(v, Gait::None);
                if (g != Gait::None) to_walk(b, g, v);
                else to_idle(b);
                b.eye_state = 4; b.eye_dur = 0.034;
            }
        } else if (took_off && !crouch) {
            // bsjump.c: from 0.3 to 0.5042 over its rise (1.9 seconds a whole), then on slowly to 0.6667 while it's up (4 seconds),
            // and the rest as it comes down to land
            set(St::Jump);
            b.jump_phase = 0;
            start(b.c, 0x08, 1.9, Mode::Once, 0.3, 0.134, true, 0.5042);
        } else if (b.st == St::Jump || b.st == St::Fall) {
            const bool near_ground = in.vy < 0 && height < (b.jump_phase == 1 ? 90.0 : 130.0);
            if (b.st == St::Jump) {
                if (b.jump_phase == 0 && b.c.mode == Mode::Stopped) {
                    b.c.sub_end = 0.6667; b.c.duration = 4.0; b.c.mode = Mode::Once; b.jump_phase = 1;
                }
                if (b.jump_phase == 0 && near_ground) {
                    b.c.sub_end = 1.0; b.c.duration = 1.4; b.c.mode = Mode::Once; b.jump_phase = 2;
                } else if (b.jump_phase == 1 && near_ground) {
                    b.c.sub_end = 1.0; b.c.duration = 2.0; b.c.mode = Mode::Once; b.jump_phase = 2;
                }
            }
            // (a fall's landing, bsjump_fall's: the jump's from 0.6667, as it lands, the floor below not known here)
            if (landed || (!air && b.st_time > 0.1)) {
                set(St::Land);
                if (b.c.anim != 0x08 || b.c.timer < 0.6667) start(b.c, 0x08, 2.0, Mode::Once, 0.6667, 0.1, true, 1.0);
                else { b.c.sub_end = 1.0; if (b.c.mode == Mode::Stopped && b.c.timer < 0.999) b.c.mode = Mode::Once; }
            }
        } else if (air && !crouch && in.vy < 0 && b.st_time > 0.05) {
            // bsjump.c's fall: falling off something
            set(St::Fall);
            b.jump_phase = 0;
            start(b.c, 0xB0, 0.38, Mode::Loop, 0.0, 0.3, true);
        } else if (crouch) {
            // bscrouch.c: down (half a second), then still; shuffling round as it moves
            if (b.st != St::Crouch) {
                set(St::Crouch);
                start(b.c, 0x01, 0.5, Mode::Once);
            } else if (v > 18.0) {
                if (b.c.anim != 0x10C) start(b.c, 0x10C, 0.5, Mode::Loop, 0.0, 0.2, true);
                b.c.duration = std::clamp(0.5 - v / 180.0 * 0.3, 0.2, 0.5);
            } else if (b.c.anim == 0x10C) {
                start(b.c, 0x10C, 0.5, Mode::Once, 0.9999, 0.2, true);
            }
        } else if (skid && b.st != St::Skid && v > 125.0) {
            set(St::Skid);
            start(b.c, 0x0E, 0.3, Mode::Once, 0.0, 0.1, true);                   // (bsturn.c)
        } else {
            const Gait g = gait_for(v, b.st == St::Walk ? b.gait : Gait::None);
            if (b.st == St::Skid && b.c.mode != Mode::Stopped) {
                // (turning round)
            } else if (b.st == St::Land && b.c.mode != Mode::Stopped && g == Gait::None) {
                // (landing, still)
            } else if (g != Gait::None) {
                to_walk(b, g, v);
            } else if (b.st != St::Idle) {
                to_idle(b);
            }
            if (b.st == St::Walk) b.c.duration = gait_duration(b.gait, v);
        }
        b.game_anim = ga;
        b.was_air = air;

        // the eyes: Banjo-Kazooie's blinks (each eye shut over its time and open again), as he stands
        if (b.st == St::Idle || b.st == St::Walk) {
            b.blink_t -= dt;
            if (b.blink_t <= 0) {
                const Blink& bl = kBlinks[b.blink_at % 5];
                b.eye_state = 1;
                b.eye_dur = bl.dur;
                b.blink_at = (b.blink_at + 1) % 5;
                b.blink_t = kBlinks[b.blink_at % 5].wait;
            }
        }
        switch (b.eye_state) {
            case 1: b.eye += dt / b.eye_dur; if (b.eye > 1.0) { b.eye = 1.0; b.eye_state = 3; } break;
            case 3: case 4: b.eye -= dt / b.eye_dur; if (b.eye < 0.0) { b.eye = 0.0; b.eye_state = 0; } break;
            case 5: b.eye += dt / b.eye_dur; if (b.eye > 1.0) { b.eye = 1.0; b.eye_state = 0; } break;
            default: break;
        }
        advance(b.c, dt);
    }

    // ---- the pose: each bone's matrix (code_630D0.c: its parent's, then to its pivot moved by the animation's move times K, turned,
    // scaled, and back from its pivot), then each of the game's 28 ----
    Pose p;
    current_pose(b.c, p);
    const size_t nb = pack.bones.size();
    std::vector<std::array<double, 12>> W(nb);   // (3x3 by rows, then the place: column vectors)
    for (size_t i = 0; i < nb; i++) {
        const Bone& bone = pack.bones[i];
        const Xf& x = p.b[bone.id];
        double R[3][3];
        mat_of_quat(x.q, R);
        double L[3][3];
        for (int r = 0; r < 3; r++) for (int c2 = 0; c2 < 3; c2++) L[r][c2] = R[r][c2] * x.s[c2];
        double lt[3];
        for (int r = 0; r < 3; r++) {
            lt[r] = bone.p[r] + pack.K * x.t[r] - (L[r][0] * bone.p[0] + L[r][1] * bone.p[1] + L[r][2] * bone.p[2]);
        }
        std::array<double, 12>& w = W[i];
        if (bone.parent < 0) {
            for (int r = 0; r < 3; r++) { for (int c2 = 0; c2 < 3; c2++) w[3 * r + c2] = L[r][c2]; w[9 + r] = lt[r]; }
        } else {
            const std::array<double, 12>& P = W[bone.parent];
            for (int r = 0; r < 3; r++) {
                for (int c2 = 0; c2 < 3; c2++) w[3 * r + c2] = P[3 * r] * L[0][c2] + P[3 * r + 1] * L[1][c2] + P[3 * r + 2] * L[2][c2];
                w[9 + r] = P[3 * r] * lt[0] + P[3 * r + 1] * lt[1] + P[3 * r + 2] * lt[2] + P[9 + r];
            }
        }
    }
    // the eyes: one look of them shown (open, half, shut), the others' bones shrunk to nothing
    const double e = b.eye;
    const int shown = e < 0.3 ? 14 : e < 0.7 ? 15 : 19;
    if (eye_slot != nullptr) *eye_slot = shown;
    for (int s = 0; s < 28; s++) {
        const int kb = pack.slot_bk[s];
        const std::array<double, 12>& w = W[(size_t)kb];
        const double* pv = pack.bones[(size_t)kb].p;
        // A = W3 B^T (the slot's vertices are B (p - pivot) F); its place W (pivot), times F
        for (int r = 0; r < 3; r++) {
            for (int c2 = 0; c2 < 3; c2++) {
                out[s].r[r][c2] = w[3 * r] * pack.B[s][c2][0] + w[3 * r + 1] * pack.B[s][c2][1] + w[3 * r + 2] * pack.B[s][c2][2];
            }
            out[s].p[r] = pack.F * (w[3 * r] * pv[0] + w[3 * r + 1] * pv[1] + w[3 * r + 2] * pv[2] + w[9 + r]);
        }
        if ((s == 14 || s == 15 || s == 19) && s != shown) {
            for (auto& row : out[s].r) for (double& x : row) x = 0.0;
        }
    }
    return true;
}

}  // namespace conker::bk_banjo
