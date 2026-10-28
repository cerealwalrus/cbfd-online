// The 5th player's "P5" icon for the character select (possess_test.cpp): made at run time from the game's own "P4" icon (sprite 0x897,
// 32 x 32 RGBA32), the P kept as it is and the 4 drawn over as a 5 in the same look (its face's colours row by row, its edges' colours
// by the way they face, all taken from the P4 icon itself). Nothing of the game's is kept in the project: only the 5's shape is ours.
#pragma once

#include <cmath>
#include <cstdint>

namespace conker::p5icon {

constexpr int size = 32;

struct Colour {
    float r = 0, g = 0, b = 0, a = 0;
};

inline Colour colour_of(uint32_t w) {
    return Colour{ (float)((w >> 24) & 0xFF), (float)((w >> 16) & 0xFF), (float)((w >> 8) & 0xFF), (float)(w & 0xFF) };
}

inline uint32_t word_of(const Colour& c) {
    auto clamp = [](float v) -> uint32_t {
        const float r = std::floor(v + 0.5f);
        return r < 0.0f ? 0u : (r > 255.0f ? 255u : (uint32_t)r);
    };
    return (clamp(c.r) << 24) | (clamp(c.g) << 16) | (clamp(c.b) << 8) | clamp(c.a);
}

inline Colour mix(const Colour& a, const Colour& b, float t) {
    return Colour{ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}

// The 5, in texels (x right, y down): its top bar, its stem, its bowl (the right half of a ring, and above and below the ring's hole to
// its left: the shoulder and the bottom stroke).
constexpr float bowl_x = 22.5f, bowl_y = 20.6f, bowl_out = 6.5f, bowl_in = 1.0f;

inline bool in_five(float x, float y) {
    if (y >= 5.5f && y < 11.0f && x >= 19.0f && x < 29.0f) {
        return true;
    }
    if (y >= 5.5f && y < 17.0f && x >= 19.0f && x < 24.5f) {
        return true;
    }
    const float dx = x - bowl_x, dy = y - bowl_y, rr = dx * dx + dy * dy;
    if (x >= bowl_x && rr >= bowl_in * bowl_in && rr < bowl_out * bowl_out) {
        return true;
    }
    if (y >= 14.0f && y < bowl_y - bowl_in && x >= 19.0f && x < bowl_x) {
        return true;
    }
    if (y >= bowl_y + bowl_in && y < bowl_y + bowl_out && x >= 15.0f && x < bowl_x) {
        return true;
    }
    return false;
}

// The P's last texel of each row in the P4 icon (to its right, the 4: from row 11 to 19 its stroke right against the P).
inline int p_last(int y) {
    if (y <= 10) {
        return 18;
    }
    if (y <= 19) {
        static const int last[] = { 18, 17, 16, 15, 14, 13, 12, 11, 10 };
        return last[y - 11];
    }
    return 9;
}

// in and out: the icons' texels in rows (in[y * 32 + x], RGBA as the game has them, R the top byte), the swizzle of the texture's
// odd rows undone.
inline void make(const uint32_t* in, uint32_t* out) {
    auto at = [&](int x, int y) { return colour_of(in[y * size + x]); };
    auto face = [&](int y) {
        Colour c = at(6, y < 8 ? 8 : (y > 24 ? 24 : y));   // the P's stem
        c.a = 255.0f;
        return c;
    };
    // The edges' colours, from the 4's stem: its right (the light's side), left, top and the P's bottom; an edge's outer texel, then the
    // one inside it.
    const Colour right_out = at(26, 10), left_out = at(20, 10), top_out = at(24, 6), bottom_out = at(5, 25);
    const Colour right_in = at(25, 10);
    auto edge = [&](float nx, float ny, bool outer, const Colour& f) {
        float w_right = nx > 0 ? nx : 0, w_left = nx < 0 ? -nx : 0, w_top = ny < 0 ? -ny : 0, w_bottom = ny > 0 ? ny : 0;
        w_top *= 1.0f + (nx < 0 ? nx : 0);   // a top-left edge is dark, as the 4's is
        float sum = w_right + w_left + w_top + w_bottom;
        if (sum <= 0.0f) {
            w_left = sum = 1.0f;
        }
        const Colour r = outer ? right_out : right_in;
        const Colour l = outer ? left_out : mix(left_out, f, 0.42f);
        const Colour t = outer ? top_out : mix(top_out, f, 0.5f);
        const Colour b = outer ? bottom_out : f;
        Colour c;
        c.r = (r.r * w_right + l.r * w_left + t.r * w_top + b.r * w_bottom) / sum;
        c.g = (r.g * w_right + l.g * w_left + t.g * w_top + b.g * w_bottom) / sum;
        c.b = (r.b * w_right + l.b * w_left + t.b * w_top + b.b * w_bottom) / sum;
        c.a = (r.a * w_right + l.a * w_left + t.a * w_top + b.a * w_bottom) / sum;
        return c;
    };

    // The 5 at 8 x 8 samples a texel.
    constexpr int ss = 8, n = size * ss;
    static bool mask[n][n];
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            mask[j][i] = in_five((i + 0.5f) / ss, (j + 0.5f) / ss);
        }
    }
    // From a texel's centre, the nearest sample out of the 5 (or, from outside, in it) within reach: its distance, in texels, and the
    // way to it.
    auto nearest = [&](int x, int y, bool want_inside, int reach, float& dist, float& nx, float& ny) {
        const int cx = x * ss + ss / 2, cy = y * ss + ss / 2;
        float best = 1e30f, bx = 0, by = 0;
        for (int j = cy - reach; j < cy + reach; j++) {
            if (j < 0 || j >= n) {
                continue;
            }
            for (int i = cx - reach; i < cx + reach; i++) {
                if (i < 0 || i >= n || mask[j][i] != want_inside) {
                    continue;
                }
                const float dx = i + 0.5f - cx, dy = j + 0.5f - cy, dd = dx * dx + dy * dy;
                if (dd < best) {
                    best = dd;
                    bx = dx;
                    by = dy;
                }
            }
        }
        if (best >= 1e30f) {
            dist = 1e9f;
            nx = ny = 0;
            return;
        }
        dist = std::sqrt(best) / ss;
        const float l = std::sqrt(bx * bx + by * by);
        nx = l > 0 ? bx / l : 0;
        ny = l > 0 ? by / l : 0;
    };

