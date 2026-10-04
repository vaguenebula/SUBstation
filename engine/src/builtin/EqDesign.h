#pragma once
// The EQ's filters: each band, from its type, frequency, gain, Q and slope, as
// a cascade of second-order (and one first-order) sections. Shared by the EQ
// device (builtin/devices/Eq.cpp) and the bindings (eq_response), so the curve
// the editor draws is the one the engine plays.
//
// Each section is an analog prototype made digital with matched poles and a
// magnitude-matched numerator (M. Vicanek, "Matched Second Order Digital
// Filters", 2016): the poles by impulse invariance, the numerator so the
// response is the analog one's at 0 Hz, at Nyquist and at the section's own
// frequency. Unlike the bilinear transform, a bell or shelf high up keeps its
// shape instead of being squeezed against Nyquist.

#include <algorithm>
#include <cmath>
#include <complex>

namespace sub::eq {

enum Type { Bell = 0, LowShelf, LowCut, HighShelf, HighCut, Notch, BandPass, TiltShelf, NumTypes };

constexpr int kSlopes[] = {6, 12, 18, 24, 30, 36, 48, 72, 96};  // dB/octave: orders 1..16
constexpr int kNumSlopes = 9;
constexpr int kMaxSections = 8;  // 96 dB/octave: order 16
constexpr double kPi = 3.14159265358979323846;
constexpr double kButterworthQ = 0.70710678118654752;

inline bool usesGain(int type) noexcept {
    return type == Bell || type == LowShelf || type == HighShelf || type == TiltShelf;
}
inline bool usesSlope(int type) noexcept {
    return type == LowShelf || type == HighShelf || type == LowCut || type == HighCut || type == TiltShelf;
}

// b0 + b1 z^-1 + b2 z^-2 over 1 + a1 z^-1 + a2 z^-2.
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    // |H| squared at w (radians per sample).
    double magnitudeSquared(double w) const noexcept {
        const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
        return std::norm(b0 + b1 * z1 + b2 * z2) / std::norm(1.0 + a1 * z1 + a2 * z2);
    }
};

struct Design {
    Biquad sections[kMaxSections];
    int count = 0;

    double magnitudeSquared(double w) const noexcept {
        double m = 1.0;
        for (int i = 0; i < count; ++i) m *= sections[i].magnitudeSquared(w);
        return m;
    }
};

// An analog section (n2 s² + n1 s + n0) / (d2 s² + d1 s + d0), s in radians per
// sample; d2 = n2 = 0 for a first-order one.
struct Analog {
    double n2, n1, n0, d2, d1, d0;

    double magnitudeSquared(double w) const noexcept {
        const double nr = n0 - n2 * w * w, ni = n1 * w;
        const double dr = d0 - d2 * w * w, di = d1 * w;
        return (nr * nr + ni * ni) / (dr * dr + di * di);
    }
};

// The analog section made digital (see the top); `w` is its own frequency, where it matches exactly.
inline Biquad matched(const Analog& a, double w) noexcept {
    const double nyquistMagnitude2 = a.magnitudeSquared(kPi);
    const double dcMagnitude2 = a.magnitudeSquared(0.0);
    Biquad q;
    if (a.d2 == 0.0) {  // first order: the pole, then the numerator matching 0 Hz and Nyquist
        const double pole = std::exp(-std::min(a.d0 / a.d1, 0.97 * kPi));
        q.a1 = -pole;
        const double atDc = std::sqrt(dcMagnitude2) * (1.0 - pole);
        const double atNyquist = std::sqrt(nyquistMagnitude2) * (1.0 + pole);
        q.b0 = 0.5 * (atDc + atNyquist);
        q.b1 = 0.5 * (atDc - atNyquist);
        return q;
    }
    // Poles by impulse invariance (their frequency kept below Nyquist, where they would alias).
    const double wp = std::min(std::sqrt(a.d0 / a.d2), 0.97 * kPi);
    const double zeta = a.d1 / (2.0 * std::sqrt(a.d0 * a.d2));
    const double decay = std::exp(-zeta * wp);
    if (zeta < 1.0) {
        q.a1 = -2.0 * decay * std::cos(std::min(wp * std::sqrt(1.0 - zeta * zeta), kPi));
    } else {
        q.a1 = -2.0 * decay * std::cosh(wp * std::sqrt(zeta * zeta - 1.0));
    }
    q.a2 = decay * decay;

    // |A|² and |B|² as A0 φ0 + A1 φ1 + A2 φ2 (Vicanek's φ basis); match at 0, Nyquist and wm.
    const double wm = std::clamp(w, 1e-6, 0.95 * kPi);
    const double s = std::sin(0.5 * wm);
    const double phi1 = s * s, phi0 = 1.0 - phi1, phi2 = 4.0 * phi0 * phi1;
    const double A0 = (1.0 + q.a1 + q.a2) * (1.0 + q.a1 + q.a2);
    const double A1 = (1.0 - q.a1 + q.a2) * (1.0 - q.a1 + q.a2);
    const double A2 = -4.0 * q.a2;
    const double B0 = A0 * dcMagnitude2;
    double B1 = A1 * nyquistMagnitude2;
    const double target = a.magnitudeSquared(wm) * (A0 * phi0 + A1 * phi1 + A2 * phi2);
    double B2 = (target - B0 * phi0 - B1 * phi1) / phi2;
    const double root0 = std::sqrt(B0);
    double root1 = std::sqrt(B1);
    if (0.25 * (root0 + root1) * (root0 + root1) + B2 < 0.0) {
        // No real numerator has all three (a resonant cut, a notch high up): keep 0 Hz and wm, and
        // take the Nyquist magnitude that just can be had (W² + B2 = 0), solved for its root r.
        const double qa = 0.25 * (1.0 - 1.0 / phi0), qb = 0.5 * root0, qc = 0.25 * B0 + (target - B0 * phi0) / phi2;
        const double disc = std::max(0.0, qb * qb - 4.0 * qa * qc);
        root1 = std::max(0.0, (-qb - std::sqrt(disc)) / (2.0 * qa));
        B1 = root1 * root1;
        B2 = (target - B0 * phi0 - B1 * phi1) / phi2;
    }
    const double W = 0.5 * (root0 + root1);
    q.b0 = 0.5 * (W + std::sqrt(std::max(0.0, W * W + B2)));
    q.b1 = 0.5 * (root0 - root1);
    q.b2 = q.b0 != 0.0 ? -B2 / (4.0 * q.b0) : 0.0;
    return q;
}

