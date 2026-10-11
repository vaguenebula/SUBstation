#pragma once
// A note's pitch bend: MIDI 2.0's per-note pitch bend, drawn as a curve.
//
// A note's bend is a curve of breakpoints over its length, in semitones from
// its key, plus stretches of vibrato on top of it. The curve starts at 0 (the
// note's own pitch) at the note's start and goes to its first point, through
// the others (a point's curve bends the segment starting at it, as an
// automation breakpoint's does: automationShape()), then holds the last
// point's value to the note's end; a point at the start sets where it starts.
// A vibrato swells in over its first `fade` of its length, swings `depth`
// semitones either way `rate` times a unit of time, and dies away over its last
// tenth, so it begins and ends on the curve: drawn over a bend, it follows it.
//
// The rules are written once, here, for whatever unit time is in: the engine
// plays them in samples (Snapshot.h's NoteBendRender), the application layer
// draws and edits them in beats (app/src/model/Clip.h's Note), with any types
// whose fields are named as BendPoint's and VibratoSpan's. Header-only, no
// engine types: the application layer includes it.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

#include "Automation.h"

namespace sub {

// How far a note bends either way. MIDI 2.0's (and MPE's) per-note pitch bend
// range is 48 semitones by default; a bend beyond it is held to it.
inline constexpr double kMaxBendSemitones = 48.0;
// A vibrato dies away over this share of its length at its end.
inline constexpr double kVibratoRelease = 0.1;

// A breakpoint of a note's bend: `time` from the note's start, `semitones` from
// its key, and how the segment from it to the next one bends (-1..1).
struct BendPoint {
    double time = 0.0;
    double semitones = 0.0;
    double curve = 0.0;
};

// A stretch of vibrato over a note, added to its curve: from `start` (from the
// note's start) for `length`, swinging `depth` semitones either way (its peak),
// `rate` cycles a unit of time, swelling in over the first `fade` (0..1) of it.
struct VibratoSpan {
    double start = 0.0;
    double length = 0.0;
    double depth = 0.5;
    double rate = 0.0;
    double fade = 0.3;
};

namespace bend {

// 0 at 0, 1 at 1, flat at both ends (smoothstep), held outside.
inline double smooth(double u) noexcept {
    if (u <= 0.0) return 0.0;
    if (u >= 1.0) return 1.0;
    return u * u * (3.0 - 2.0 * u);
}

// The curve's value at `t` (from the note's start), from points sorted by time.
template <typename Points>
double curveAt(const Points& points, double t) noexcept {
    const size_t count = std::size(points);
    if (count == 0) return 0.0;
    size_t next = 0;  // the first point after t
    while (next < count && points[next].time <= t) ++next;
    if (next == count) return std::clamp(static_cast<double>(points[count - 1].semitones), -kMaxBendSemitones, kMaxBendSemitones);
    // From the note's own pitch at its start, or from the point before.
    const double fromTime = next == 0 ? 0.0 : points[next - 1].time;
    const double fromValue = next == 0 ? 0.0 : static_cast<double>(points[next - 1].semitones);
    const double curve = next == 0 ? 0.0 : static_cast<double>(points[next - 1].curve);
    const double toValue = points[next].semitones;
    const double span = points[next].time - fromTime;
    double value = toValue;
    if (span > 0.0) {
        const double x = std::clamp((t - fromTime) / span, 0.0, 1.0);
        const auto shape = static_cast<float>(toValue >= fromValue ? curve : -curve);
        // (A straight segment in double precision; a curved one as automation bends it.)
        const double along = curve == 0.0 ? x : static_cast<double>(automationShape(static_cast<float>(x), std::clamp(shape, -1.f, 1.f)));
        value = fromValue + (toValue - fromValue) * along;
    }
    return std::clamp(value, -kMaxBendSemitones, kMaxBendSemitones);
}

// What one vibrato adds at `t` (from the note's start). `rateScale` turns its
// rate into cycles per unit of `t` (1 if it already is).
template <typename Vibrato>
double vibratoAt(const Vibrato& vibrato, double t, double rateScale = 1.0) noexcept {
    const double length = vibrato.length;
    const double x = t - vibrato.start;
    if (!(length > 0.0) || x <= 0.0 || x >= length) return 0.0;
    const double u = x / length;
    const double fade = std::clamp(static_cast<double>(vibrato.fade), 0.0, 1.0);
    const double swell = fade > 0.0 ? smooth(u / fade) : 1.0;
    const double release = smooth((1.0 - u) / kVibratoRelease);
    const double phase = 2.0 * std::numbers::pi * static_cast<double>(vibrato.rate) * rateScale * x;
    return static_cast<double>(vibrato.depth) * swell * release * std::sin(phase);
}

// The note's bend at `t`: its curve plus its vibratos, held to the range.
template <typename Points, typename Vibratos>
double at(const Points& points, const Vibratos& vibratos, double t, double rateScale = 1.0) noexcept {
    double value = curveAt(points, t);
    for (const auto& vibrato : vibratos) value += vibratoAt(vibrato, t, rateScale);
    return std::clamp(value, -kMaxBendSemitones, kMaxBendSemitones);
}

}  // namespace bend
}  // namespace sub
