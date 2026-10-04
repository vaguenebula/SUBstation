#include "model/Numbers.h"

#include <cmath>
#include <cstdio>

namespace sub::app {

double roundHalfEven(double value) {
    const double floor = std::floor(value);
    const double diff = value - floor;
    if (diff < 0.5) return floor;
    if (diff > 0.5) return floor + 1.0;
    if (diff == 0.5) return std::fmod(floor, 2.0) == 0.0 ? floor : floor + 1.0;
    return value;  // not a finite number
}

double floorDiv(double a, double b) {
    double mod = std::fmod(a, b);
    // fmod is exact, so a - mod is mathematically a multiple of b; the
    // division may land just off a whole number, which is snapped to it.
    double div = (a - mod) / b;
    if (mod != 0.0) {
        if ((b < 0) != (mod < 0)) {  // the remainder takes the divisor's sign
            mod += b;
            div -= 1.0;
        }
    }
    if (div != 0.0) {
        double floored = std::floor(div);
        if (div - floored > 0.5) floored += 1.0;
        return floored;
    }
    return std::copysign(0.0, a / b);
}

QString formatFixed(double value, int decimals, bool sign) {
    char buffer[512];
    std::snprintf(buffer, sizeof buffer, sign ? "%+.*f" : "%.*f", decimals, value);
    return QString::fromLatin1(buffer);
}

}  // namespace sub::app