// The Butterworth Q of section k (0: the most resonant) of an order-n filter.
inline double butterworthQ(int n, int k) noexcept {
    return 1.0 / (2.0 * std::sin((2.0 * k + 1.0) * kPi / (2.0 * n)));
}

// A band's sections. `freq` in Hz, `gain` in dB, `slope` an index into kSlopes.
inline Design design(int type, double freq, double gain, double q, int slope, double sampleRate) noexcept {
    Design d;
    const double w = 2.0 * kPi * std::clamp(freq, 1.0, 0.5 * sampleRate) / sampleRate;
    q = std::clamp(q, 0.01, 100.0);
    const int order = usesSlope(type) ? kSlopes[std::clamp(slope, 0, kNumSlopes - 1)] / 6 : 2;
    const auto add = [&](const Analog& a) {
        if (d.count < kMaxSections) d.sections[d.count++] = matched(a, w);
    };
    // Section k's Q: Butterworth's, the most resonant one scaled by the band's Q.
    const auto sectionQ = [&](int k) { return butterworthQ(order, k) * (k == 0 ? q / kButterworthQ : 1.0); };
    const int pairs = order / 2;
    const bool odd = order % 2 == 1;
    const double w2 = w * w;

    switch (type) {
        case Bell: {
            const double A = std::pow(10.0, gain / 40.0);
            add({1.0, A * w / q, w2, 1.0, w / (A * q), w2});
            break;
        }
        case Notch:
            add({1.0, 0.0, w2, 1.0, w / q, w2});
            break;
        case BandPass:
            add({0.0, w / q, 0.0, 1.0, w / q, w2});
            break;
        case LowCut:
            if (odd) add({0.0, 1.0, 0.0, 0.0, 1.0, w});
            for (int k = 0; k < pairs; ++k) add({1.0, 0.0, 0.0, 1.0, w / sectionQ(k), w2});
            break;
        case HighCut:
            if (odd) add({0.0, 0.0, w, 0.0, 1.0, w});
            for (int k = 0; k < pairs; ++k) add({0.0, 0.0, w2, 1.0, w / sectionQ(k), w2});
            break;
        case LowShelf:
        case HighShelf:
        case TiltShelf: {
            // Each section takes its share of the gain (a first-order one half a second-order one's).
            const double share = gain / order;
            const bool low = type == LowShelf;
            if (odd) {
                const double g = std::sqrt(std::pow(10.0, share / 20.0));  // the square root of its gain
                if (low) add({0.0, 1.0, w * g, 0.0, 1.0, w / g});
                else add({0.0, g * g, w * g, 0.0, 1.0, w * g});
            }
            for (int k = 0; k < pairs; ++k) {
                const double A = std::pow(10.0, 2.0 * share / 40.0);
                const double rootA = std::sqrt(A), sq = sectionQ(k);
                if (low) add({A, A * rootA / sq * w, A * A * w2, A, rootA / sq * w, w2});
                else add({A * A, A * rootA / sq * w, A * w2, 1.0, rootA / sq * w, A * w2});
            }
            if (type == TiltShelf && d.count > 0) {  // half the gain down below, half up above
                const double down = std::pow(10.0, -gain / 40.0);
                d.sections[0].b0 *= down;
                d.sections[0].b1 *= down;
                d.sections[0].b2 *= down;
            }
            break;
        }
        default:
            break;
    }
    return d;
}

}  // namespace sub::eq
