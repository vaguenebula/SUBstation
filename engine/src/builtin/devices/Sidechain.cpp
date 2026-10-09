// Built-in "Sidechain" device: ducks its input along a curve each time a hit
// comes, to make room for a kick: the curve can be drawn, or fitted by the
// editor to the envelope of the kick where it clashes with the input.
//
// A hit is found sample-accurately in the sidechain (the kick's track): the
// first sample at or above the threshold, once the key has fallen back below
// it (3 dB of hysteresis, and 20 ms at least between hits). Or the hits come
// on the beat (every bar, 1/2 .. 1/16) while the transport plays, with no
// sidechain at all. Without a sidechain chosen, nothing triggers (a ducker
// keyed by its own input would only duck its own notes).
//
// The curve is up to kPoints breakpoints (x: 0..1 of the curve's length, y:
// 0 = ducked by the depth, 1 = untouched), each bending the segment after it
// as automation does (automationShape), evaluated at every sample from the
// hit on; it holds its last value until the next hit. Its length is in ms,
// or synced to the tempo. The gain is smoothed (`smooth`, a one-pole) so a
// curve that jumps doesn't click unless asked to.
//
// Lookahead delays the input (reported as latency, so the rest of the mix is
// delayed to match): the curve then starts that much before the kick.
// "Lows" ducks only what is below the crossover: the input is split in two
// (Linkwitz-Riley, 24 dB/octave, so the two add up flat) and only the low
// part is ducked.
//
// The editor reads three displays, one value per sample, in step: the key
// summed to mono, the input (before the lookahead) summed to mono, and the
// samples since the latest hit (-1 once its curve is over).

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

#include "Automation.h"
#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/Dsp.h"
#include "builtin/EqDesign.h"
#include "rt/RtUtils.h"

namespace sub {
namespace {

constexpr int kPoints = 16;
constexpr double kMaxLookaheadMs = 20.0;
constexpr double kMinGapMs = 20.0;            // between two hits from the key
constexpr float kHysteresis = 0.70794578f;   // -3 dB: the key falls this far below the threshold to re-arm

const std::vector<std::string>& triggerLabels() {
    static const std::vector<std::string> kLabels = {"Sidechain", "Every Bar", "Every 1/2",
                                                     "Every 1/4", "Every 1/8", "Every 1/16"};
    return kLabels;
}
constexpr double kTriggerBeats[] = {0.0, -1.0, 2.0, 1.0, 0.5, 0.25};  // -1: a bar

const std::vector<std::string>& rateLabels() {
    static const std::vector<std::string> kLabels = {"1/32", "1/16", "1/8", "3/16", "1/4", "3/8", "1/2", "1 Bar"};
    return kLabels;
}
constexpr double kRateBeats[] = {0.125, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, -1.0};  // -1: a bar

// The default curve: ducked at once, held a little, then back up, easing in.
struct DefaultPoint {
    float x, y, curve;
};
constexpr DefaultPoint kDefaultCurve[] = {{0.f, 0.f, 0.f}, {0.1f, 0.f, -0.45f}, {1.f, 1.f, 0.f}};

class SidechainProcessor final : public BuiltinProcessor {
public:
    enum Param {
        Trigger = 0, Threshold, Depth, Sync, Length, Rate, Smooth, Lookahead, Range, Crossover,
        AutoFit, Character, kFirstPoint
    };
    enum PointParam { Used = 0, X, Y, Curve, kPointParams };
    enum Display { KeyDisplay = 0, InputDisplay, PhaseDisplay };

    SidechainProcessor() : BuiltinProcessor(infos(), {{"key", 1}, {"input", 1}, {"phase", 1}}) {}

    std::string typeId() const override { return "builtin:sidechain"; }
    std::string name() const override { return "Sidechain"; }
    bool hasSidechain() const override { return true; }

    int latencySamples() const override { return lookaheadSamples(); }

    // The lookahead changed: the engine must realign the tracks.
    bool idle() override {
        const int latency = lookaheadSamples();
        if (latency == reportedLatency_) return false;
        reportedLatency_ = latency;
        return true;
    }

    void prepare(double sampleRate, int) override {
        sampleRate_ = sampleRate;
        const auto capacity = static_cast<size_t>(std::ceil(kMaxLookaheadMs * 0.001 * sampleRate)) + 1;
        for (auto& delay : delay_) delay.assign(capacity, 0.f);
        reportedLatency_ = lookaheadSamples();
        reset();
    }

