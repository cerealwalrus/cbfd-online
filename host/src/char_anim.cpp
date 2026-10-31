// (0.11.5) The added characters' own way of moving: Cloud, Snake and Fox (and any other set up in assets/char_anim.txt). The game moves
// every added character with Conker's animations (3B's: a squirrel's lunging run with his arms held out like wings), which on a man or a
// fox look nothing like their own games (the 31 Oct videos: Zack in Crisis Core, Snake in Metal Gear Solid, Sabre in Dinosaur Planet).
// Here, as each one is drawn, its bones are posed our own way while it stands, walks and runs on the ground: a person's walk and run,
// each foot put down and kept where it is while the body goes over it (the stride as long as the ground covered, so the feet don't
// slide), the knees bending to reach (two bones to the ankle), the hips turning and dropping with the legs, the arms swinging against
// them (bent at the elbows on a run, pumping), the body upright with a lean into the run and into turns, the head steady. Everything
// else (jumping, falling, hit, dying, swimming, the weapons' own moves) stays the game's, toned down as biped_pose does, each blended
// into the other over a few frames; with a weapon in its hands its upper body stays the game's (it aims it).
// Banjo is posed by Banjo-Kazooie's own animations (bk_banjo.cpp), put in the world here, his arms the game's with a weapon out.
//
// Only the drawing (the bones' matrices, segment 3, after the game has made them): nothing of the game's state changes, so nothing is
// different from one machine to another online. Each one's numbers are in assets/char_anim.txt, read again whenever it changes (so
// they can be tuned while the game runs), and the game's animation numbers that count as standing, walking and running there too.
// anim_states.log: each player's animation as it changes (the numbers for that list), what they were doing then.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "recomp.h"
#include "conker.hpp"
#include "log_files.hpp"
#if defined(CONKER_RT64)
#include "recompui/recompui.h"
#include "util/file.h"
#endif

