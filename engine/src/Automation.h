#pragma once
// Automation: envelopes of breakpoints that move a parameter over time.
//
// Every automatable thing is automated the same way, in normalized values
// (0..1): a device's parameters (ParamInfo maps them to plain values, for
// built-in devices and plug-ins of any format alike) and the mixer's controls
// of a track or the master (volume, pan). Each breakpoint's `curve` bends the
// segment that starts at it; see automationShape(). The UI's own copy of these
// rules is model/automation.py: both must agree.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace sub {

// A breakpoint as the UI sends it, in timeline beats.
struct AutomationPoint {
    double beat = 0.0;
    float value = 0.f;  // normalized
    float curve = 0.f;  // -1..1: how the segment to the next breakpoint bends; 0 is a straight line
};

// An envelope for one target on a track (or the master).
struct AutomationLaneDesc {
    uint32_t processorId = 0;  // 0: the mixer of the track (or master)
    std::string param;         // the processor's parameter id, or a mixer control: "volume", "pan"
    std::vector<AutomationPoint> points;
};

// A breakpoint on the rendering side, in timeline samples.
struct AutomationNode {
    int64_t time = 0;
    float value = 0.f;
    float curve = 0.f;
};

// How far along its way a curved segment is at x (0..1). `bend` > 0 moves
// early and levels off (the curve bulges toward the end value), < 0 the other
// way round; the shape is exponential, so a piece of a curved segment is again
// one (with the bend scaled by the piece's share of it).
constexpr float kAutomationCurvature = 6.f;
inline float automationShape(float x, float bend) noexcept {
    const float a = -bend * kAutomationCurvature;
    if (std::abs(a) < 1e-4f) return x;
    return std::expm1(a * x) / std::expm1(a);
}

// A segment's value at t (from.time <= t < to.time). A breakpoint's curve
// bulges the line upward when positive, whichever way the segment goes.
inline float automationSegment(const AutomationNode& from, const AutomationNode& to, int64_t t) noexcept {
    const float x = static_cast<float>(static_cast<double>(t - from.time) / static_cast<double>(to.time - from.time));
    const float bend = to.value >= from.value ? from.curve : -from.curve;
    return from.value + (to.value - from.value) * automationShape(x, bend);
}

// The number of breakpoints at or before t.
inline size_t automationIndex(const std::vector<AutomationNode>& nodes, int64_t t) noexcept {
    return static_cast<size_t>(std::upper_bound(nodes.begin(), nodes.end(), t,
                                                [](int64_t value, const AutomationNode& node) {
                                                    return value < node.time;
                                                }) -
                               nodes.begin());
}

// The value at t, given `index` = automationIndex(nodes, t). Before the first
// breakpoint the envelope holds the first one's value, after the last the
// last one's; where two share a time (a step), the later one's from then on.
inline float automationValueAt(const std::vector<AutomationNode>& nodes, size_t index, int64_t t) noexcept {
    if (index == 0) return nodes.front().value;
    if (index >= nodes.size()) return nodes.back().value;
    return automationSegment(nodes[index - 1], nodes[index], t);
}

inline float automationValue(const std::vector<AutomationNode>& nodes, int64_t t) noexcept {
    return nodes.empty() ? 0.f : automationValueAt(nodes, automationIndex(nodes, t), t);
}

// The value at each of the n samples from t.
inline void fillAutomation(const std::vector<AutomationNode>& nodes, int64_t t, int n, float* out) noexcept {
    size_t index = automationIndex(nodes, t);
    int i = 0;
    while (i < n) {
        const int64_t now = t + i;
        while (index < nodes.size() && nodes[index].time <= now) ++index;
        // Up to the next breakpoint the value follows one rule.
        const int run =
            index < nodes.size() ? static_cast<int>(std::min<int64_t>(n - i, nodes[index].time - now)) : n - i;
        if (index == 0 || index >= nodes.size()) {
            std::fill_n(out + i, run, index == 0 ? nodes.front().value : nodes.back().value);
        } else {
            const AutomationNode& from = nodes[index - 1];
            const AutomationNode& to = nodes[index];
            for (int k = 0; k < run; ++k) out[i + k] = automationSegment(from, to, now + k);
        }
        i += run;
    }
}

// A value snapped to one of `steps` + 1 evenly spaced ones (a discrete parameter).
inline float automationQuantize(float value, int steps) noexcept {
    if (steps <= 0) return value;
    const auto count = static_cast<float>(steps);
    return std::min(count, std::floor(std::clamp(value, 0.f, 1.f) * (count + 1.f))) / count;
}

// The mixer's controls on their lanes. Volume moves like a fader: the gain is
// the cube of the lane's value, so 1 is +6 dB, about 0.79 is 0 dB and 0 is
// silence, and a straight line is a smooth fade. Pan is -1 (left) at 0 to 1.
constexpr float kMaxVolumeGain = 1.99526231f;  // +6 dB
inline float automationVolumeGain(float value) noexcept {
    value = std::clamp(value, 0.f, 1.f);
    return value * value * value * kMaxVolumeGain;
}
inline float automationPan(float value) noexcept { return std::clamp(value, 0.f, 1.f) * 2.f - 1.f; }

}  // namespace sub