    void reset() override {
        for (auto& delay : delay_) std::fill(delay.begin(), delay.end(), 0.f);
        writeAt_ = 0;
        for (auto& channel : splitState_) {
            for (auto& band : channel) for (auto& section : band) section[0] = section[1] = 0.0;
        }
        sinceHit_ = -1;
        armed_ = true;
        sinceKeyHit_ = 1 << 30;
        keyEnvelope_ = 0.f;
        sinceBeatHit_ = int64_t{1} << 40;
        gain_ = -1.f;  // (the curve's resting value, at the first sample)
        crossover_ = -1.f;
    }

protected:
    void render(const ProcessContext& ctx, float* const* ch, int numChannels, int numFrames) override {
        const int n = std::min(numChannels, 2);
        if (n <= 0) return;
        loadCurve();
        const double lengthSamples = std::max(1.0, curveLength(ctx) * 0.001 * sampleRate_);
        const float depth = std::clamp(param(Depth) / 100.f, 0.f, 1.f);
        const float threshold = std::pow(10.f, param(Threshold) / 20.f);
        const float smoothMs = param(Smooth);
        const float smoothing = smoothMs <= 0.01f ? 0.f : static_cast<float>(onePoleCoefficient(smoothMs * 0.001, sampleRate_));
        const float keyRelease = static_cast<float>(onePoleCoefficient(0.01, sampleRate_));
        const int minGap = static_cast<int>(kMinGapMs * 0.001 * sampleRate_);
        const int lookahead = std::min(lookaheadSamples(), static_cast<int>(delay_[0].size()) - 1);
        const bool lows = choiceIndex(Range) == 1;
        updateCrossover();

        const int trigger = std::clamp(choiceIndex(Trigger), 0,
                                       static_cast<int>(std::size(kTriggerBeats)) - 1);
        const bool keyed = trigger == 0 && sidechainConnected();
        const float* keyL = keyed ? sidechain(0) : nullptr;
        const float* keyR = keyed ? sidechain(1) : nullptr;
        // On the beat: the first beat hit in this stretch, then every `every` samples after.
        double nextBeatHit = -1.0, everySamples = 0.0;
        if (trigger > 0 && ctx.playing && ctx.tempo > 0.0) {
            const double every = kTriggerBeats[trigger] < 0.0 ? ctx.beatsPerBar() : kTriggerBeats[trigger];
            const double samplesPerBeat = sampleRate_ * 60.0 / ctx.tempo;
            const double first = std::ceil(ctx.beatPos / every - 1e-9) * every;
            nextBeatHit = std::max(0.0, (first - ctx.beatPos) * samplesPerBeat);
            everySamples = every * samplesPerBeat;
        }

        const int size = static_cast<int>(delay_[0].size());
        for (int i = 0; i < numFrames; ++i) {
            // The key: a hit is its first sample at the threshold, once re-armed.
            float key = 0.f;
            if (keyL != nullptr) {
                key = std::max(std::abs(keyL[i]), std::abs(keyR[i]));
                publish(KeyDisplay, 0.5f * (keyL[i] + keyR[i]));
            } else {
                publish(KeyDisplay, 0.f);
            }
            keyEnvelope_ = dsp::followPeak(keyEnvelope_, key, keyRelease);
            if (sinceKeyHit_ < (1 << 30)) ++sinceKeyHit_;
            if (sinceBeatHit_ < (int64_t{1} << 40)) ++sinceBeatHit_;
            bool hit = false;
            if (keyed) {
                if (armed_ && key >= threshold && sinceKeyHit_ >= minGap) {
                    hit = true;
                    armed_ = false;
                    sinceKeyHit_ = 0;
                } else if (!armed_ && keyEnvelope_ < threshold * kHysteresis) {
                    armed_ = true;
                }
            }
            if (nextBeatHit >= 0.0 && i == static_cast<int>(std::llround(nextBeatHit))) {
                // (not again at the start of the next stretch, where a beat rounds to either side)
                if (sinceBeatHit_ > everySamples / 2.0) hit = true;
                sinceBeatHit_ = 0;
                nextBeatHit += everySamples;
            }
            if (hit) sinceHit_ = 0;

            // Where the curve is.
            float y;
            if (sinceHit_ >= 0) {
                y = curveAt(static_cast<double>(sinceHit_) / lengthSamples);
                publish(PhaseDisplay, static_cast<float>(sinceHit_));
                if (++sinceHit_ > lengthSamples) sinceHit_ = -1;
            } else {
                y = restingValue();
                publish(PhaseDisplay, -1.f);
            }
            const float target = 1.f - depth * (1.f - y);
            gain_ = gain_ < 0.f ? target : target + smoothing * (gain_ - target);

            // The input, through the lookahead, ducked.
            float mono = 0.f;
            for (int c = 0; c < n; ++c) {
                const float in = ch[c][i];
                mono += in;
                delay_[c][static_cast<size_t>(writeAt_)] = in;
                int readAt = writeAt_ - lookahead;
                if (readAt < 0) readAt += size;
                const float delayed = delay_[c][static_cast<size_t>(readAt)];
                const float low = split(c, 0, delayed), high = split(c, 1, delayed);
                ch[c][i] = lows ? high + gain_ * low : delayed * gain_;
            }
            publish(InputDisplay, mono / static_cast<float>(n));
            if (++writeAt_ == size) writeAt_ = 0;
        }
        flushSplitState();
    }

private:
    struct Point {
        float x, y, curve;
    };