    const Colour clear = Colour{ at(0, 0).r, at(0, 0).g, at(0, 0).b, 0.0f };
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            // The P (and nothing right of it).
            Colour base = x <= p_last(y) ? at(x, y) : clear;
            // Where the 4 was right against it, the P's own edge: facing right (row 11), then down and right.
            if (y >= 11 && y <= 19 && (x == p_last(y) || x == p_last(y) - 1)) {
                const bool outer = x == p_last(y);
                const float nx = y == 11 ? 1.0f : 0.7071f, ny = y == 11 ? 0.0f : 0.7071f;
                const Colour c = edge(nx, ny, outer, face(y));
                base = Colour{ c.r, c.g, c.b, outer ? c.a : 255.0f };
            }
            // The 5, over it.
            int covered = 0;
            for (int j = 0; j < ss; j++) {
                for (int i = 0; i < ss; i++) {
                    covered += mask[y * ss + j][x * ss + i] ? 1 : 0;
                }
            }
            if (covered > 0) {
                const Colour f = face(y);
                Colour c;
                float d, nx, ny;
                if (mask[y * ss + ss / 2][x * ss + ss / 2]) {
                    nearest(x, y, false, 3 * ss, d, nx, ny);
                    c = d >= 2.0f ? f : edge(nx, ny, d < 1.0f, f);
                } else {
                    nearest(x, y, true, ss, d, nx, ny);
                    c = edge(-nx, -ny, true, f);
                }
                const float a = c.a / 255.0f * covered / (float)(ss * ss);
                const float under = base.a / 255.0f * (1.0f - a), all = a + under;
                if (all > 0.0f) {
                    base.r = (c.r * a + base.r * under) / all;
                    base.g = (c.g * a + base.g * under) / all;
                    base.b = (c.b * a + base.b * under) / all;
                }
                base.a = all * 255.0f;
            }
            out[y * size + x] = word_of(base);
        }
    }
}

}  // namespace conker::p5icon