namespace {
    // ---- vectors and turns (row vectors, as the game's matrices: v' = v M; a turn's rows are where x, y and z go) ----
    // (the model's ways: x its left, y up, z forward)
    struct V3 { double x = 0, y = 0, z = 0; };
    inline V3 v3(double x, double y, double z) { V3 r; r.x = x; r.y = y; r.z = z; return r; }
    inline V3 operator+(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
    inline V3 operator-(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
    inline V3 operator*(V3 a, double s) { return v3(a.x * s, a.y * s, a.z * s); }
    inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
    inline double len(V3 a) { return std::sqrt(dot(a, a)); }
    inline V3 unit(V3 a) { const double n = len(a); return n > 1e-12 ? a * (1.0 / n) : v3(0, 1, 0); }

    struct M3 { double r[3][3]; };
    inline M3 ident() { M3 m{}; m.r[0][0] = m.r[1][1] = m.r[2][2] = 1.0; return m; }
    inline V3 row(const M3& m, int i) { return v3(m.r[i][0], m.r[i][1], m.r[i][2]); }
    inline M3 rows(V3 a, V3 b, V3 c) {
        M3 m{};
        const V3 v[3] = { a, b, c };
        for (int i = 0; i < 3; i++) { m.r[i][0] = v[i].x; m.r[i][1] = v[i].y; m.r[i][2] = v[i].z; }
        return m;
    }
    inline V3 mulv(V3 v, const M3& m) {
        return v3(v.x * m.r[0][0] + v.y * m.r[1][0] + v.z * m.r[2][0], v.x * m.r[0][1] + v.y * m.r[1][1] + v.z * m.r[2][1],
            v.x * m.r[0][2] + v.y * m.r[1][2] + v.z * m.r[2][2]);
    }
    inline M3 mul(const M3& a, const M3& b) {   // a, then b
        M3 m{};
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m.r[i][j] = a.r[i][0] * b.r[0][j] + a.r[i][1] * b.r[1][j] + a.r[i][2] * b.r[2][j];
        return m;
    }
    inline M3 transp(const M3& a) { M3 m{}; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m.r[i][j] = a.r[j][i]; return m; }
    bool inverse(const M3& a, M3& out) {
        const double (&m)[3][3] = a.r;
        const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
            m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        if (!(std::fabs(det) > 1e-12)) return false;
        const double d = 1.0 / det;
        out.r[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * d; out.r[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * d; out.r[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * d;
        out.r[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * d; out.r[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * d; out.r[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * d;
        out.r[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * d; out.r[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * d; out.r[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * d;
        return true;
    }
    // the turn by `ang` about `axis` (right-handed), row form: c I + (1 - c) a a^T - s [a]x
    M3 rot(V3 axis, double ang) {
        const V3 a = unit(axis);
        const double c = std::cos(ang), s = std::sin(ang), t = 1.0 - c;
        const double A[3] = { a.x, a.y, a.z };
        const double K[3][3] = { { 0, -a.z, a.y }, { a.z, 0, -a.x }, { -a.y, a.x, 0 } };
        M3 m{};
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m.r[i][j] = (i == j ? c : 0.0) + t * A[i] * A[j] - s * K[i][j];
        return m;
    }
    inline M3 pitch(double a) { return rot(v3(1, 0, 0), a); }   // up tips forward (+z) for a > 0 (forward tips down, down tips back)
    inline M3 yaw(double a) { return rot(v3(0, 1, 0), a); }     // forward turns to +x (the left) for a > 0
    inline M3 roll(double a) { return rot(v3(0, 0, 1), a); }    // up tips to -x (the right) for a > 0
    // the least turn taking `from`'s way to `to`'s
    M3 align(V3 from, V3 to) {
        const V3 f = unit(from), t = unit(to);
        const V3 ax = cross(f, t);
        const double s = len(ax), c = dot(f, t);
        if (s < 1e-9) {
            if (c > 0) return ident();
            const V3 other = std::fabs(f.x) < 0.9 ? v3(1, 0, 0) : v3(0, 1, 0);
            return rot(cross(f, other), 3.14159265358979);
        }
        return rot(ax, std::atan2(s, c));
    }
    M3 orthonormal(M3 a) {
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < i; j++) {
                const double d = a.r[i][0] * a.r[j][0] + a.r[i][1] * a.r[j][1] + a.r[i][2] * a.r[j][2];
                for (int t = 0; t < 3; t++) a.r[i][t] -= d * a.r[j][t];
            }
            const double n = std::sqrt(a.r[i][0] * a.r[i][0] + a.r[i][1] * a.r[i][1] + a.r[i][2] * a.r[i][2]);
            if (n > 1e-12) for (int t = 0; t < 3; t++) a.r[i][t] /= n;
        }
        return a;
    }
    // quaternions, for blending two turns
    struct Q4 { double w, x, y, z; };
    Q4 to_quat(const M3& rowform) {
        const M3 m = transp(rowform);   // column form
        const double tr = m.r[0][0] + m.r[1][1] + m.r[2][2];
        Q4 q{};
        if (tr > 0) {
            const double s = std::sqrt(tr + 1.0) * 2;
            q.w = 0.25 * s; q.x = (m.r[2][1] - m.r[1][2]) / s; q.y = (m.r[0][2] - m.r[2][0]) / s; q.z = (m.r[1][0] - m.r[0][1]) / s;
        } else if (m.r[0][0] > m.r[1][1] && m.r[0][0] > m.r[2][2]) {
            const double s = std::sqrt(std::max(1e-12, 1.0 + m.r[0][0] - m.r[1][1] - m.r[2][2])) * 2;
            q.w = (m.r[2][1] - m.r[1][2]) / s; q.x = 0.25 * s; q.y = (m.r[0][1] + m.r[1][0]) / s; q.z = (m.r[0][2] + m.r[2][0]) / s;
        } else if (m.r[1][1] > m.r[2][2]) {
            const double s = std::sqrt(std::max(1e-12, 1.0 + m.r[1][1] - m.r[0][0] - m.r[2][2])) * 2;
            q.w = (m.r[0][2] - m.r[2][0]) / s; q.x = (m.r[0][1] + m.r[1][0]) / s; q.y = 0.25 * s; q.z = (m.r[1][2] + m.r[2][1]) / s;
        } else {
            const double s = std::sqrt(std::max(1e-12, 1.0 + m.r[2][2] - m.r[0][0] - m.r[1][1])) * 2;
            q.w = (m.r[1][0] - m.r[0][1]) / s; q.x = (m.r[0][2] + m.r[2][0]) / s; q.y = (m.r[1][2] + m.r[2][1]) / s; q.z = 0.25 * s;
        }
        return q;
    }
    M3 from_quat(Q4 q) {
        const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        if (n < 1e-12) return ident();
        q.w /= n; q.x /= n; q.y /= n; q.z /= n;
        M3 c{};   // column form
        c.r[0][0] = 1 - 2 * (q.y * q.y + q.z * q.z); c.r[0][1] = 2 * (q.x * q.y - q.z * q.w); c.r[0][2] = 2 * (q.x * q.z + q.y * q.w);
        c.r[1][0] = 2 * (q.x * q.y + q.z * q.w); c.r[1][1] = 1 - 2 * (q.x * q.x + q.z * q.z); c.r[1][2] = 2 * (q.y * q.z - q.x * q.w);
        c.r[2][0] = 2 * (q.x * q.z - q.y * q.w); c.r[2][1] = 2 * (q.y * q.z + q.x * q.w); c.r[2][2] = 1 - 2 * (q.x * q.x + q.y * q.y);
        return transp(c);
    }
    M3 blend(const M3& a, const M3& b, double t) {
        if (t <= 0.0) return a;
        if (t >= 1.0) return b;
        Q4 qa = to_quat(a), qb = to_quat(b);
        double d = qa.w * qb.w + qa.x * qb.x + qa.y * qb.y + qa.z * qb.z;
        if (d < 0) { qb.w = -qb.w; qb.x = -qb.x; qb.y = -qb.y; qb.z = -qb.z; d = -d; }
        Q4 q{};
        if (d > 0.9995) {
            q.w = qa.w + (qb.w - qa.w) * t; q.x = qa.x + (qb.x - qa.x) * t; q.y = qa.y + (qb.y - qa.y) * t; q.z = qa.z + (qb.z - qa.z) * t;
        } else {
            const double th = std::acos(std::clamp(d, -1.0, 1.0)), sn = std::sin(th);
            const double wa = std::sin((1 - t) * th) / sn, wb = std::sin(t * th) / sn;
            q.w = qa.w * wa + qb.w * wb; q.x = qa.x * wa + qb.x * wb; q.y = qa.y * wa + qb.y * wb; q.z = qa.z * wa + qb.z * wb;
        }
        return from_quat(q);
    }
    // the turn cut down to k of its angle (about the same axis)
    M3 damp(const M3& r, double k) { return blend(ident(), r, k); }

    constexpr double pi = 3.14159265358979;
    constexpr double deg = pi / 180.0;
    inline double smooth01(double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3 - 2 * x); }
    inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
    inline double wrap01(double p) { p = std::fmod(p, 1.0); return p < 0 ? p + 1.0 : p; }

    float rf(uint8_t* rdram, uint32_t addr) {
        const uint32_t b = (uint32_t)MEM_W(0, (gpr)(int32_t)addr);
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }
    inline bool kseg0(uint32_t a) { return (a >> 24) == 0x80 && (a & 0x00FFFFFFu) < 0x007FF000u; }

    // ---- the log ----
    std::FILE* log_file = nullptr;
    int log_lines = 0;
    void say(const char* line) {
        if (log_lines > 8000) return;
        if (log_file == nullptr) {
            log_file = std::fopen(conker::logs::path("anim_states.log").string().c_str(), "w");
            if (log_file == nullptr) return;
        }
        log_lines++;
        std::fprintf(log_file, "%s\n", line);
        std::fflush(log_file);
    }

    // ---- (testing) anim_capture.bin: while assets/anim_capture.txt is there, character 0's every drawing (up to 3600): its place, its
    // game animation, how much of ours, the game's matrices and what was drawn (records of "ANIM", frame, k, anim, w, x, y, z, ours, then
    // 28 x 12 floats each, big-endian) ----
    std::FILE* capture_file = nullptr;
    int capture_count = 0;
    bool capture_on = false;
    uint32_t capture_checked = 0xFFFFFFFFu;
    void capture_put_u32(uint32_t v) {
        const uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
        std::fwrite(b, 1, 4, capture_file);
    }
    void capture_put_f(double v) {
        const float f = (float)v;
        uint32_t w;
        std::memcpy(&w, &f, 4);
        capture_put_u32(w);
    }

    // ---- assets/char_anim.txt: [tag] sections of `name = number` lines ([*] for all); in [*] `loco = ...` the game's animations (in
    // hexadecimal, as the log has them) that count as standing, walking and running, `keep = ...` ones left to the game whatever else ----
    struct Params {
        std::map<std::string, std::map<std::string, double>> by_tag;
        std::set<int> loco, keep;
        bool loaded = false;
        double get(const std::string& tag, const char* key, double fallback) const {
            for (const std::string& t : { tag, std::string("*") }) {
                auto it = by_tag.find(t);
                if (it != by_tag.end()) {
                    auto kv = it->second.find(key);
                    if (kv != it->second.end()) return kv->second;
                }
            }
            return fallback;
        }
    };
    Params params;
    std::filesystem::path params_path;
    std::filesystem::file_time_type params_time{};
    uint32_t params_checked = 0;
    std::string trim(const std::string& s) {
        const size_t a = s.find_first_not_of(" \t\r\n");
        const size_t b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
    void load_params(bool force) {
#if defined(CONKER_RT64)
        if (params_path.empty()) {
            params_path = recompui::file::get_asset_path("char_anim.txt");
        }
#else
        if (params_path.empty()) {
            const char* p = std::getenv("CHAR_ANIM_TXT");
            params_path = p != nullptr ? p : "char_anim.txt";
        }
#endif
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(params_path, ec);
        if (ec) {
            if (!params.loaded) {
                params.loaded = true;   // (none: nothing of ours, all the game's)
                say("[params] no char_anim.txt");
            }
            return;
        }
        if (!force && params.loaded && t == params_time) return;
        params_time = t;
        std::ifstream in(params_path);
        if (!in) return;
        Params p;
        std::string line, tag = "*";
        while (std::getline(in, line)) {
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            line = trim(line);
            if (line.empty()) continue;
            if (line.front() == '[' && line.back() == ']') {
                tag = trim(line.substr(1, line.size() - 2));
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
            if (key == "loco" || key == "keep") {
                std::stringstream ss(value);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    item = trim(item);
                    if (item.empty()) continue;
                    (key == "loco" ? p.loco : p.keep).insert((int)std::strtol(item.c_str(), nullptr, 16));
                }
                continue;
            }
            p.by_tag[tag][key] = std::strtod(value.c_str(), nullptr);
        }
        p.loaded = true;
        params = std::move(p);
        char msg[200];
        std::snprintf(msg, sizeof(msg), "[params] f=%u char_anim.txt read: %zu sections, %zu walking animations, %zu kept", (unsigned)0,
            params.by_tag.size(), params.loco.size(), params.keep.size());
        say(msg);
    }

    // ---- each character's state ----
    constexpr uint32_t characters = 0x800CC2D0u, character_size = 0x32Cu;
    constexpr int max_chars = 16;
    struct State {
        bool have = false;
        double pos[3] = {};
        double vel[3] = {};        // world units a frame, smoothed
        double speed = 0;          // horizontal, smoothed
        double vy = 0;             // up, smoothed
        double w_loco = 0;         // how much of ours is shown (0: the game's)
        double t = 0;              // frames (for breathing)
        uint16_t anim = 0xFFFF;
        uint32_t anim_age = 0;
        uint8_t weapon = 0, scoped = 0, life = 0;
        int32_t pickup = -1;       // (the match's pickup it holds, D_800D23B0's +0xEB0, -1 none)
        int16_t look = 0;
        float ctl_last[4] = {};    // (the animation's record, +0x2D0: its numbers as the last one ended, for the log)
        // from the drawings
        uint32_t frame_drawn = 0xFFFFFFFFu;
        double phase = 0;          // the stride's cycle: 0 the left foot put down, 0.5 the right
        double back = 0;           // 1 walking backward (smoothed)
        bool have_obj = false;
        V3 last_obj;
        double turn = 0;           // facing's change a frame (radians), smoothed
        double last_yaw = 0;
        bool have_yaw = false;
        double ground_c = 0;       // where its feet stand, from its place (+0x18)
        bool have_ground = false;
        V3 root_off;               // the root's place from the character's, smoothed (the game's)
        bool have_root_off = false;
        double root_y = 0;         // our root's height (smoothed a little)
        // in the air (a running jump keeps the game's run): taken off, or falling (gravity's pull frame after frame); down again
        // when the fall stops short
        bool airborne = false;
        double air = 0, land = 0, vy_raw = 0;
        double anim_t = 0;         // the game's animation's progress (its record's frame over its length, +0x2D0's +0x8 / +0x18)
        double sword_w = 0;        // how much of our sword's holding and swinging is shown
        int fall_n = 0, flat_n = 0;
        bool have_root_y = false;
        double banjo_arm = 0;      // (Banjo) how much his arms are the game's (a weapon in his hands)
        uint32_t banjo_frame = 0xFFFFFFFFu;
        double banjo_yaw = 0;      // (Banjo) the way he faces, as drawn (turned toward the character's at a turning speed)
        bool banjo_have_yaw = false;
        double banjo_x = 0, banjo_z = 0;   // (Banjo) where he stood when last posed (a jump across the map, as he starts again: turned at once)
    };
    State st[max_chars];
    uint32_t frames = 0;
}

// ---- once a game frame ----
void conker::char_anim::frame(uint8_t* rdram) {
    frames++;
    if (frames - params_checked >= 30 || !params.loaded) {
        params_checked = frames;
        load_params(false);
    }
    if (frames / 30 != capture_checked) {
        capture_checked = frames / 30;
#if defined(CONKER_RT64)
        std::error_code ec;
        capture_on = std::filesystem::exists(recompui::file::get_asset_path("anim_capture.txt"), ec);
#endif
        if (!capture_on && capture_file != nullptr) {
            std::fclose(capture_file);   // (taken away: the next time it's there, a new file)
            capture_file = nullptr;
            capture_count = 0;
        }
        if (capture_on && capture_file == nullptr) {
            static int capture_files = 0;
            char name[48];
            std::snprintf(name, sizeof(name), "anim_capture_%d.bin", ++capture_files);
            capture_file = std::fopen(conker::logs::path(name).string().c_str(), "wb");
            capture_count = 0;
        }
        if (capture_file != nullptr) std::fflush(capture_file);
    }
    const bool match = MEM_BU(0, (gpr)(int32_t)0x800BE616) != 0;
    if (!match) {
        for (State& s : st) s = State{};
        return;
    }
    const int count = std::clamp((int)MEM_B(0, (gpr)(int32_t)0x8008FD8Cu), 0, max_chars);
    const int humans = std::max((int)(int8_t)MEM_B(0, (gpr)(int32_t)0x8008FE40u), 1);
    const double max_vy = params.get("*", "max_vy", 30.0);
    const bool loco_any = params.get("*", "loco_any", 0.0) > 0.5;
    const double blend_in = params.get("*", "blend_in", 0.25), blend_out = params.get("*", "blend_out", 0.34);
    for (int k = 0; k < count; k++) {
        State& s = st[k];
        const uint32_t o = characters + (uint32_t)k * character_size;
        const gpr ob = (gpr)(int32_t)o;
        const double p[3] = { rf(rdram, o + 0x14), rf(rdram, o + 0x18), rf(rdram, o + 0x1C) };
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
        const uint32_t aim = (uint32_t)MEM_W(0x31C, ob);
        const uint32_t cam = (uint32_t)MEM_W(0x318, ob);
        const uint32_t ctl = (uint32_t)MEM_W(0x2D0, ob);
        const uint8_t weapon = kseg0(aim) ? MEM_BU(0x78, (gpr)(int32_t)aim) : 0;
        const uint8_t scoped = kseg0(aim) ? MEM_BU(0x197, (gpr)(int32_t)aim) : 0;
        const uint8_t life = kseg0(aim) ? MEM_BU(0x120, (gpr)(int32_t)aim) : 0;
        const int16_t look = kseg0(cam) ? (int16_t)MEM_H(0x73C, (gpr)(int32_t)cam) : 0;
        const uint16_t anim = (uint16_t)MEM_HU(0x84, ob);
        float ctl_now[4] = {};
        if (kseg0(ctl)) {
            ctl_now[0] = rf(rdram, ctl + 0x8);
            ctl_now[1] = rf(rdram, ctl + 0xC);
            ctl_now[2] = rf(rdram, ctl + 0x10);
            ctl_now[3] = rf(rdram, ctl + 0x18);
        }
        double v[3] = { 0, 0, 0 };
        if (s.have) {
            for (int i = 0; i < 3; i++) v[i] = p[i] - s.pos[i];
            if (std::fabs(v[0]) + std::fabs(v[1]) + std::fabs(v[2]) > 400.0) {
                // (a respawn: begun again)
                for (double& c : v) c = 0;
                s.speed = 0;
                s.vy = 0;
                s.w_loco = 0;
                s.have_obj = false;
                s.have_root_y = false;
            }
        }
        const double horiz = std::sqrt(v[0] * v[0] + v[2] * v[2]);
        s.speed += (horiz - s.speed) * 0.4;
        s.vy += (v[1] - s.vy) * 0.5;
        {
            const double acc = v[1] - s.vy_raw;
            const double before = s.vy_raw;
            s.vy_raw = v[1];
            if (!s.airborne) {
                if (v[1] > 6.0 && acc > 6.0) {
                    s.airborne = true;   // (took off)
                } else if (acc < -1.2 && v[1] < -1.0) {
                    if (++s.fall_n >= 3) s.airborne = true;   // (falling)
                } else {
                    s.fall_n = 0;
                }
                s.flat_n = 0;
            } else {
                bool down = false;
                if (acc > 4.0 && v[1] > -1.0) {
                    down = true;   // (the fall stopped)
                } else if (std::fabs(v[1]) < 0.5 && std::fabs(acc) < 0.5) {
                    down = ++s.flat_n >= 2;
                } else {
                    s.flat_n = 0;
                }
                if (down) {
                    s.airborne = false;
                    s.land = std::clamp(-before / 20.0, 0.0, 1.0);
                    s.fall_n = 0;
                    s.flat_n = 0;
                }
            }
            s.air += ((s.airborne ? 1.0 : 0.0) - s.air) * (s.airborne ? 0.4 : 0.55);
            if (s.air < 0.01) s.air = 0.0;
            s.land *= 0.72;
        }
        for (int i = 0; i < 3; i++) s.vel[i] += (v[i] - s.vel[i]) * 0.4;
        s.t += 1.0;
        // the game's animation, and the log of each player's as it changes
        if (anim != s.anim) {
            if (k < humans) {
                char line[420];
                std::snprintf(line, sizeof(line), "[anim] f=%u character %d: %04X -> %04X after %u frames; speed %.2f, up %.2f, weapon %02X, scoped %u, "
                    "look %d, life %02X; its record (+0x2D0) as it ended %.3f %.3f %.3f %.3f, now %.3f %.3f %.3f %.3f",
                    (unsigned)frames, k, (unsigned)s.anim, (unsigned)anim, (unsigned)s.anim_age, horiz, v[1], (unsigned)weapon, (unsigned)scoped,
                    (int)look, (unsigned)life, s.ctl_last[0], s.ctl_last[1], s.ctl_last[2], s.ctl_last[3], ctl_now[0], ctl_now[1], ctl_now[2],
                    ctl_now[3]);
                say(line);
            }
            s.anim = anim;
            s.anim_age = 0;
        } else {
            s.anim_age++;
            if (k < humans && (s.anim_age == 10 || s.anim_age == 20)) {
                char line[220];
                std::snprintf(line, sizeof(line), "[anim]   f=%u character %d: %04X %u frames on: its record %.3f %.3f %.3f %.3f; speed %.2f, up %.2f",
                    (unsigned)frames, k, (unsigned)anim, (unsigned)s.anim_age, ctl_now[0], ctl_now[1], ctl_now[2], ctl_now[3], horiz, v[1]);
                say(line);
            }
        }
        int32_t pickup = -1;
        uint32_t pickup_kind = 0;
        {
            const uint32_t block = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800D23B0u);
            if (kseg0(block)) {
                pickup = MEM_W(0xEB0 + 4 * (uint32_t)k, (gpr)(int32_t)block);
                if (pickup >= 0 && pickup < 64) pickup_kind = (uint32_t)MEM_W(0x1174 + 4 * (uint32_t)pickup, (gpr)(int32_t)block);
            }
        }
        if (pickup != s.pickup && k < humans) {
            char line[160];
            std::snprintf(line, sizeof(line), "[pickup] f=%u character %d: holds pickup %d (of kind %X), was %d; weapon out %02X (animation %04X)",
                (unsigned)frames, k, (int)pickup, (unsigned)pickup_kind, (int)s.pickup, (unsigned)weapon, (unsigned)anim);
            say(line);
        }
        s.pickup = pickup;
        if (weapon != s.weapon && k < humans) {
            char line[160];
            std::snprintf(line, sizeof(line), "[weapon] f=%u character %d: %02X -> %02X (animation %04X)", (unsigned)frames, k, (unsigned)s.weapon,
                (unsigned)weapon, (unsigned)anim);
            say(line);
        }
        std::memcpy(s.ctl_last, ctl_now, sizeof(ctl_now));
        s.anim_t = ctl_now[3] > 0.5f ? std::clamp((double)ctl_now[0] / (double)ctl_now[3], 0.0, 1.0) : 0.0;
        s.weapon = weapon;
        s.scoped = scoped;
        s.life = life;
        s.look = look;
        // how much of ours: the game's animation one of the walking ones (or, loco_any, any on the ground), not going up or down fast
        bool ours = (params.loco.count(anim) != 0 || (loco_any && std::fabs(s.vy) < 2.0)) && std::fabs(s.vy) < max_vy && life == 0;
        if (params.keep.count(anim) != 0) ours = false;
        s.w_loco += ((ours ? 1.0 : 0.0) - s.w_loco) * (ours ? blend_in : blend_out);
        if (s.w_loco < 0.01) s.w_loco = 0.0;
        if (s.w_loco > 0.99) s.w_loco = 1.0;
        for (int i = 0; i < 3; i++) s.pos[i] = p[i];
        s.have = true;
    }
}

namespace {
    // ---- the matrices ----
    struct Mat { double m[4][3]; };   // rows 0-2 the turn (with the scale), row 3 the place (the 4th column is the game's, left alone)
    Mat read_mat(uint8_t* rdram, uint32_t at) {
        Mat r{};
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 3; j++) {
                const uint32_t w = (uint32_t)MEM_W((int32_t)(i * 16 + j * 4), (gpr)(int32_t)at);
                float f;
                std::memcpy(&f, &w, 4);
                r.m[i][j] = f;
            }
        }
        return r;
    }
    void write_mat(uint8_t* rdram, uint32_t at, const Mat& r) {
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 3; j++) {
                const float f = (float)r.m[i][j];
                uint32_t w;
                std::memcpy(&w, &f, 4);
                MEM_W((int32_t)(i * 16 + j * 4), (gpr)(int32_t)at) = (int32_t)w;
            }
        }
    }
    inline M3 basis(const Mat& a) { M3 r{}; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.r[i][j] = a.m[i][j]; return r; }
    inline V3 place(const Mat& a) { return v3(a.m[3][0], a.m[3][1], a.m[3][2]); }
    constexpr uint8_t kPar[28] = { 255, 0, 1, 2, 3, 4, 4, 1, 7, 8, 9, 9, 1, 12, 12, 12, 0, 16, 17, 18, 0, 20, 21, 22, 0, 24, 25, 26 };

    std::array<std::pair<uint32_t, uint32_t>, 16> done{};
    size_t done_next = 0;
    uint32_t signature(uint8_t* rdram, uint32_t m) {
        uint32_t sig = 0;
        for (int w = 0; w < 12; w++) sig = sig * 31u + (uint32_t)MEM_W(16 * 0x40 + w * 4, (gpr)(int32_t)m);
        for (int w = 0; w < 12; w++) sig = sig * 31u + (uint32_t)MEM_W(9 * 0x40 + w * 4, (gpr)(int32_t)m);
        return sig;
    }

    // the knee (or elbow) for a hip at h reaching for `target` with bones a1 and a2, bending toward `pole`; *reached where the end gets
    V3 two_bone(V3 h, V3 target, double a1, double a2, V3 pole, V3* reached) {
        V3 d = target - h;
        double dist = len(d);
        if (dist < 1e-9) { d = v3(0, -1, 0); dist = 1e-9; }
        const V3 u = d * (1.0 / dist);
        const double lo = std::fabs(a1 - a2) + 1e-6, hi = (a1 + a2) * 0.99995;
        const double dc = std::clamp(dist, lo, hi);
        const double ca = std::clamp((a1 * a1 + dc * dc - a2 * a2) / (2 * a1 * dc), -1.0, 1.0);
        const double sa = std::sqrt(std::max(0.0, 1 - ca * ca));
        V3 v = pole - u * dot(pole, u);
        if (len(v) < 1e-9) v = cross(u, v3(1, 0, 0));
        v = unit(v);
        *reached = h + u * dc;
        return h + u * (a1 * ca) + v * (a1 * sa);
    }

    // one style's numbers (char_anim.txt's [tag], then [*], then these)
    struct Style {
        double start_speed, walk_speed, run_speed;          // leg lengths a frame: moving at all; fully walking; fully running
        double stride_base, stride_gain, stride_max;        // the stride (two steps), leg lengths: base + gain * speed, at most max
        double duty_walk, duty_run;                         // the part of the stride each foot is down
        double lift_walk, lift_run, lift_peak_walk, lift_peak_run, swing_lag;
        double reach_idle, reach_walk, reach_run, bounce_run, settle;
        double feet_apart, feet_apart_run, stagger, toe_out;
        double strike, push;
        double pelvis_yaw_walk, pelvis_yaw_run, pelvis_drop, sway, pelvis_tilt;
        double lean_walk, lean_run, lean_turn, lean_strafe;
        double chest_twist_walk, chest_twist_run, head_down, breathe;
        double arm_swing_walk, arm_swing_run, arm_back_run, arm_out_idle, arm_out_walk, arm_out_run;
        double elbow_idle, elbow_walk, elbow_run, elbow_pump, forearm_in_run;
        double tail_sway, tail_lift;
        double armed_upper;
        double hips_into, hips_into_max, ankle_rise, idle_hip, idle_sway;
        double sprint_speed, duty_sprint, lift_sprint, bounce_sprint, arm_swing_sprint, lean_sprint;
        double air_front, air_back, air_toes, air_arms_out, air_arms_up, land_dip;
        double sword_style, sword_stance, sword_stagger, sword_reach, sword_twist;
    };
    Style style_of(const std::string& tag) {
        const Params& P = params;
        Style s{};
        const auto g = [&](const char* key, double fallback) { return P.get(tag, key, fallback); };
        s.start_speed = g("start_speed", 0.006);
        s.walk_speed = g("walk_speed", 0.05);
        s.run_speed = g("run_speed", 0.13);
        s.stride_base = g("stride_base", 1.0);
        s.stride_gain = g("stride_gain", 7.5);
        s.stride_max = g("stride_max", 5.5);
        s.duty_walk = g("duty_walk", 0.62);
        s.duty_run = g("duty_run", 0.36);
        s.lift_walk = g("lift_walk", 0.10);
        s.lift_run = g("lift_run", 0.42);
        s.lift_peak_walk = g("lift_peak_walk", 0.30);
        s.lift_peak_run = g("lift_peak_run", 0.42);
        s.swing_lag = g("swing_lag", 0.6);
        s.reach_idle = g("reach_idle", 0.955);
        s.reach_walk = g("reach_walk", 0.975);
        s.reach_run = g("reach_run", 0.955);
        s.bounce_run = g("bounce_run", 0.035);
        s.settle = g("settle", 0.6);
        s.feet_apart = g("feet_apart", 1.3);
        s.feet_apart_run = g("feet_apart_run", 0.55);
        s.stagger = g("stagger", 0.07);
        s.toe_out = g("toe_out", 9) * deg;
        s.strike = g("strike", 16) * deg;
        s.push = g("push", 32) * deg;
        s.pelvis_yaw_walk = g("pelvis_yaw_walk", 6) * deg;
        s.pelvis_yaw_run = g("pelvis_yaw_run", 11) * deg;
        s.pelvis_drop = g("pelvis_drop", 4) * deg;
        s.sway = g("sway", 0.025);
        s.pelvis_tilt = g("pelvis_tilt", 4) * deg;
        s.lean_walk = g("lean_walk", 3) * deg;
        s.lean_run = g("lean_run", 10) * deg;
        s.lean_turn = g("lean_turn", 12) * deg;
        s.lean_strafe = g("lean_strafe", 4) * deg;
        s.chest_twist_walk = g("chest_twist_walk", 7) * deg;
        s.chest_twist_run = g("chest_twist_run", 14) * deg;
        s.head_down = g("head_down", 3) * deg;
        s.breathe = g("breathe", 1.2) * deg;
        s.arm_swing_walk = g("arm_swing_walk", 16) * deg;
        s.arm_swing_run = g("arm_swing_run", 40) * deg;
        s.arm_back_run = g("arm_back_run", -6) * deg;
        s.arm_out_idle = g("arm_out_idle", 9) * deg;
        s.arm_out_walk = g("arm_out_walk", 8) * deg;
        s.arm_out_run = g("arm_out_run", 13) * deg;
        s.elbow_idle = g("elbow_idle", 12) * deg;
        s.elbow_walk = g("elbow_walk", 16) * deg;
        s.elbow_run = g("elbow_run", 86) * deg;
        s.elbow_pump = g("elbow_pump", 20) * deg;
        s.forearm_in_run = g("forearm_in_run", 16) * deg;
        s.tail_sway = g("tail_sway", 14) * deg;
        s.tail_lift = g("tail_lift", 8) * deg;
        s.armed_upper = g("armed_upper", 1);
        s.hips_into = g("hips_into", 0.55);
        s.hips_into_max = g("hips_into_max", 50) * deg;
        s.ankle_rise = g("ankle_rise", 0.13);
        s.idle_hip = g("idle_hip", 2.5) * deg;
        s.idle_sway = g("idle_sway", 0.02);
        s.sprint_speed = g("sprint_speed", 0.45);
        s.duty_sprint = g("duty_sprint", 0.25);
        s.lift_sprint = g("lift_sprint", 0.55);
        s.bounce_sprint = g("bounce_sprint", 0.05);
        s.arm_swing_sprint = g("arm_swing_sprint", 50) * deg;
        s.lean_sprint = g("lean_sprint", 14) * deg;
        s.air_front = g("air_front", 0.22);
        s.air_back = g("air_back", 0.42);
        s.air_toes = g("air_toes", 15) * deg;
        s.air_arms_out = g("air_arms_out", 25) * deg;
        s.air_arms_up = g("air_arms_up", 20) * deg;
        s.land_dip = g("land_dip", 0.14);
        s.sword_style = g("sword_style", 0);       // 1: two hands on it (Zack), 2: one, the other out (Sabre)
        s.sword_stance = g("sword_stance", 1.6);   // its feet apart, times standing's
        s.sword_stagger = g("sword_stagger", 0.32); // its left foot ahead (and the right back), leg lengths
        s.sword_reach = g("sword_reach", 0.9);     // its knees bent (the legs' reach)
        s.sword_twist = g("sword_twist", 1.0);     // how far the body turns with a swing
        return s;
    }
    // the swing's lift: 0 at q 0 and 1, 1 at q = peak
    double lift_curve(double q, double peak) {
        q = std::clamp(q, 0.0, 1.0);
        peak = std::clamp(peak, 0.1, 0.9);
        const double a = std::log(0.5) / std::log(peak);
        return std::sin(pi * std::pow(q, a));
    }
}

namespace {
    // ---- the sword (the Anorium sword, the game's 0x21, on bone 9: its blade along the bone's +z, its edges along y, flat across x) ----
    // Each move a list of keys over the game's own animation's time (0 to 1, so a swing lands when the game's does): where the hands
    // grip it (from the middle of the shoulders, arm lengths, the body's ways), which way its blade points and its edge leads, how far
    // the chest turns (+ to its left) and leans, how far the hips drop into a lunge. A swing's keys give the blade's angle in its plane
    // (u its forward, v the way it cuts to): between two of them the blade goes round in that plane, its edge first.
    struct SwordKey {
        double t;
        double gx, gy, gz;       // the grip
        bool in_plane;           // the blade by its angle in the plane (else by dx, dy, dz and its edge ex, ey, ez)
        double theta;            // (degrees)
        double dx, dy, dz, ex, ey, ez;
        double twist, lean, drop;   // (degrees, degrees, leg lengths)
    };
    struct SwordMove {
        V3 u, v;                 // the plane: theta 0 along u, 90 along v
        std::vector<SwordKey> keys;
    };
    SwordKey hold(double t, double gx, double gy, double gz, V3 d, V3 e, double twist, double lean, double drop) {
        return { t, gx, gy, gz, false, 0, d.x, d.y, d.z, e.x, e.y, e.z, twist, lean, drop };
    }
    SwordKey cut(double t, double gx, double gy, double gz, double theta, double twist, double lean, double drop) {
        return { t, gx, gy, gz, true, theta, 0, 0, 0, 0, 0, 0, twist, lean, drop };
    }
    // (Zack, both hands: the Buster Sword's hold in front, low, pointing at the foe; its run with it up across the body; a diagonal cut
    // from over the right shoulder down to the left; on the run, a flat cut right to left)
    SwordMove zack_ready() {
        return { v3(0, 0, 1), v3(0.6, -0.8, 0), { hold(0, -0.12, -0.62, 0.42, v3(-0.18, 0.32, 0.93), v3(0, -1, 0.3), -8, 6, 0) } };
    }
    SwordMove zack_run() {
        return { v3(0, 0, 1), v3(0.6, -0.8, 0), { hold(0, -0.1, -0.55, 0.36, v3(0.45, 0.85, 0.25), v3(0.2, -0.2, 1), 0, 0, 0) } };
    }
    SwordMove zack_cut() {
        return { v3(0, 0, 1), unit(v3(0.8, -0.45, 0)), {
            cut(0.00, -0.12, -0.62, 0.42, -20, -8, 6, 0.00),
            cut(0.22, -0.40, 0.30, 0.05, -120, -35, -4, 0.02),
            cut(0.34, -0.10, -0.05, 0.62, -10, 5, 10, 0.07),
            cut(0.46, 0.30, -0.45, 0.45, 70, 30, 16, 0.11),
            cut(0.58, 0.38, -0.50, 0.28, 82, 35, 14, 0.10),
            cut(1.00, -0.12, -0.62, 0.42, -20, -8, 6, 0.00) } };
    }
    SwordMove zack_run_cut() {
        return { v3(0, 0, 1), unit(v3(1, -0.2, 0)), {
            cut(0.00, -0.15, -0.45, 0.40, -30, -10, 8, 0.00),
            cut(0.20, -0.50, -0.10, 0.00, -120, -45, 4, 0.03),
            cut(0.34, 0.00, -0.20, 0.62, 0, 0, 12, 0.08),
            cut(0.46, 0.40, -0.30, 0.35, 95, 40, 14, 0.10),
            cut(0.60, 0.45, -0.35, 0.20, 110, 45, 12, 0.08),
            cut(1.00, -0.15, -0.45, 0.40, -30, -10, 8, 0.00) } };
    }
    // (Sabre, one hand: the sword out in front at the foe, crouched; on the run held low, pointing back; a flat cut right to left; on
    // the run a rising cut)
    SwordMove sabre_ready() {
        return { v3(0, 0, 1), v3(1, -0.15, 0), { hold(0, -0.30, -0.50, 0.55, v3(0.05, 0.20, 1.0), v3(0, -1, 0.2), -12, 10, 0) } };
    }
    SwordMove sabre_run() {
        return { v3(0, 0, 1), v3(1, -0.15, 0), { hold(0, -0.32, -0.90, -0.05, v3(-0.08, -0.22, -0.97), v3(0, -1, 0.2), 0, 0, 0) } };
    }
    SwordMove sabre_cut() {
        return { v3(0, 0, 1), unit(v3(1, -0.15, 0)), {
            cut(0.00, -0.30, -0.50, 0.55, -10, -12, 10, 0.00),
            cut(0.22, -0.60, -0.30, 0.00, -110, -40, 4, 0.03),
            cut(0.36, -0.10, -0.35, 0.70, 0, 0, 14, 0.09),
            cut(0.48, 0.35, -0.40, 0.45, 100, 40, 16, 0.11),
            cut(0.60, 0.40, -0.42, 0.30, 115, 45, 12, 0.09),
            cut(1.00, -0.30, -0.50, 0.55, -10, -12, 10, 0.00) } };
    }
    SwordMove sabre_run_cut() {
        return { v3(0, 0, 1), unit(v3(0.55, 0.85, 0)), {
            cut(0.00, -0.32, -0.80, 0.10, -150, -10, 6, 0.00),
            cut(0.20, -0.45, -0.85, -0.05, -150, -30, 10, 0.05),
            cut(0.34, -0.05, -0.30, 0.65, 0, 5, 6, 0.06),
            cut(0.46, 0.30, 0.25, 0.40, 80, 35, -4, 0.02),
            cut(0.60, 0.35, 0.30, 0.25, 95, 40, -4, 0.02),
            cut(1.00, -0.32, -0.80, 0.10, -150, -10, 6, 0.00) } };
    }
    // the blade's way and its edge's at a key
    void key_blade(const SwordMove& mv, const SwordKey& k, V3* d, V3* e) {
        if (k.in_plane) {
            const double th = k.theta * deg;
            *d = unit(mv.u * std::cos(th) + mv.v * std::sin(th));
            *e = unit(mv.u * -std::sin(th) + mv.v * std::cos(th));
        } else {
            *d = unit(v3(k.dx, k.dy, k.dz));
            *e = unit(v3(k.ex, k.ey, k.ez));
        }
    }
    // the hand's turn for a blade pointing d, its edge toward e (local x the flat, y the edge, z the blade)
    M3 blade_turn(V3 d, V3 e) {
        d = unit(d);
        e = e - d * dot(d, e);
        if (len(e) < 1e-6) e = std::fabs(d.y) < 0.9 ? cross(d, v3(0, 1, 0)) : cross(d, v3(1, 0, 0));
        e = unit(e);
        return rows(cross(e, d), e, d);
    }
    struct SwordNow { V3 grip, d, e; M3 hand; double twist, lean, drop; };
    // the move at time t: between its keys, eased; a cut's blade round its plane, else turned the shortest way; the grip on an arc
    SwordNow sword_at(const SwordMove& mv, double t) {
        const auto& ks = mv.keys;
        size_t i = 0;
        while (i + 1 < ks.size() && ks[i + 1].t <= t) i++;
        const SwordKey& a = ks[i];
        const SwordKey& b = ks[std::min(i + 1, ks.size() - 1)];
        const double span = b.t - a.t;
        const double f = span > 1e-6 ? smooth01((t - a.t) / span) : 0.0;
        SwordNow n{};
        V3 da, ea, db, eb;
        key_blade(mv, a, &da, &ea);
        key_blade(mv, b, &db, &eb);
        if (a.in_plane && b.in_plane) {
            SwordKey m = a;
            m.theta = lerp(a.theta, b.theta, f);
            key_blade(mv, m, &n.d, &n.e);
            n.hand = blade_turn(n.d, n.e);
        } else {
            n.hand = blend(blade_turn(da, ea), blade_turn(db, eb), f);
            n.d = row(n.hand, 2);
            n.e = row(n.hand, 1);
        }
        const V3 ga = v3(a.gx, a.gy, a.gz), gb = v3(b.gx, b.gy, b.gz);
        const double la = len(ga), lb = len(gb);
        V3 g = ga + (gb - ga) * f;
        if (la > 1e-6 && lb > 1e-6) g = unit(g) * lerp(la, lb, f);   // (round, not straight through the body)
        n.grip = g;
        n.twist = lerp(a.twist, b.twist, f) * deg;
        n.lean = lerp(a.lean, b.lean, f) * deg;
        n.drop = lerp(a.drop, b.drop, f);
        return n;
    }

    void capture(int k, uint32_t frame, uint16_t anim, double w, const V3& obj, bool ours, const Mat* game, const Mat* drawn) {
        if (!capture_on || capture_file == nullptr || k != 0 || capture_count >= 3600) return;
        capture_count++;
        capture_put_u32(0x414E494Du);
        capture_put_u32(frame);
        capture_put_u32((uint32_t)k);
        capture_put_u32(anim);
        capture_put_f(w);
        capture_put_f(obj.x);
        capture_put_f(obj.y);
        capture_put_f(obj.z);
        capture_put_u32(ours ? 1u : 0u);
        for (const Mat* set : { game, drawn }) {
            for (int b = 0; b < 28; b++) for (int i = 0; i < 4; i++) for (int j = 0; j < 3; j++) capture_put_f(set[b].m[i][j]);
        }
    }
}

namespace {
    // ---- (0.11.5) Banjo: bk_banjo.cpp's pose (Banjo-Kazooie's animations), put where the character is and turned its way; with a weapon
    // out (or aiming) his arms are the game's, aiming it, from his own shoulders ----
    int banjo_told = 0;
    // (Banjo's facing) the character's own facing (+0x7A, a 16-bit turn) against the way the game's root faces while it stands or runs:
    // which way round the one goes and how far it is from the other, learned as it plays (the circular mean of the difference, for each
    // way round: the right one stays put as it turns). It starts as the game has it (the root's way a quarter turn on from +0x7A, as the
    // logs showed), so it's known from the first frame
    struct FacingFit { double c[2] = { 0.0, 0.0 }, s[2] = { 30.0, 0.0 }, w = 30.0; } facing_fit;
    double wrap_pi(double a) { while (a > pi) a -= 2 * pi; while (a < -pi) a += 2 * pi; return a; }
    void banjo_say(int k, uint32_t m, const char* what, double sc) {
        if (banjo_told >= 600) return;
        banjo_told++;
        char line[640];
        std::snprintf(line, sizeof(line), "[banjo] f=%u character %d (matrices %08X, scale %.3f): %s", (unsigned)frames, k, (unsigned)m, sc, what);
        say(line);
    }
    bool banjo_pose(uint8_t* rdram, int k, uint32_t m) {
        const uint32_t sig_now = signature(rdram, m);
        for (const auto& d : done) {
            if (d.first == m && d.second == sig_now) return true;   // (drawn again from the same matrices: posed already)
        }
        Mat M[28];
        for (int b = 0; b < 28; b++) M[b] = read_mat(rdram, m + (uint32_t)b * 0x40u);
        const M3 A0 = basis(M[0]);
        const double sc = len(row(A0, 0));
        if (!(sc > 1e-6) || !std::isfinite(sc)) { banjo_say(k, m, "no scale", sc); return false; }
        V3 xh = row(A0, 0);
        xh.y = 0;
        if (len(xh) < 1e-9) { banjo_say(k, m, "no facing", sc); return false; }
        xh = unit(xh);
        const V3 up = v3(0, 1, 0);
        static State none;
        State& s = (k >= 0 && k < max_chars) ? st[k] : none;   // (the select screen draws its characters as 17 and on)
        const bool match = s.have && MEM_BU(0, (gpr)(int32_t)0x800BE616) != 0;
        const double F = conker::bk_banjo::model_scale();
        const uint32_t o = characters + (uint32_t)std::clamp(k, 0, 63) * character_size;
        // his facing: the character's own, +0x7A, turned to the game's root's as the two agree while it stands or runs (Conker's own
        // animations turn his root elsewhere: his jumps spin it, his stops swing it round, his falls and deaths tip it over); the root's
        // way only while that's not known. Drawn turning toward it at up to Banjo-Kazooie's turning speed (yaw.c: 700 degrees a second;
        // Conker's turn-round snaps half a turn)
        const V3 fwd_root = cross(xh, up);
        const double yaw_root = std::atan2(fwd_root.x, fwd_root.z);
        const M3 R0t = transp(rows(xh, up, fwd_root));   // (from the world to the way the game's root faces)
        const uint16_t face16 = (uint16_t)MEM_HU(0x7A, (gpr)(int32_t)o);
        const double face = face16 * (2 * pi / 65536.0);
        const bool steady = (s.anim == 0x000F || s.anim == 0x0002) && s.anim_age > 8;
        double yaw = yaw_root;
        if (match) {
            if (s.banjo_frame != frames && steady && s.life == 0) {
                FacingFit& ff = facing_fit;
                for (int w = 0; w < 2; w++) {
                    const double phi = yaw_root - (w == 0 ? face : -face);
                    ff.c[w] = ff.c[w] * 0.995 + std::cos(phi);
                    ff.s[w] = ff.s[w] * 0.995 + std::sin(phi);
                }
                ff.w = ff.w * 0.995 + 1.0;
            }
            const FacingFit& ff = facing_fit;
            const double r0 = std::hypot(ff.c[0], ff.s[0]) / std::max(1e-9, ff.w), r1 = std::hypot(ff.c[1], ff.s[1]) / std::max(1e-9, ff.w);
            const int way = r0 >= r1 ? 0 : 1;
            const bool fitted = ff.w > 20.0 && std::max(r0, r1) > 0.9;
            const double target = fitted ? wrap_pi((way == 0 ? face : -face) + std::atan2(ff.s[way], ff.c[way])) : yaw_root;
            const double px = rf(rdram, o + 0x14), pz = rf(rdram, o + 0x1C);
            const bool moved_far = std::hypot(px - s.banjo_x, pz - s.banjo_z) > 150.0;   // (started again elsewhere)
            s.banjo_x = px;
            s.banjo_z = pz;
            if (!s.banjo_have_yaw || s.anim == 0x0000 || moved_far) {
                s.banjo_yaw = target;
                s.banjo_have_yaw = true;
            } else if (s.banjo_frame != frames) {
                const double d = wrap_pi(target - s.banjo_yaw);
                const double most = 700.0 / 30.0 * deg * std::max(1u, frames - s.banjo_frame);
                s.banjo_yaw = wrap_pi(s.banjo_yaw + std::clamp(d, -most, most));
            }
            yaw = s.banjo_yaw;
        }
        const V3 fwd = v3(std::sin(yaw), 0, std::cos(yaw));
        xh = cross(up, fwd);
        const M3 R1 = rows(xh, up, fwd);
        conker::bk_banjo::Input in;
        in.have = match;
        in.frame = frames;
        in.speed_bk = s.speed * 30.0 / (sc * F);
        in.vy = s.vy;
        in.y = rf(rdram, o + 0x18);
        in.to_bk = 1.0 / (sc * F);
        in.airborne = s.airborne;
        in.game_anim = s.anim;
        in.life = s.life;
        in.skid = params.get("banjo", "skid", 0.0) > 0.5;
        conker::bk_banjo::SlotXf X[28];
        int eye = 14;
        if (!conker::bk_banjo::pose(k, in, X, &eye)) { banjo_say(k, m, "no pose", sc); return false; }
        // where his feet stand: the character's place in a match; else under the game's root, on the floor its toes stand on
        const double floor_off = params.get("banjo", "floor", 0.0);
        V3 base;
        if (match) {
            base = v3(rf(rdram, o + 0x14), in.y + floor_off * sc, rf(rdram, o + 0x1C));
        } else {
            const V3 r0 = place(M[0]);
            base = v3(r0.x, std::min(place(M[19]).y, place(M[23]).y) - 8.6 * sc + floor_off * sc, r0.z);
        }
        M3 rw[28];
        V3 pw[28];
        for (int b = 0; b < 28; b++) {
            M3 rc{};
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) rc.r[i][j] = X[b].r[j][i];
            rw[b] = mul(rc, R1);
            pw[b] = base + mulv(v3(X[b].p[0], X[b].p[1], X[b].p[2]), R1) * sc;
        }
        // a weapon in his hands: his arms as the game aims it (its turns, from the way its root faces to the way he does; his own arms'
        // lengths from his shoulders), blended in and out
        const bool armed = match && (s.weapon != 0 || s.scoped != 0 || s.look != 0);
        if (s.banjo_frame != frames) {
            s.banjo_frame = frames;
            s.banjo_arm += ((armed ? 1.0 : 0.0) - s.banjo_arm) * (armed ? 0.35 : 0.25);
            if (s.banjo_arm < 0.01) s.banjo_arm = 0.0;
            if (s.banjo_arm > 0.99) s.banjo_arm = 1.0;
        }
        if (s.banjo_arm > 0.0) {
            for (const int a : { 2, 7 }) {
                for (int j = 0; j < 3; j++) {
                    const int b = a + j;
                    M3 g = basis(M[b]);
                    for (auto& r : g.r) for (double& v : r) v /= sc;
                    rw[b] = blend(rw[b], mul(mul(orthonormal(g), R0t), R1), s.banjo_arm);
                    if (j > 0) {
                        double bv[3];
                        conker::bk_banjo::bind_vector(b - 1, b, bv);
                        pw[b] = pw[b - 1] + mulv(v3(bv[0], bv[1], bv[2]), rw[b - 1]) * sc;
                    }
                }
            }
        }
        Mat R[28];
        for (int b = 0; b < 28; b++) {
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[b].m[i][j] = rw[b].r[i][j] * sc;
            R[b].m[3][0] = pw[b].x; R[b].m[3][1] = pw[b].y; R[b].m[3][2] = pw[b].z;
            for (int i = 0; i < 4; i++) for (int j = 0; j < 3; j++) if (!std::isfinite(R[b].m[i][j])) { banjo_say(k, m, "not finite", sc); return false; }
        }
        for (int b = 0; b < 28; b++) write_mat(rdram, m + (uint32_t)b * 0x40u, R[b]);
        if (banjo_told < 40 || (match && k == 0 && frames % 15 == 0 && banjo_told < 600)) {
            char what[320];
            std::snprintf(what, sizeof(what), "posed (match %d, animation %04X, his base %.1f %.1f %.1f, the game's root %.1f %.1f %.1f; facing +0x7A %04X, "
                "the root's way %.1f) %s", match ? 1 : 0, (unsigned)s.anim, base.x, base.y, base.z, place(M[0]).x, place(M[0]).y, place(M[0]).z,
                (unsigned)(uint16_t)MEM_HU(0x7A, (gpr)(int32_t)o), std::atan2(fwd.x, fwd.z) / deg, conker::bk_banjo::state_of(k).c_str());
            banjo_say(k, m, what, sc);
        }
        capture(k, frames, s.anim, 1.0, base, true, M, R);
        done[done_next] = { m, signature(rdram, m) };
        done_next = (done_next + 1) % done.size();
        return true;
    }
}

bool conker::char_anim::is_banjo(int k) {
    return k >= 0 && k < max_chars && st[k].banjo_frame != 0xFFFFFFFFu && frames - st[k].banjo_frame < 60u;
}

// ---- as a character is drawn ----
bool conker::char_anim::pose(uint8_t* rdram, int k, const char* tag_c, uint32_t m) {
    if (k < 0 || tag_c == nullptr) return false;
    if (!params.loaded) load_params(true);
    const std::string tag(tag_c);
    if (tag == "banjo" && params.get(tag, "enable", 1.0) > 0.5) {
        if (conker::bk_banjo::ready()) return banjo_pose(rdram, k, m);
        banjo_say(k, m, "assets/banjo/bk_anims.bin not read", 0.0);
    }
    if (k >= max_chars) return false;
    if (params.get(tag, "enable", 0) < 0.5) return false;
    State& s = st[k];
    if (!s.have) return false;
    const uint32_t sig_now = signature(rdram, m);
    for (const auto& d : done) {
        if (d.first == m && d.second == sig_now) return true;   // (drawn again from the same matrices: posed already)
    }
    Mat M[28];
    for (int b = 0; b < 28; b++) M[b] = read_mat(rdram, m + (uint32_t)b * 0x40u);
    const M3 A0 = basis(M[0]);
    const double sc = len(row(A0, 0));
    if (!(sc > 1e-6) || !std::isfinite(sc)) return false;
    // the body's facing: the root's x way, level (the game's lean and roll left out)
    V3 xh = row(A0, 0);
    xh.y = 0;
    if (len(xh) < 1e-9) return false;
    xh = unit(xh);
    const V3 up = v3(0, 1, 0), fwd = cross(xh, up);
    const M3 R1 = rows(xh, up, fwd);
    // each bone's place from its parent's, in its parent's own units and ways (the skeleton, as the game made it)
    V3 off[28];
    M3 gw[28];   // the game's turns in the world, scale off
    for (int b = 0; b < 28; b++) {
        M3 a = basis(M[b]);
        for (auto& r : a.r) for (double& v : r) v /= sc;
        gw[b] = orthonormal(a);
        const int p = kPar[b];
        if (p == 255) {
            off[b] = v3(0, 0, 0);
            continue;
        }
        M3 pinv;
        if (!inverse(basis(M[p]), pinv)) return false;
        off[b] = mulv(place(M[b]) - place(M[p]), pinv);
    }
    const double thigh[2] = { len(off[17]), len(off[21]) }, shin[2] = { len(off[18]), len(off[22]) };
    const double L = 0.5 * (thigh[0] + shin[0] + thigh[1] + shin[1]);   // (model units)
    if (L < 1e-3) return false;
    const double Lw = L * sc;   // (world units)
    const Style S = style_of(tag);
    const uint32_t o = characters + (uint32_t)k * character_size;
    const double obj_y = rf(rdram, o + 0x18);
    const V3 obj = v3(rf(rdram, o + 0x14), obj_y, rf(rdram, o + 0x1C));
    const bool new_frame = s.frame_drawn != frames;
    s.frame_drawn = frames;

    // ---- this frame's motion (once a frame) ----
    const double sp = s.speed / Lw;   // leg lengths a frame
    const double move = smooth01((sp - S.start_speed) / std::max(1e-6, S.walk_speed - S.start_speed));
    const double run = smooth01((sp - S.walk_speed) / std::max(1e-6, S.run_speed - S.walk_speed));
    const double sprint = smooth01((sp - S.run_speed) / std::max(1e-6, S.sprint_speed - S.run_speed));
    const double duty = lerp(lerp(S.duty_walk, S.duty_run, run), S.duty_sprint, sprint);
    const double air = s.air;
    // the way it goes, against its facing: 0 forward, +90 to its left (backward: the legs' plane as forward's, the cycle run back)
    double psi = 0;
    {
        const V3 hv = v3(s.vel[0], 0, s.vel[2]);
        if (len(hv) > 0.02 * Lw) psi = std::atan2(dot(hv, xh), dot(hv, fwd));
    }
    if (s.back > 0.5) psi = psi > 0 ? psi - pi : psi + pi;
    psi = std::clamp(psi, -85 * deg, 85 * deg);
    // (going to the side: the hips turned into it, the legs running that way, the chest still facing ahead; the feet never cross)
    const double hip_turn = std::clamp(psi * S.hips_into, -S.hips_into_max, S.hips_into_max) * move;
    const double stride = std::min(S.stride_max, S.stride_base + S.stride_gain * sp) * L;   // (model units)
    if (new_frame) {
        const double yaw_now = std::atan2(fwd.x, fwd.z);
        if (s.have_yaw) {
            double dyaw = yaw_now - s.last_yaw;
            while (dyaw > pi) dyaw -= 2 * pi;
            while (dyaw < -pi) dyaw += 2 * pi;
            s.turn += (dyaw - s.turn) * 0.3;
        }
        s.last_yaw = yaw_now;
        s.have_yaw = true;
        // the stride's cycle on by the ground covered (backward: back)
        double dist = 0;
        V3 hv = v3(0, 0, 0);
        if (s.have_obj) {
            hv = v3(obj.x - s.last_obj.x, 0, obj.z - s.last_obj.z);
            dist = len(hv);
            if (dist > 400) dist = 0;
        }
        s.last_obj = obj;
        s.have_obj = true;
        const double ahead = dot(hv, fwd), side = dot(hv, xh);
        const bool backward = ahead < -0.35 * std::fabs(side) && dist > 0.01 * Lw;
        if (dist > 0.005 * Lw) s.back += ((backward ? 1.0 : 0.0) - s.back) * 0.3;
        s.phase = wrap01(s.phase + (s.back > 0.5 ? -1.0 : 1.0) * dist / (stride * sc) * (1.0 - 0.85 * air));
        // where the feet stand: the game's lowest foot joint, while still and on the ground, from the character's own height (+0x18)
        double game_low = 1e30;
        for (int b : { 18, 19, 22, 23 }) game_low = std::min(game_low, place(M[b]).y);
        const bool still = s.speed < 0.01 * Lw && std::fabs(s.vy) < 0.3;
        if (!s.have_ground) {
            s.ground_c = game_low - obj_y;
            s.have_ground = true;
        } else if (still) {
            s.ground_c += (game_low - obj_y - s.ground_c) * 0.1;
        }
        const V3 game_root_off = place(M[0]) - obj;
        if (!s.have_root_off) {
            s.root_off = game_root_off;
            s.have_root_off = true;
        } else {
            s.root_off = s.root_off + (game_root_off - s.root_off) * 0.05;
        }
    }
    const double w = s.w_loco;
    if (w <= 0.0) {
        capture(k, frames, s.anim, 0.0, obj, false, M, M);
        return false;   // (the game's: biped_pose)
    }

    // ---- ours: in the body's ways (x its left, y up, z forward), model units, from the ground under it ----
    const double ph = s.phase;
    const V3 stepway = v3(std::sin(psi), 0, std::cos(psi));
    const double strafe = std::sin(psi) * move;
    const double back_k = s.back > 0.5 ? -0.5 : 1.0;   // (stepping back: the lean the other way, less)
    const M3 hips_way = yaw(hip_turn);

    // the pelvis: turned with the legs (the forward leg's hip ahead), dropped on the swinging leg's side, leaned into the run and turns
    const double cyc = std::cos(2 * pi * ph);   // (+1: the left leg furthest forward)
    const double stance_side = std::sin(2 * pi * (ph - duty * 0.5 + 0.25));   // (+1: over the left foot)
    const double pelvis_yaw = -lerp(S.pelvis_yaw_walk, S.pelvis_yaw_run, run) * move * cyc;
    const double pelvis_roll = S.pelvis_drop * move * (1 - 0.6 * run) * stance_side;
    const double turn_lean = std::clamp(-s.turn * 8.0 * std::min(1.0, sp / std::max(1e-6, S.run_speed)), -1.0, 1.0) * S.lean_turn;
    const double side_lean = turn_lean - strafe * S.lean_strafe;
    const double lean = (lerp(lerp(S.lean_walk, S.lean_run, run), S.lean_sprint, sprint) * move * back_k * std::cos(psi)) * (1 - 0.5 * air);
    const double breathe = std::sin(s.t * 2 * pi / 110.0) * S.breathe * (1 - move);
    // the sword out (the game's 0x21 in its hands) with a style of its own: its moves ours (0122 ready, 0124 running with it, 012A a
    // cut, 0129 a cut on the run), blended in and out
    const int sword_style = (int)S.sword_style;
    int sword_move = 0;   // 1 ready, 2 running, 3 a cut, 4 a cut on the run
    if (sword_style > 0 && s.weapon == 0x21) {
        if (s.anim == 0x122) sword_move = 1;
        else if (s.anim == 0x124) sword_move = 2;
        else if (s.anim == 0x12A) sword_move = 3;
        else if (s.anim == 0x129) sword_move = 4;
    }
    if (new_frame) {
        s.sword_w += ((sword_move != 0 ? 1.0 : 0.0) - s.sword_w) * (sword_move != 0 ? 0.3 : 0.4);
        if (s.sword_w < 0.01) s.sword_w = 0.0;
        if (s.sword_w > 0.99) s.sword_w = 1.0;
    }
    SwordNow sw{};
    double stance = 0;   // (the fighting stance: feet wide and staggered, knees bent)
    if (sword_move != 0) {
        SwordMove mv;
        if (sword_style == 1) mv = sword_move == 1 ? zack_ready() : sword_move == 2 ? zack_run() : sword_move == 3 ? zack_cut() : zack_run_cut();
        else mv = sword_move == 1 ? sabre_ready() : sword_move == 2 ? sabre_run() : sword_move == 3 ? sabre_cut() : sabre_run_cut();
        sw = sword_at(mv, sword_move >= 3 ? s.anim_t : 0.0);
        stance = (sword_move == 2 ? 0.0 : 1.0) * (1 - move);
    }
    const double sw_w = s.sword_w;
    const double idle_shift = std::sin(s.t * 2 * pi / 300.0) * (1 - move);   // (standing: the weight from one foot to the other, slowly)
    M3 Q[28];
    for (M3& q : Q) q = ident();
    const double sw_twist = sw.twist * S.sword_twist * sw_w, sw_lean = sw.lean * sw_w;
    Q[0] = mul(mul(roll(pelvis_roll + side_lean * 0.5 + idle_shift * S.idle_hip), pitch(S.pelvis_tilt * move + lean * 0.35 + sw_lean * 0.3)),
        yaw(pelvis_yaw + hip_turn + sw_twist * 0.45));
    // the chest: the rest of the lean, its roll back against the pelvis's drop (the shoulders level), turned against the hips
    const double chest_yaw = lerp(S.chest_twist_walk, S.chest_twist_run, run) * move * cyc;
    Q[1] = mul(mul(roll(-pelvis_roll * 0.9 + side_lean * 0.5 - idle_shift * S.idle_hip * 0.8), pitch(lean + breathe + sw_lean)),
        yaw(chest_yaw + hip_turn * 0.15 + sw_twist));
    // the head: level, looking ahead (turned a little with the chest)
    Q[12] = mul(pitch(S.head_down - lean * 0.6 - breathe * 0.5 - sw_lean * 0.7), yaw(chest_yaw * 0.2 + sw_twist * 0.2));
    Q[13] = Q[14] = Q[15] = Q[12];

    // the root: over the ground point (where the game's root is, on the whole), swayed over the foot that's down
    const double sway_x = S.sway * L * move * (1 - 0.7 * run) * stance_side + S.idle_sway * L * idle_shift;
    V3 rootp = v3(sway_x, 0, 0);
    const V3 hip_rel[2] = { mulv(off[16], Q[0]), mulv(off[20], Q[0]) };
    const double hip_half = std::max(0.5 * std::fabs(off[16].x - off[20].x), 0.05 * L);

    // each foot: where its ankle would be with the foot flat (down for the stance, lifted on the swing), its pitch (+ toes down)
    struct Foot { V3 flat; double pitch; bool down; } feet[2];
    const double lift_h = lerp(lerp(S.lift_walk, S.lift_run, run), S.lift_sprint, sprint) * L * move;
    const double lift_peak = lerp(S.lift_peak_walk, S.lift_peak_run, run);
    const double s_len = stride * duty * move;   // (how far the foot goes back under the body while it's down)
    const double apart = hip_half * lerp(S.feet_apart, S.feet_apart_run, run * move) * (1 + (S.sword_stance - 1) * stance * sw_w);
    for (int side = 0; side < 2; side++) {
        const int a = side == 0 ? 18 : 22;
        const double sgn = side == 0 ? 1.0 : -1.0;
        const double ankle_h = std::max(0.0, -off[a + 1].y);   // (the ankle over the ground, the foot flat: the toe's the lowest)
        // (a long foot tips less: its ankle rises no more than a little)
        const double foot_len = std::max(1e-3, len(off[a + 1]));
        const double push = std::min(S.push, std::asin(std::clamp(S.ankle_rise * L / foot_len, 0.0, 1.0)));
        const double strike = std::min(S.strike, std::asin(std::clamp(S.ankle_rise * L / foot_len, 0.0, 1.0)));
        const double p = wrap01(ph + (side == 0 ? 0.0 : 0.5));
        double travel, h = 0, fp;
        bool down;
        if (p < duty) {
            const double u = p / duty;   // (0 put down .. 1 lifted)
            travel = s_len * (0.5 - u);
            down = true;
            // heel strike: toes up, coming down flat; heel off toward the end: up on the toes
            fp = -strike * std::max(0.0, 1 - u / 0.18) + push * smooth01((u - 0.62) / 0.38);
        } else {
            const double q = (p - duty) / (1 - duty);   // (0 lifted .. 1 put down)
            const double qe = std::pow(q, 1.0 + S.swing_lag * run);
            travel = -s_len * 0.5 + s_len * (0.5 - 0.5 * std::cos(pi * qe));
            h = lift_h * lift_curve(q, lift_peak);
            down = false;
            // toes down after lifting, up again before putting it down
            fp = push * (1 - smooth01(q / 0.45)) - strike * smooth01((q - 0.55) / 0.45);
        }
        fp *= move;
        if (air > 0) {
            // (in the air: both feet up, the one behind higher, toes down, coming in under the body)
            const bool behind = travel < 0;
            travel *= 1 - 0.35 * air;
            h = lerp(h, L * (behind ? S.air_back : S.air_front), air);
            fp = lerp(fp, S.air_toes, air);
        }
        // standing: one foot a little ahead (and the weight on one, then the other)
        const double stand_z = lerp(sgn * S.stagger, side == 0 ? S.sword_stagger : -S.sword_stagger * 0.85, stance * sw_w) * L * (1 - move);
        V3 flat = mulv(v3(sgn * apart, 0, stand_z), hips_way) + stepway * travel;
        flat.y = h + ankle_h;
        feet[side] = { flat, fp, (down || move < 0.5) && air < 0.5 };
    }
    // the ankle for each foot: up on the toe (toes down) or back on the heel (toes up), from the foot's bones; a lifted foot's toe and
    // heel kept off the ground
    V3 ankle[2];
    M3 footQ[2];
    for (int side = 0; side < 2; side++) {
        const int a = side == 0 ? 18 : 22;
        const double sgn = side == 0 ? 1.0 : -1.0;
        const V3 toe_off = off[a + 1];                                   // ankle to toe, as built (the model's ways)
        const double ankle_h = std::max(0.0, -toe_off.y);
        const V3 heel_off = v3(0, -ankle_h, 0);                          // (the heel: under the ankle, on the ground)
        const M3 turned = yaw(sgn * S.toe_out + hip_turn);
        footQ[side] = mul(pitch(feet[side].pitch), turned);
        if (feet[side].pitch >= 0) {
            const V3 toe = feet[side].flat + mulv(toe_off, turned);
            ankle[side] = toe - mulv(toe_off, footQ[side]);
        } else {
            const V3 heel = feet[side].flat + heel_off;
            ankle[side] = heel - mulv(heel_off, footQ[side]);
        }
        const double toe_y = ankle[side].y + mulv(toe_off, footQ[side]).y;
        const double heel_y = ankle[side].y + mulv(heel_off, footQ[side]).y;
        const double lowest = std::min(toe_y, heel_y);
        if (lowest < 0) ankle[side].y -= lowest;
    }
    // the root's height: as high as the feet that are down let it (their legs nearly straight); a run bouncing (lowest as a foot takes
    // the weight, highest between steps), never higher than the feet down can reach
    const double reach = lerp(lerp(lerp(S.reach_idle, S.sword_reach, stance * sw_w), S.reach_walk, move), S.reach_run, run);
    double y_soft = 1e30, y_hard = 1e30, y_any = 1e30;
    for (int side = 0; side < 2; side++) {
        const double Lleg = thigh[side] + shin[side];
        const V3 hr = hip_rel[side] + rootp;
        const V3 d = ankle[side] - hr;
        const double horiz2 = d.x * d.x + d.z * d.z;
        const auto height = [&](double r) { return ankle[side].y - hip_rel[side].y + std::sqrt(std::max(r * r - horiz2, 0.05 * r * r)); };
        y_any = std::min(y_any, height(Lleg * reach));
        if (feet[side].down) {
            y_soft = std::min(y_soft, height(Lleg * reach));
            y_hard = std::min(y_hard, height(Lleg * 0.998));
        }
    }
    double ry;
    {
        const double y_walk = y_soft < 1e29 ? y_soft : y_any;
        const double Lleg = L;
        const double land = std::sqrt(std::max(0.2 * Lleg * Lleg, std::pow(Lleg * reach, 2) - std::pow(s_len * 0.5, 2)));
        const double ankle_up = 0.5 * (std::max(0.0, -off[19].y) + std::max(0.0, -off[23].y));
        const double bounce = -std::cos(2 * pi * 2.0 * (ph - duty * 0.5));   // (-1 a foot's stance's middle, +1 between)
        const double bounce_k = lerp(S.bounce_run, S.bounce_sprint, sprint);
        const double y_run = ankle_up - 0.5 * (hip_rel[0].y + hip_rel[1].y) + land + bounce_k * L * (bounce - 0.4);
        ry = lerp(y_walk, y_run, run * move);
        // (in the air: as high as it stands, a little lower, the legs drawn up)
        const double y_stand = ankle_up - 0.5 * (hip_rel[0].y + hip_rel[1].y) + Lleg * S.reach_walk - 0.04 * L;
        ry = lerp(ry, y_stand, air);
    }
    if (new_frame || !s.have_root_y) {
        if (!s.have_root_y) {
            s.root_y = ry;
            s.have_root_y = true;
        } else {
            s.root_y += (ry - s.root_y) * S.settle;
        }
        if (y_hard < 1e29) s.root_y = std::min(s.root_y, y_hard);
    }
    rootp.y = s.root_y - s.land * S.land_dip * L - sw.drop * L * sw_w;   // (a landing: down on bent knees, coming back up; a lunge)
    // the legs: hip, knee (bending forward, over the toes), ankle; the foot as set
    V3 rel[28];
    rel[0] = rootp;
    for (int side = 0; side < 2; side++) {
        const int hb = side == 0 ? 16 : 20;
        const double sgn = side == 0 ? 1.0 : -1.0;
        const V3 H = rootp + hip_rel[side];
        V3 reached;
        const V3 pole = mulv(v3(sgn * std::sin(S.toe_out), 0.15, std::cos(S.toe_out)), hips_way);
        const V3 K = two_bone(H, ankle[side], thigh[side], shin[side], pole, &reached);
        // the thigh: the least turn from as built to hip-to-knee; the shin: on from the thigh's, the least more to knee-to-ankle
        Q[hb] = align(off[hb + 1], K - H);
        Q[hb + 1] = mul(Q[hb], align(mulv(off[hb + 2], Q[hb]), reached - K));
        Q[hb + 2] = footQ[side];
        // the toes: flat on the ground while up on them, else with the foot
        const bool on_toes = feet[side].down && feet[side].pitch > 0;
        Q[hb + 3] = on_toes ? yaw(sgn * S.toe_out + hip_turn) : footQ[side];
    }
    // the arms: swung against the legs (the left arm forward with the right leg), bent at the elbow, pumping on a run
    for (int side = 0; side < 2; side++) {
        const double sgn = side == 0 ? 1.0 : -1.0;
        const int sh = side == 0 ? 2 : 7;
        const double c = std::cos(2 * pi * (ph + (side == 0 ? 0.5 : 0.0)));   // (+1: this arm forward)
        const double swing = lerp(lerp(S.arm_swing_walk, S.arm_swing_run, run), S.arm_swing_sprint, sprint) * (1 - 0.6 * air);
        const double beta = (swing * c + S.arm_back_run * run) * move + 3 * deg * (1 - move) + S.air_arms_up * air;
        const double ab = lerp(S.arm_out_idle, lerp(S.arm_out_walk, S.arm_out_run, run), move) + breathe * 0.5 + S.air_arms_out * air;
        const double elbow = lerp(S.elbow_idle, lerp(S.elbow_walk, S.elbow_run, run), move) + S.elbow_pump * run * move * std::max(0.0, c);
        const V3 ua = unit(v3(sgn * std::sin(ab), -std::cos(ab) * std::cos(beta), std::cos(ab) * std::sin(beta)));
        // (the forearm: the upper arm's way bent forward at the elbow about the arm's own level side axis; in across the body on a run)
        V3 hinge = cross(v3(0, 0, 1), ua);
        if (len(hinge) < 1e-6) hinge = v3(1, 0, 0);
        hinge = unit(hinge);
        if (hinge.x < 0) hinge = hinge * -1.0;
        V3 fa = mulv(ua, rot(hinge, -elbow));
        fa = unit(fa - v3(sgn * std::sin(S.forearm_in_run * run * move), 0, 0));
        Q[sh] = align(off[sh + 1], ua);
        Q[sh + 1] = mul(Q[sh], align(mulv(off[sh + 2], Q[sh]), fa));
        Q[sh + 2] = Q[sh + 1];
        Q[sh + 3] = Q[sh + 1];
        Q[sh + 4] = Q[sh + 1];
    }
    // the sword's: the right hand at its grip, the blade its way; both hands on it (Zack), or the left out for balance (Sabre); the
    // elbows by two bones to the hands, pointing down and out
    if (sw_w > 0 && sword_move != 0) {
        const V3 r1 = rel[0] + mulv(off[1], Q[0]);
        const V3 shoulder[2] = { r1 + mulv(off[2], Q[1]), r1 + mulv(off[7], Q[1]) };
        const V3 mid = (shoulder[0] + shoulder[1]) * 0.5;
        const double reach_arm = len(off[8]) + len(off[9]);
        const V3 grip = mid + sw.grip * reach_arm;
        M3 SQ[28];
        for (int b = 2; b <= 11; b++) SQ[b] = Q[b];
        for (int side = 0; side < 2; side++) {
            const int sh = side == 0 ? 2 : 7;
            const double sgn = side == 0 ? 1.0 : -1.0;
            const double a1 = len(off[sh + 1]), a2 = len(off[sh + 2]);
            V3 target;
            if (side == 1) {
                target = grip;
            } else if (sword_style == 1) {
                target = grip - sw.d * (0.17 * reach_arm);   // (the left hand under the right on the grip)
            } else {
                // (one hand: the left arm out to the side and back, for balance, more as it cuts)
                const double ab = (55 + 15 * std::fabs(std::sin(sw.twist))) * deg, be = -20 * deg, el = 30 * deg;
                const V3 ua = unit(v3(sgn * std::sin(ab), -std::cos(ab) * std::cos(be), std::cos(ab) * std::sin(be)));
                V3 hinge = cross(v3(0, 0, 1), ua);
                hinge = len(hinge) < 1e-6 ? v3(1, 0, 0) : unit(hinge);
                if (hinge.x < 0) hinge = hinge * -1.0;
                const V3 fa = mulv(ua, rot(hinge, -el));
                SQ[sh] = align(off[sh + 1], ua);
                SQ[sh + 1] = mul(SQ[sh], align(mulv(off[sh + 2], SQ[sh]), fa));
                SQ[sh + 2] = SQ[sh + 3] = SQ[sh + 4] = SQ[sh + 1];
                continue;
            }
            V3 reached;
            const V3 pole = v3(sgn * 0.7, -0.6, -0.35);
            const V3 E = two_bone(shoulder[side], target, a1, a2, pole, &reached);
            SQ[sh] = align(off[sh + 1], E - shoulder[side]);
            SQ[sh + 1] = mul(SQ[sh], align(mulv(off[sh + 2], SQ[sh]), reached - E));
            SQ[sh + 2] = sw.hand;
            SQ[sh + 3] = SQ[sh + 4] = sw.hand;
        }
        for (int b = 2; b <= 11; b++) Q[b] = blend(Q[b], SQ[b], sw_w);
    }
    // the tail: behind the pelvis, raised on a run, swaying against its turn (a wave down it)
    for (int i = 0; i < 4; i++) {
        const int b = 24 + i;
        const double sway = -pelvis_yaw * 1.5 + S.tail_sway * move * std::sin(2 * pi * (ph - 0.1 * (i + 1))) * 0.5;
        Q[b] = mul(pitch(S.tail_lift * (0.5 + 0.8 * run * move) * (1.0 + 0.25 * i)), yaw(sway * (0.5 + 0.25 * i)));
    }
    // the joints' places from the root (body ways, model units)
    for (int b = 1; b < 28; b++) rel[b] = rel[kPar[b]] + mulv(off[b], Q[kPar[b]]);

    // ---- with the game's (toned down, as biped_pose) ----
    M3 rw[28];
    rw[0] = mul(damp(mul(gw[0], transp(R1)), 0.5), R1);   // (its lean and roll from the level body, halved)
    double kd[28];
    for (double& v : kd) v = 1.0;
    kd[1] = 0.5; kd[12] = 0.6; kd[16] = kd[20] = 0.65; kd[17] = kd[21] = 0.75; kd[18] = kd[22] = 0.75;
    for (int b = 1; b < 28; b++) {
        const int p = kPar[b];
        if (b >= 2 && b <= 11) {
            rw[b] = gw[b];                                  // (the arms: as the game aims them)
            continue;
        }
        const M3 Lb = mul(gw[b], transp(gw[p]));            // its turn from its parent's
        rw[b] = mul(kd[b] < 1.0 ? damp(Lb, kd[b]) : Lb, rw[p]);
    }
    // (a weapon out, or aiming: the upper body (chest, arms, head) the game's, aiming it)
    const bool armed = s.weapon != 0 || s.scoped != 0 || s.look != 0;
    const bool upper_game = armed && S.armed_upper > 0.5 && !(sword_move != 0 && sw_w > 0.5);
    M3 out[28];
    for (int b = 0; b < 28; b++) {
        const M3 ours = mul(Q[b], R1);
        double wb = w;
        if (upper_game && b >= 1 && b <= 15) wb = 0.0;
        out[b] = blend(rw[b], ours, wb);
    }
    // the places: the root between the game's and ours, the rest down the chain
    const V3 ground = v3(obj.x + s.root_off.x, obj_y + s.ground_c, obj.z + s.root_off.z);
    const V3 our_root = ground + mulv(rel[0], R1) * sc;
    const V3 game_root = place(M[0]);
    V3 pw[28];
    pw[0] = game_root + (our_root - game_root) * w;
    for (int b = 1; b < 28; b++) {
        const int p = kPar[b];
        pw[b] = pw[p] + mulv(off[b], out[p]) * sc;
    }
    Mat R[28];
    for (int b = 0; b < 28; b++) {
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[b].m[i][j] = out[b].r[i][j] * sc;
        R[b].m[3][0] = pw[b].x; R[b].m[3][1] = pw[b].y; R[b].m[3][2] = pw[b].z;
        for (int i = 0; i < 4; i++) for (int j = 0; j < 3; j++) if (!std::isfinite(R[b].m[i][j])) return false;
    }
    for (int b = 0; b < 28; b++) write_mat(rdram, m + (uint32_t)b * 0x40u, R[b]);
    capture(k, frames, s.anim, w, obj, true, M, R);
    done[done_next] = { m, signature(rdram, m) };
    done_next = (done_next + 1) % done.size();
    return true;
}