    static int pointParam(int point, int which) noexcept { return kFirstPoint + point * kPointParams + which; }

    int lookaheadSamples() const noexcept {
        return static_cast<int>(std::lround(std::clamp(static_cast<double>(param(Lookahead)), 0.0, kMaxLookaheadMs) *
                                            0.001 * sampleRate_));
    }

    // The curve's length in ms: its own, or its rate at the tempo.
    double curveLength(const ProcessContext& ctx) const noexcept {
        if (!isOn(Sync) || ctx.tempo <= 0.0) return param(Length);
        const int rate = std::clamp(choiceIndex(Rate), 0,
                                    static_cast<int>(std::size(kRateBeats)) - 1);
        const double beats = kRateBeats[rate] < 0.0 ? ctx.beatsPerBar() : kRateBeats[rate];
        return beats * 60000.0 / ctx.tempo;
    }

    // The breakpoints in use, in order of x.
    void loadCurve() noexcept {
        numPoints_ = 0;
        for (int p = 0; p < kPoints; ++p) {
            if (!isOn(pointParam(p, Used))) continue;
            points_[static_cast<size_t>(numPoints_++)] = {std::clamp(param(pointParam(p, X)), 0.f, 1.f),
                                                          std::clamp(param(pointParam(p, Y)), 0.f, 1.f),
                                                          std::clamp(param(pointParam(p, Curve)), -1.f, 1.f)};
        }
        std::stable_sort(points_.begin(), points_.begin() + numPoints_,
                         [](const Point& a, const Point& b) { return a.x < b.x; });
    }

    float restingValue() const noexcept { return numPoints_ > 0 ? points_[static_cast<size_t>(numPoints_ - 1)].y : 1.f; }

    // The curve at x (0..1 of its length): the first point's value before it, the last's after.
    float curveAt(double x) const noexcept {
        if (numPoints_ == 0) return 1.f;
        const auto fx = static_cast<float>(x);
        if (fx <= points_[0].x) return points_[0].y;
        for (int p = 1; p < numPoints_; ++p) {
            const Point& to = points_[static_cast<size_t>(p)];
            if (fx >= to.x) continue;
            const Point& from = points_[static_cast<size_t>(p - 1)];
            const float span = to.x - from.x;
            if (span <= 0.f) return to.y;
            const float bend = to.y >= from.y ? from.curve : -from.curve;
            return from.y + (to.y - from.y) * automationShape((fx - from.x) / span, bend);
        }
        return restingValue();
    }

    // The Linkwitz-Riley crossover: per band (low, high), two Butterworth sections.
    void updateCrossover() noexcept {
        const float freq = param(Crossover);
        if (freq == crossover_) return;
        crossover_ = freq;
        const double w = 2.0 * std::numbers::pi * std::min(static_cast<double>(freq), 0.45 * sampleRate_) / sampleRate_;
        const double cosw = std::cos(w), alpha = std::sin(w) / (2.0 * 0.70710678118654752);
        const double a0 = 1.0 + alpha;
        split_[0] = {(1.0 - cosw) / 2.0 / a0, (1.0 - cosw) / a0, (1.0 - cosw) / 2.0 / a0, -2.0 * cosw / a0,
                     (1.0 - alpha) / a0};
        split_[1] = {(1.0 + cosw) / 2.0 / a0, -(1.0 + cosw) / a0, (1.0 + cosw) / 2.0 / a0, -2.0 * cosw / a0,
                     (1.0 - alpha) / a0};
    }

