// Trigonometry made of plain arithmetic, for numbers that go into the game's memory online: every machine of an online
// game has to compute exactly the same, and the C library's sin, cos, atan2... pick a version by the processor (they can
// differ in the last bit from one PC to another). Additions, multiplications and std::remainder are exact and the same
// everywhere. Accurate to about a hundred-thousandth of a radian. (Moved here from mouse_camera.cpp.)
#pragma once

#include <cmath>

namespace conker::det {
    constexpr double pi = 3.14159265358979323846;
    inline double sin_d(double x) {
        x = std::remainder(x, 2.0 * pi);
        if (x > pi / 2) {
            x = pi - x;
        }
        else if (x < -pi / 2) {
            x = -pi - x;
        }
        const double z = x * x;
        return x * (1.0 + z * (-1.0 / 6 + z * (1.0 / 120 + z * (-1.0 / 5040 + z * (1.0 / 362880 + z * (-1.0 / 39916800))))));
    }
    inline float sin(float x) {
        return (float)sin_d((double)x);
    }
    inline float cos(float x) {
        return (float)sin_d((double)x + pi / 2);
    }
    inline double atan_unit(double x) {   // |x| <= 1
        const double z = x * x;
        return x * (0.9998660 + z * (-0.3302995 + z * (0.1801410 + z * (-0.0851330 + z * 0.0208351))));
    }
    inline double atan2_d(double y, double x) {
        if (x == 0.0 && y == 0.0) {
            return 0.0;
        }
        const double ax = std::fabs(x), ay = std::fabs(y);
        double a = ay > ax ? pi / 2 - atan_unit(ax / ay) : atan_unit(ay / ax);
        if (x < 0.0) {
            a = pi - a;
        }
        return y < 0.0 ? -a : a;
    }
    inline float atan2(float y, float x) {
        return (float)atan2_d((double)y, (double)x);
    }
    inline float tan(float x) {
        const double s = sin_d((double)x), c = sin_d((double)x + pi / 2);
        return (float)(s / c);
    }
}