    // Channel c's sample through band `band`'s two sections.
    float split(int c, int band, float x) noexcept {
        const eq::Biquad& q = split_[band];
        double value = x;
        for (auto& state : splitState_[c][band]) value = q.tick(value, state[0], state[1]);
        return static_cast<float>(value);
    }

    void flushSplitState() noexcept {
        for (auto& channel : splitState_) {
            for (auto& band : channel) {
                for (auto& section : band) {
                    for (double& z : section) z = dsp::flushTiny(z);
                }
            }
        }
    }

    static const std::vector<ParamInfo>& infos() {
        static const std::vector<ParamInfo> kInfos = [] {
            const std::vector<std::string>& kOnOff = offOnLabels();
            static const std::vector<std::string> kRanges = {"Full", "Lows"};
            static const std::vector<std::string> kCharacters = {"Tight", "Natural", "Loose"};
            std::vector<ParamInfo> list = {
                {"trigger", "Trigger", "", 0.f, static_cast<float>(triggerLabels().size() - 1), 0.f, false,
                 triggerLabels()},
                {"threshold", "Threshold", "dB", -60.f, 0.f, -24.f},
                {"depth", "Depth", "%", 0.f, 100.f, 100.f},
                {"sync", "Sync", "", 0.f, 1.f, 0.f, false, kOnOff},
                {"length", "Length", "ms", 10.f, 2000.f, 250.f, true},
                {"rate", "Length (Synced)", "", 0.f, static_cast<float>(rateLabels().size() - 1), 4.f, false,
                 rateLabels()},
                {"smooth", "Smooth", "ms", 0.f, 30.f, 1.f},
                {"lookahead", "Lookahead", "ms", 0.f, static_cast<float>(kMaxLookaheadMs), 0.f},
                {"range", "Range", "", 0.f, 1.f, 0.f, false, kRanges},
                {"crossover", "Crossover", "Hz", 30.f, 1000.f, 150.f, true},
                {"autofit", "Auto Fit", "", 0.f, 1.f, 0.f, false, kOnOff},
                {"character", "Fit Character", "", 0.f, 2.f, 1.f, false, kCharacters},
            };
            list[Lookahead].automatable = false;  // (it is the device's latency)
            for (const int hidden : {AutoFit, Character}) {
                list[static_cast<size_t>(hidden)].automatable = false;
                list[static_cast<size_t>(hidden)].hidden = true;
            }
            for (int p = 0; p < kPoints; ++p) {
                const std::string id = "p" + std::to_string(p + 1) + "_";
                const std::string name = "Point " + std::to_string(p + 1) + " ";
                const bool isDefault = p < static_cast<int>(std::size(kDefaultCurve));
                const DefaultPoint d = isDefault ? kDefaultCurve[p] : DefaultPoint{1.f, 1.f, 0.f};
                ParamInfo used{id + "used", name + "Used", "", 0.f, 1.f, isDefault ? 1.f : 0.f, false, kOnOff};
                ParamInfo x{id + "x", name + "Time", "", 0.f, 1.f, d.x};
                ParamInfo y{id + "y", name + "Level", "", 0.f, 1.f, d.y};
                ParamInfo curve{id + "curve", name + "Curve", "", -1.f, 1.f, d.curve};
                for (ParamInfo* info : {&used, &x, &y, &curve}) {
                    info->automatable = false;
                    info->hidden = true;
                    list.push_back(*info);
                }
            }
            return list;
        }();
        return kInfos;
    }

    double sampleRate_ = 48000.0;
    int reportedLatency_ = 0;
    std::array<Point, kPoints> points_{};
    int numPoints_ = 0;
    std::vector<float> delay_[2];
    int writeAt_ = 0;
    eq::Biquad split_[2];                     // the low and high pass (Butterworth)
    double splitState_[2][2][2][2] = {};      // per channel, band and section: the TDF-II's two
    float crossover_ = -1.f;
    int64_t sinceHit_ = -1;                   // samples since the latest hit; -1: its curve is over
    bool armed_ = true;
    int sinceKeyHit_ = 1 << 30;
    float keyEnvelope_ = 0.f;
    int64_t sinceBeatHit_ = int64_t{1} << 40;
    float gain_ = -1.f;
};

}  // namespace

SUB_REGISTER_BUILTIN(SidechainProcessor, AudioEffect);

}  // namespace sub
