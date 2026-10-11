// The built-in Saturator: each curve plays as its editor draws it (the same
// functions), with the numbers its formulas give; the Waveshaper's controls do
// what Ableton's do; saturation adds odd harmonics only; Post Clip holds the
// output to the Output level at any Dry/Wet; Output and Dry/Wet; Color leaves a
// clean sound alone (also while it glides) and moves the saturation; DC;
// Hi-Quality's aliasing (the curves' and Post Clip's), latency and pre-roll;
// every control changes without a click;
// automation through the engine to the sample; reset and a new sample rate;
// silence rings out to exact zeros; the tail covers the ringing; extremes stay
// finite, and NaN or infinity in the input is silence; one channel; the
// displays.

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "Engine.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/SaturatorDesign.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"
#include "harness/Standalone.h"

using namespace subtest;
namespace saturator = sub::saturator;

namespace {

constexpr int kBlock = 1024;  // the renderer's largest block (Renderer::kMaxBlock)
constexpr int kHqLatency = 36;
constexpr int kHqPreroll = 80;
constexpr int kFade = kSampleRate / 100;  // the lists' and switches' 10 ms

using Values = ParamValues;
using Change = ParamChange;

// A Saturator on its own, outside an engine (harness/Standalone.h).
struct Saturator : Standalone {
    explicit Saturator(double rate = kSampleRate, const Values& values = {}) : Standalone("saturator", rate, values) {}
};

Samples play(const Values& values, const Samples& in, const std::vector<Change>& changes = {},
             double rate = kSampleRate) {
    Saturator s(rate, values);
    return s.play(in, changes);
}

// Each level held `hold` samples.
Samples staircase(const std::vector<float>& levels, int hold = 64) {
    Samples x;
    for (const float level : levels) x.insert(x.end(), static_cast<size_t>(hold), level);
    return x;
}

// 41 levels from -1.5 to 1.5.
std::vector<float> levels41() {
    std::vector<float> levels;
    for (int k = 0; k <= 40; ++k) levels.push_back(-1.5f + 3.f * static_cast<float>(k) / 40.f);
    return levels;
}

// A slow straight sweep, for counting a curve's wiggles.
Samples ramp(float from, float to, int samples) {
    Samples x(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i)
        x[static_cast<size_t>(i)] = from + (to - from) * static_cast<float>(i) / static_cast<float>(samples - 1);
    return x;
}

// Harmonic `k` of `f0` relative to the fundamental, in dB, from a Hann-windowed spectrum.
double harmonicDbc(const Samples& y, double f0, int k, double rate = kSampleRate) {
    const std::vector<double> s = spectrum(y, hanning(y.size()));
    const auto level = [&](double f) {
        const auto centre = static_cast<int64_t>(std::lround(f * static_cast<double>(y.size()) / rate));
        double peak = 0.0;
        for (int64_t b = centre - 2; b <= centre + 2; ++b)
            if (b >= 0 && b < static_cast<int64_t>(s.size())) peak = std::max(peak, s[static_cast<size_t>(b)]);
        return peak;
    };
    return 20.0 * std::log10((level(k * f0) + 1e-30) / level(f0));
}

// The share (dB) of a periodic render's energy (a whole number of periods,
// each harmonic exactly on a bin) in bins below 20 kHz that aren't harmonics of
// `f0`: what folded back.
double aliasDb(const Samples& y, double f0, double rate = kSampleRate) {
    const std::vector<double> s = spectrum(y);
    double alias = 0.0, total = 0.0;
    for (size_t k = 1; k < s.size(); ++k) {
        const double f = static_cast<double>(k) * rate / static_cast<double>(y.size());
        const double e = s[k] * s[k];
        total += e;
        const double h = f / f0;
        if (f < 20000.0 && std::abs(h - std::round(h)) > 1e-6) alias += e;
    }
    return 10.0 * std::log10(alias / total + 1e-30);
}

// The output the editor draws for these settings at each input.
Samples transferOf(int type, float driveDb, const Samples& in, float thresholdDb = -18.f,
                   std::vector<float> ws = {50.f, 50.f, 50.f, 0.f, 0.f, 0.f}, int clip = 0) {
    const saturator::Shape shape = saturator::makeShape(type, thresholdDb, ws[0], ws[1], ws[2], ws[3], ws[4], ws[5]);
    const float g = sub::expDbToGain(driveDb);
    Samples out(in.size());
    for (size_t i = 0; i < in.size(); ++i) out[i] = saturator::transfer(shape, saturator::clipAt(clip), g, in[i]);
    return out;
}

Values wsValues(const std::vector<float>& ws) {
    return {{"ws_drive", ws[0]}, {"ws_lin", ws[1]},   {"ws_curve", ws[2]},
            {"ws_damp", ws[3]},  {"ws_depth", ws[4]}, {"ws_period", ws[5]}};
}

// The device's output for a constant input `x` (after it has settled).
float level(const Values& values, float x) {
    const Samples out = play(values, Samples(256, x));
    return out.back();
}

// A click at frame `at` of a track's left and/or right channel.
uint32_t clickTrack(sub::Engine& engine, float left, float right, int64_t at, double seconds = 1.0) {
    return stereoClickTrack(engine, left, right, at, seconds);
}

int64_t seconds(double s) { return static_cast<int64_t>(s * kSampleRate); }

// The most a signal held within ±1 at 4x can come out of the 4x path's filters
// (dsp::Oversampler::down at factor 4): the sum of the magnitudes of the
// weights one output sample takes from the 4x samples. Post Clip's ceiling
// holds at 4x; this is how far past it the band-limited output can go, at
// the very worst.
double downWorstGain() {
    constexpr int kFrames = 64, kLookedAt = 40;
    double sum = 0.0;
    for (int at = 0; at < 4 * kFrames; ++at) {
        sub::dsp::Oversampler os;
        os.prepare(kFrames, 2);
        os.setFactorLog2(2);
        const std::vector<float> silence(kFrames, 0.f);
        float* fast = os.up(silence.data(), kFrames);
        std::fill(fast, fast + 4 * kFrames, 0.f);
        fast[at] = 1.f;
        std::vector<float> out(kFrames);
        os.down(fast, kFrames, out.data());
        sum += std::abs(out[kLookedAt]);
    }
    return sum;
}

}  // namespace

TEST_CASE("the saturator is listed with its parameters") {
    const sub::BuiltinInfo info = builtinInfo("saturator");
    CHECK_EQ(info.name, std::string("Saturator"));
    CHECK(!info.isInstrument());
    CHECK(paramIds(info.params) ==
          (std::vector<std::string>{"drive", "type", "threshold", "output", "mix", "clip", "color", "base", "freq",
                                    "width", "depth", "dc", "hq", "ws_drive", "ws_lin", "ws_curve", "ws_damp",
                                    "ws_depth", "ws_period"}));
    struct Expected {
        const char* name;
        const char* unit;
        float min, max, def;
    };
    const std::vector<Expected> expected = {
        {"Drive", "dB", -36.f, 36.f, 0.f},
        {"Type", "", 0.f, 7.f, 0.f},
        {"Threshold", "dB", -50.f, 0.f, -18.f},
        {"Output", "dB", -36.f, 0.f, 0.f},
        {"Dry/Wet", "%", 0.f, 100.f, 100.f},
        {"Post Clip", "", 0.f, 2.f, 0.f},
        {"Color", "", 0.f, 1.f, 0.f},
        {"Color Amt Low", "dB", -36.f, 36.f, 0.f},  // (Live 12.1's names)
        {"Color Freq", "Hz", 30.f, 18500.f, 1000.f},
        {"Color Width", "%", 0.f, 100.f, 50.f},
        {"Color Amt Hi", "dB", -36.f, 36.f, 0.f},
        {"DC", "", 0.f, 1.f, 0.f},
        {"Hi-Quality", "", 0.f, 1.f, 0.f},
        {"WS Drive", "%", 0.f, 100.f, 50.f},
        {"WS Lin", "%", 0.f, 100.f, 50.f},
        {"WS Curve", "%", 0.f, 100.f, 50.f},
        {"WS Damp", "%", 0.f, 100.f, 0.f},
        {"WS Depth", "%", 0.f, 100.f, 0.f},
        {"WS Period", "%", 0.f, 100.f, 0.f},
    };
    REQUIRE(info.params.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        const sub::ParamInfo& p = info.params[i];
        INFO(p.id);
        CHECK_EQ(p.name, std::string(expected[i].name));
        CHECK_EQ(p.unit, std::string(expected[i].unit));
        CHECK_EQ(p.minValue, expected[i].min);
        CHECK_EQ(p.maxValue, expected[i].max);
        CHECK_EQ(p.defaultValue, expected[i].def);
        CHECK_EQ(p.logScale, p.id == "freq");
        CHECK_EQ(p.automatable, p.id != "hq");  // (Hi-Quality is the latency)
    }
    CHECK(info.params[1].valueLabels ==
          (std::vector<std::string>{"Analog Clip", "Soft Sine", "Bass Shaper", "Medium Curve", "Hard Curve",
                                    "Sinoid Fold", "Digital Clip", "Waveshaper"}));
    CHECK(info.params[5].valueLabels == (std::vector<std::string>{"No Clip", "Soft Clip", "Hard Clip"}));
    for (const size_t s : {6u, 11u, 12u}) CHECK(info.params[s].valueLabels == (std::vector<std::string>{"Off", "On"}));
    CHECK_EQ(info.params[1].stepCount(), 7);
    CHECK_EQ(info.params[5].stepCount(), 2);

    Saturator s;
    const std::vector<sub::DisplayInfo> displays = s.processor().displays();
    REQUIRE(displays.size() == 4u);
    const std::vector<std::pair<std::string, int>> want = {
        {"in_peak", 128}, {"out_peak", 128}, {"input", 1}, {"output", 1}};
    for (size_t i = 0; i < want.size(); ++i) {
        CHECK_EQ(displays[i].id, want[i].first);
        CHECK_EQ(displays[i].samplesPerValue, want[i].second);
    }
    CHECK_EQ(s.processor().latencySamples(), 0);
    CHECK_EQ(s.processor().tailSamples(), 0);
    CHECK_EQ(s.processor().name(), std::string("Saturator"));
    CHECK_EQ(s.processor().typeId(), std::string("builtin:saturator"));
}

TEST_CASE("by default the saturator passes quiet audio through untouched") {
    // Analog Clip is straight up to 0.75 (-2.5 dBFS): a 0.7 sine comes out bit for bit.
    const Samples left = sine(1000.0, 1.0, 0.7), right = sine(1300.0, 1.0, -0.7);
    Saturator s;
    Samples l = left, r = right;
    s.run({&l, &r});
    CHECK_ARRAY_EQUAL(l, left);
    CHECK_ARRAY_EQUAL(r, right);
}

TEST_CASE("each saturator curve plays as its editor draws it") {
    const Samples in = staircase(levels41());
    const std::vector<float> driven = {70.f, 70.f, 70.f, 70.f, 70.f, 70.f};
    for (int type = 0; type < saturator::kTypes; ++type) {
        for (const float drive : {-6.f, 0.f, 6.f, 18.f}) {
            std::vector<float> thresholds = {-18.f};
            if (type == 2) thresholds = {-50.f, -18.f, 0.f};
            std::vector<std::vector<float>> wsSets = {{50.f, 50.f, 50.f, 0.f, 0.f, 0.f}};
            if (type == 7) wsSets.push_back(driven);
            for (const float threshold : thresholds) {
                for (const std::vector<float>& ws : wsSets) {
                    INFO(saturator::typeLabels()[static_cast<size_t>(type)] + " at " + std::to_string(drive) + " dB, " +
                         std::to_string(threshold) + " dB, WS " + std::to_string(ws[0]));
                    const Values values =
                        Values{{"type", static_cast<float>(type)}, {"drive", drive}, {"threshold", threshold}} +
                        wsValues(ws);
                    CHECK_ALLCLOSE(play(values, in), transferOf(type, drive, in, threshold, ws), 1e-6, 1e-6);
                }
            }
        }
    }
}

TEST_CASE("the saturator's curves' numbers") {
    const auto at = [](int type, float x, Values more = {}) {
        return level(Values{{"type", static_cast<float>(type)}} + more, x);
    };
    // Analog Clip: straight to 0.75, a rounded corner to 1 at 1.25.
    CHECK_EQ(at(0, 0.5f), 0.5f);
    CHECK_APPROX(at(0, 1.f), 0.9375);
    CHECK_EQ(at(0, 2.f), 1.f);
    CHECK_EQ(at(0, -2.f), -1.f);
    // Soft Sine: sin(pi/2 u), a gain of pi/2 for quiet input, flat past full scale.
    CHECK_APPROX_TOL(at(1, 0.5f), 0.707107, 0.0, 1e-6);
    CHECK_APPROX_REL(at(1, 0.001f) / 0.001, kPi / 2.0, 0.001);
    CHECK_EQ(at(1, 1.2f), 1.f);
    // Medium Curve: tanh.
    CHECK_APPROX_TOL(at(3, 0.5f), 0.462117, 0.0, 1e-6);
    CHECK_APPROX_TOL(at(3, 1.f), 0.761594, 0.0, 1e-6);
    // Hard Curve: u - 4/27 u³ to 1 at 1.5.
    CHECK_APPROX_TOL(at(4, 0.5f), 0.481481, 0.0, 1e-6);
    CHECK_APPROX_TOL(at(4, 1.f), 0.851852, 0.0, 1e-6);
    CHECK_EQ(at(4, 1.6f), 1.f);
    // Sinoid Fold: up to 1 at full scale, then folding back: 0 at twice it, -1 at three times.
    CHECK_APPROX_TOL(at(5, 1.f), 1.0, 0.0, 1e-6);
    CHECK(std::abs(at(5, 1.f, {{"drive", 6.0206f}})) < 1e-5);
    CHECK_APPROX_TOL(at(5, 1.5f, {{"drive", 6.0206f}}), -1.0, 0.0, 1e-5);
    // Digital Clip: a hard clip at 1.
    {
        const Samples out = play({{"type", 6.f}, {"drive", 12.f}}, sine(1000.0, 0.5, 0.5));
        CHECK_EQ(maxAbs(out), 1.0);
        const size_t clipped =
            static_cast<size_t>(std::count_if(out.begin(), out.end(), [](float v) { return std::abs(v) == 1.f; }));
        CHECK(clipped > out.size() * 3 / 10);
    }
    // Bass Shaper: straight below the threshold, a tanh above it.
    CHECK_EQ(at(2, 0.05f, {{"threshold", -20.f}}), 0.05f);
    CHECK_APPROX_TOL(at(2, 1.f, {{"threshold", -20.f}}), 0.785435, 0.0, 2e-6);
    {
        const Samples in = staircase(levels41());
        // At 0 dB a hard clip; at -50 dB all but a tanh.
        CHECK_ARRAY_EQUAL(play({{"type", 2.f}, {"threshold", 0.f}}, in), play({{"type", 6.f}}, in));
        const Samples soft = play({{"type", 2.f}, {"threshold", -50.f}}, in);
        double worst = 0.0;
        for (size_t i = 0; i < in.size(); ++i)
            worst = std::max(worst, static_cast<double>(std::abs(soft[i] - sub::dsp::fastTanh(in[i]))));
        CHECK(worst < 0.004);
    }
    // Waveshaper at its defaults.
    CHECK_APPROX_TOL(at(7, 0.5f), 0.527300, 0.0, 1e-6);
    CHECK_APPROX_TOL(at(7, 1.f), 0.982014, 0.0, 1e-6);

    // The fast sine the curves use, against std::sin, near and far.
    double worst = 0.0;
    for (int i = -20000; i <= 20000; ++i) {
        const float x = static_cast<float>(i) * 0.0123f;
        worst = std::max(worst, std::abs(saturator::sine(x) - std::sin(static_cast<double>(x))));
    }
    CHECK(worst < 3e-7);
    CHECK_EQ(saturator::sine(0.f), 0.f);
}

TEST_CASE("the saturator's Waveshaper controls do what Ableton's do") {
    const Values shaper = {{"type", 7.f}, {"ws_drive", 100.f}};
    // WS Drive 0: no effect at all, whatever the others.
    {
        const Samples in = noise(4800, 1, 1.5f);
        const Values values = Values{{"type", 7.f}} + wsValues({0.f, 70.f, 90.f, 60.f, 80.f, 30.f});
        CHECK_ARRAY_EQUAL(play(values, in), in);
    }
    // Lin sets the straight part's slope: 25 % halves it (Curve and Depth at 0).
    CHECK_APPROX_REL(level(shaper + Values{{"ws_lin", 25.f}, {"ws_curve", 0.f}}, 0.001f) / 0.001, 0.5, 0.005);
    // Curve adds a third harmonic.
    {
        const Samples in = sine(1000.0, 1.0, 0.3);
        const double without = harmonicDbc(play(shaper + Values{{"ws_curve", 0.f}}, in), 1000.0, 3);
        const double with = harmonicDbc(play(shaper + Values{{"ws_curve", 100.f}}, in), 1000.0, 3);
        INFO(std::to_string(without) + " dBc, then " + std::to_string(with));
        CHECK(with > without + 10.0);
    }
    // Damp gates what is near silence, and leaves loud sound alone.
    {
        const auto rmsDb = [&](float damp, float amplitude) {
            return 20.0 * std::log10(rms(play(shaper + Values{{"ws_damp", damp}}, sine(1000.0, 0.5, amplitude))));
        };
        CHECK(rmsDb(100.f, 0.01f) < rmsDb(0.f, 0.01f) - 20.0);
        CHECK(std::abs(rmsDb(100.f, 0.8f) - rmsDb(0.f, 0.8f)) < 1.0);
    }
    // Depth lays a sine over the curve; Period makes its ripples denser.
    {
        const auto extrema = [&](float period) {
            const Samples out =
                play(shaper + Values{{"ws_depth", 100.f}, {"ws_period", period}}, ramp(-1.f, 1.f, 8000));
            int count = 0, last = 0;
            for (size_t i = 1; i < out.size(); ++i) {
                const float d = out[i] - out[i - 1];
                const int sign = d > 0.f ? 1 : (d < 0.f ? -1 : 0);
                if (sign != 0 && last != 0 && sign != last) ++count;
                if (sign != 0) last = sign;
            }
            return count;
        };
        CHECK_EQ(extrema(0.f), 0);
        CHECK(extrema(100.f) >= 30);
    }
}

TEST_CASE("the saturator adds odd harmonics only") {
    const Samples out = play({{"type", 3.f}}, sine(1000.0, 1.0, 1.0));
    const std::vector<double> s = spectrum(out);  // a whole number of periods: every harmonic on its bin
    const auto dbc = [&](int k) { return 20.0 * std::log10(s[static_cast<size_t>(1000 * k)] / s[1000] + 1e-30); };
    CHECK(dbc(3) > -40.0);
    CHECK(dbc(5) > -70.0);
    CHECK(dbc(2) < -100.0);
    CHECK(dbc(4) < -100.0);
    CHECK(std::abs(mean(out)) < 1e-6);
}

TEST_CASE("the saturator's Post Clip holds the output to the Output level") {
    const Samples in = sine(1000.0, 0.5, 1.0);
    const Values loud = Values{{"type", 7.f}, {"drive", 12.f}};  // (the Waveshaper at WS Drive 50 %: unbounded)
    const Samples none = play(loud, in), soft = play(loud + Values{{"clip", 1.f}}, in),
                  hard = play(loud + Values{{"clip", 2.f}}, in);
    CHECK(maxAbs(none) > 1.5);
    CHECK(maxAbs(hard) <= 1.0);
    CHECK_EQ(maxAbs(hard), 1.0);
    CHECK(maxAbs(soft) <= 1.0);
    CHECK(clickiness(soft) < clickiness(hard));
    const Samples quieter = play(loud + Values{{"clip", 2.f}, {"output", -6.f}}, in);
    CHECK(maxAbs(quieter) <= 0.501188);
    CHECK_ALLCLOSE(transferOf(7, 12.f, in, -18.f, {50.f, 50.f, 50.f, 0.f, 0.f, 0.f}, 2), hard, 1e-6, 1e-6);

    // At any Dry/Wet: Post Clip holds the blend, the dry sound in it too (here a hot input, 2.0).
    const Samples hot = sine(100.0, 0.5, 2.0);
    for (const float mix : {100.f, 50.f, 0.f}) {
        INFO(std::to_string(mix) + " %");
        const Values blend = {{"type", 7.f}, {"drive", 6.f}, {"mix", mix}};  // (the Waveshaper: unbounded)
        CHECK(maxAbs(play(blend, hot)) > 1.5);
        CHECK(maxAbs(play(blend + Values{{"clip", 1.f}}, hot)) <= 1.0);
        CHECK(maxAbs(play(blend + Values{{"clip", 2.f}}, hot)) <= 1.0);
        CHECK(maxAbs(play(blend + Values{{"clip", 2.f}, {"output", -6.f}}, hot)) <= 0.501188);
    }
    // Fully dry, sound under the knee comes through Soft Clip untouched (the Analog Clip curve is straight there).
    const Samples underKnee = sine(1000.0, 0.5, 0.7);
    CHECK_ARRAY_EQUAL(play({{"drive", 24.f}, {"mix", 0.f}, {"clip", 1.f}}, underKnee), underKnee);
}

TEST_CASE("the saturator's Output and Dry/Wet") {
    const Samples in = sine(1000.0, 0.5, 0.5);
    CHECK_ARRAY_EQUAL(play({}, in), in);  // (the default is straight at this level)
    Samples half = in;
    for (float& v : half) v *= 0.5f;
    CHECK_ALLCLOSE(play({{"output", -6.0206f}}, in), half, 1e-5, 1e-9);
    // Fully dry: the input, times Output.
    const Values hot = {{"type", 6.f}, {"drive", 12.f}};
    CHECK_ARRAY_EQUAL(play(hot + Values{{"mix", 0.f}}, in), in);
    const Samples dryQuieter = play(hot + Values{{"mix", 0.f}, {"output", -12.f}}, in);
    Samples want = in;
    for (float& v : want) v *= sub::expDbToGain(-12.f);
    CHECK_ALLCLOSE(dryQuieter, want, 1e-6, 0.0);
    // Half: the average of the input and the saturated sound.
    const Samples wet = play(hot, in), blend = play(hot + Values{{"mix", 50.f}}, in);
    Samples average(in.size());
    for (size_t i = 0; i < in.size(); ++i) average[i] = 0.5f * (in[i] + wet[i]);
    CHECK_ALLCLOSE(blend, average, 0.0, 1e-6);
}

TEST_CASE("the saturator's Color leaves clean sound alone and moves the saturation") {
    // (a) Where the curve is straight, emphasis and de-emphasis cancel.
    const Samples quiet = noise(kSampleRate, 2, 0.05f);
    const Values colored = {{"color", 1.f}, {"base", 12.f}, {"depth", 12.f}, {"freq", 2000.f}};
    CHECK_ALLCLOSE(slice(play(colored, quiet), kSampleRate / 10), slice(quiet, kSampleRate / 10), 0.0, 1e-5);

    // (b) Saturated, a band boosted before the curve comes out with less in it; one cut, with more.
    const Samples loud = noise(kSampleRate, 3, 0.5f);
    const Values clip = {{"type", 6.f}, {"drive", 24.f}};
    const Samples plain = slice(play(clip, loud), kSampleRate / 4);
    const auto bandChange = [&](const Values& color, double lo, double hi) {
        const Samples out = slice(play(clip + Values{{"color", 1.f}} + color, loud), kSampleRate / 4);
        return bandDb(out, lo, hi) - bandDb(plain, lo, hi);
    };
    const double depthUp = bandChange({{"depth", 24.f}, {"freq", 2000.f}}, 1700.0, 2300.0);
    const double depthDown = bandChange({{"depth", -24.f}, {"freq", 2000.f}}, 1700.0, 2300.0);
    const double baseUp = bandChange({{"base", 24.f}}, 20.0, 100.0);
    const double baseDown = bandChange({{"base", -24.f}}, 20.0, 100.0);
    INFO("depth +24: " + std::to_string(depthUp) + ", -24: " + std::to_string(depthDown) +
         "; base +24: " + std::to_string(baseUp) + ", -24: " + std::to_string(baseDown) + " dB");
    CHECK(depthUp <= -6.0);
    CHECK(depthDown >= 10.0);
    CHECK(baseUp <= -3.0);
    CHECK(baseDown >= 10.0);

    // (c) The de-emphasis is the emphasis's exact inverse, at every frequency.
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        const saturator::ColorDesign d = saturator::colorDesign(-30.0, 3000.0, 20.0, 33.0, rate);
        const sub::dsp::BiquadCoefficients invShelf = saturator::inverse(d.shelf), invPeak = saturator::inverse(d.peak);
        for (int k = 0; k < 200; ++k) {
            const double f = 10.0 * std::pow(0.45 * rate / 10.0, k / 199.0);
            const double sum =
                saturator::colorResponseDb(d, f, rate) + invShelf.magnitudeDb(f, rate) + invPeak.magnitudeDb(f, rate);
            CHECK(std::abs(sum) < 1e-9);
        }
    }
    CHECK_APPROX_TOL(saturator::widthToQ(0.0), 5.7667, 1e-3, 0.0);
    CHECK_APPROX_TOL(saturator::widthToQ(50.0), 1.4142, 1e-3, 0.0);
    CHECK_APPROX_TOL(saturator::widthToQ(100.0), 0.2667, 1e-3, 0.0);

    // (d) While its controls glide, a clean sound stays exactly clean: the
    // de-emphasis's states are rescaled at each redesign (without that, an
    // 18 dB glide leaves an error of about 5e-5 on a 0.05 signal).
    const Values start = {{"color", 1.f}, {"depth", 12.f}, {"freq", 100.f}, {"width", 0.f}};
    const std::vector<Change> glides = {
        {seconds(0.15), "base", 24.f},  {seconds(0.15), "freq", 5000.f}, {seconds(0.3), "base", -24.f},
        {seconds(0.3), "freq", 300.f},  {seconds(0.3), "width", 100.f},  {seconds(0.45), "base", 0.f},
        {seconds(0.5), "depth", -18.f}, {seconds(0.6), "width", 30.f},   {seconds(0.65), "freq", 12000.f},
        {seconds(0.75), "depth", 0.f},  {seconds(0.8), "color", 0.f},
    };
    const Samples out = play(start, quiet, glides);
    CHECK_ALLCLOSE(out, quiet, 0.0, 2e-5);

    // The same with Hi-Quality (Color's filters at 4x, designed for that rate),
    // against the 4x path without Color.
    const Samples through = play({{"hq", 1.f}}, quiet);
    const Samples hqOut = play(start + Values{{"hq", 1.f}}, quiet, glides);
    CHECK_ALLCLOSE(hqOut, through, 0.0, 2e-5);

    // Hi-Quality switched off while Color glides on and on (Amt Lo and Amt Hi
    // jumping every 150 ms, as stepped automation or a knob kept moving): once
    // the fade is done, the 1x path is clean at once.
    {
        Samples twoTone = sine(200.0, 2.0, 0.025);
        const Samples high = sine(3000.0, 2.0, 0.025);
        for (size_t i = 0; i < twoTone.size(); ++i) twoTone[i] += high[i];
        std::vector<Change> steps;
        for (int k = 0; k < 13; ++k) {
            steps.push_back({seconds(0.15 * k), "base", k % 2 ? 24.f : -24.f});
            steps.push_back({seconds(0.15 * k), "depth", k % 2 ? -18.f : 18.f});
        }
        const int64_t off = seconds(0.51);
        steps.push_back({off, "hq", 0.f, true});
        std::sort(steps.begin(), steps.end(), [](const Change& a, const Change& b) { return a.frame < b.frame; });
        const Values linear = {{"drive", -12.f}, {"color", 1.f}, {"freq", 300.f}, {"hq", 1.f}};
        const Samples out = play(linear, twoTone, steps);
        Samples want = twoTone;
        for (float& v : want) v *= sub::expDbToGain(-12.f);
        CHECK_ALLCLOSE(slice(out, off + kFade), slice(want, off + kFade), 0.0, 1e-6);
    }

    // (e) On at 0 dB, Color is switched out: bit for bit as off.
    const Values shaper = {{"type", 7.f}, {"drive", 12.f}};
    CHECK_ARRAY_EQUAL(play(shaper + Values{{"color", 1.f}, {"freq", 300.f}}, loud), play(shaper, loud));
}

TEST_CASE("the saturator's DC removes an offset") {
    Samples in = sine(100.0, 2.0, 0.2);
    for (float& v : in) v += 0.3f;
    const int64_t last = seconds(1.5);
    CHECK(std::abs(mean(slice(play({{"dc", 1.f}}, in), last))) < 1e-3);
    CHECK_APPROX_TOL(mean(slice(play({}, in), last)), 0.3, 0.0, 1e-3);
}

TEST_CASE("the saturator's Hi-Quality oversamples and reports its latency") {
    // (a) Its latency, and the engine told when it changes.
    {
        Saturator s;
        CHECK(!s.processor().idle());
        s.set("hq", 1.f);
        CHECK_EQ(s.processor().latencySamples(), kHqLatency);
        CHECK(s.processor().idle());
        CHECK(!s.processor().idle());
        s.set("hq", 0.f);
        CHECK_EQ(s.processor().latencySamples(), 0);
        CHECK(s.processor().idle());
        CHECK(!s.processor().idle());
        CHECK_EQ(sub::dsp::Oversampler::latencyFor(2), kHqLatency);
    }
    // (b) Where the curve is straight, the input 36 samples late.
    {
        const Samples in = sine(1000.0, 0.5, 0.25);
        const Samples out = play({{"hq", 1.f}}, in);
        CHECK_ALLCLOSE(slice(out, seconds(0.1)), slice(in, seconds(0.1) - kHqLatency, -kHqLatency), 0.0, 1e-3);
    }
    // (c) What folds back below Nyquist, far less.
    const auto alias = [](const Values& values, double freq) {
        const Samples in = sine(freq, 1.1, 0.5);
        return aliasDb(slice(play(values, in), seconds(0.1), seconds(1.1)), freq);
    };
    {
        const Values medium = {{"type", 3.f}, {"drive", 18.f}};
        const double plain = alias(medium, 5000.0), hq = alias(medium + Values{{"hq", 1.f}}, 5000.0);
        INFO("Medium Curve: " + std::to_string(plain) + " dB, with Hi-Quality " + std::to_string(hq));
        CHECK(hq < plain - 40.0);
    }
    {
        const Values digital = {{"type", 6.f}, {"drive", 24.f}};
        const double plain = alias(digital, 7100.0), hq = alias(digital + Values{{"hq", 1.f}}, 7100.0);
        INFO("Digital Clip: " + std::to_string(plain) + " dB, with Hi-Quality " + std::to_string(hq));
        CHECK(hq < plain - 20.0);
    }
    // Post Clip runs at 4x too (with Color's de-emphasis and Dry/Wet before it): Soft
    // Clip after a curve, Hard Clip after the unbounded Waveshaper or a band cut by Color.
    // (Measured 35, 36, 33, 36, 24 and 44 dB; at the base rate it was 1 to 4 dB.)
    {
        const std::vector<std::pair<Values, double>> clipped = {
            {{{"drive", 12.f}, {"clip", 1.f}}, 30.0},
            {{{"type", 4.f}, {"drive", 12.f}, {"clip", 1.f}}, 30.0},
            {{{"type", 7.f}, {"drive", 18.f}, {"clip", 2.f}}, 25.0},
            {{{"type", 7.f}, {"drive", 18.f}, {"clip", 1.f}}, 30.0},
            {{{"type", 3.f}, {"drive", 12.f}, {"color", 1.f}, {"depth", -12.f}, {"freq", 5000.f}, {"clip", 2.f}}, 18.0},
            {{{"type", 3.f}, {"drive", 12.f}, {"color", 1.f}, {"depth", -12.f}, {"freq", 5000.f}, {"clip", 1.f}}, 30.0},
        };
        for (const auto& [values, better] : clipped) {
            const double plain = alias(values, 5000.0), hq = alias(values + Values{{"hq", 1.f}}, 5000.0);
            INFO("Post Clip: " + std::to_string(plain) + " dB, with Hi-Quality " + std::to_string(hq));
            CHECK(hq < plain - better);
        }
    }
    // (d) Through the engine: the other tracks wait for it.
    {
        sub::Engine engine;
        engine.setClipFadeMs(0);
        constexpr int64_t kClick = 1000;
        clickTrack(engine, 0.5f, 0.f, kClick);
        const uint32_t wet = clickTrack(engine, 0.f, 0.5f, kClick);
        const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(wet), "saturator", -1);
        setParam(engine, id, "hq", 1.f);
        engine.idle();
        CHECK_EQ(engine.processorInfo(id).latency, kHqLatency);
        const Samples out = engine.renderOffline(0.0, kSampleRate / 2);
        const Samples left = channel(out, 0), right = channel(out, 1);
        std::vector<float> leftAbs(left.size()), rightAbs(right.size());
        std::transform(left.begin(), left.end(), leftAbs.begin(), [](float v) { return std::abs(v); });
        std::transform(right.begin(), right.end(), rightAbs.begin(), [](float v) { return std::abs(v); });
        CHECK_EQ(argmax(leftAbs), argmax(rightAbs));
        CHECK_EQ(nonzero(left).size(), 1u);  // (the dry click, as it was)
        // The wet one band-limited by the 4x path's filters, its peak where the dry click is.
        Saturator alone(kSampleRate, {{"hq", 1.f}});
        const Samples response = alone.play(impulse(256, 0, left[argmax(leftAbs)]));
        CHECK_EQ(argmax(response), static_cast<size_t>(kHqLatency));
        CHECK_APPROX_TOL(right[argmax(rightAbs)], response[kHqLatency], 1e-5, 0.0);
    }
    // (e) Switched on, its 4x path runs unheard for 80 samples first: once
    // faded in it plays exactly as if it had been on all along.
    {
        const Samples tone = smoothSine(221.25, 1.0, kSampleRate, 0.3);
        const Values medium = {{"type", 3.f}, {"drive", 12.f}};
        const int64_t at = kSampleRate / 2;
        const Samples switched = play(medium, tone, {{at, "hq", 1.f, true}});
        const Samples always = play(medium + Values{{"hq", 1.f}}, tone);
        const Samples never = play(medium, tone);
        CHECK_ARRAY_EQUAL(slice(switched, 0, at), slice(never, 0, at));
        CHECK_ARRAY_EQUAL(slice(switched, at + kHqPreroll + kFade), slice(always, at + kHqPreroll + kFade));
        // Switched off: the 1x path, at once exact once faded.
        Saturator s(kSampleRate, medium + Values{{"hq", 1.f}});
        const Samples back = s.play(tone, {{at, "hq", 0.f, true}});
        CHECK_ARRAY_EQUAL(slice(back, at + kFade), slice(never, at + kFade));
    }
}

TEST_CASE("changing any saturator control is click-free") {
    // A tone at 221.25 Hz with every control jumping in turn, as automation's
    // steps make it, to a setting that sounds different: at 0.2 s, 0.6 s, ...
    // (each a peak or a trough of the tone, where a step is largest). Around
    // each change the output must be no clickier than twice the settings it
    // passes through (either side, and those a glide crosses on its way), or
    // -50 dB under the tone, where splicing the two sides without a fade
    // measures at least 10 times that (a hard clip's own corners measure
    // within 30 times a splice).
    struct Sequence {
        std::string name;
        Values start;
        std::vector<Values> steps;
        bool direct = false;  // set between blocks (not automatable)
        float dc = 0.f;       // an offset added to the tone
    };
    const std::vector<Sequence> sequences = {
        {"drive", {{"drive", -36.f}}, {{{"drive", 12.f}}, {{"drive", 0.f}}}},
        {"output", {}, {{{"output", -24.f}}, {{"output", 0.f}}}},
        {"mix", {{"drive", 12.f}}, {{{"mix", 0.f}}, {{"mix", 60.f}}, {{"mix", 100.f}}}},
        {"type",
         {{"drive", 12.f}, {"type", 3.f}},
         {{{"type", 1.f}}, {{"type", 5.f}}, {{"type", 7.f}}, {{"type", 2.f}}, {{"type", 3.f}}}},
        {"clip",
         {{"drive", 18.f}, {"type", 7.f}},
         {{{"clip", 1.f}}, {{"clip", 0.f}}, {{"clip", 2.f}}, {{"clip", 0.f}}}},
        {"color", {{"drive", 12.f}, {"base", 18.f}, {"depth", -12.f}}, {{{"color", 1.f}}, {{"color", 0.f}}}},
        {"freq and width",
         {{"drive", 12.f}, {"color", 1.f}, {"depth", -12.f}, {"freq", 300.f}, {"width", 0.f}},
         {{{"freq", 3000.f}}, {{"width", 100.f}}, {{"freq", 300.f}}, {{"width", 0.f}}}},
        {"base and depth",
         {{"drive", 12.f}, {"color", 1.f}, {"freq", 2000.f}},
         {{{"base", 24.f}}, {{"base", -24.f}}, {{"base", 0.f}}, {{"depth", -24.f}}, {{"depth", 24.f}}}},
        {"dc", {}, {{{"dc", 1.f}}, {{"dc", 0.f}}}, false, 0.2f},
        {"threshold",
         {{"drive", 12.f}, {"type", 2.f}, {"threshold", -50.f}},
         {{{"threshold", -6.f}}, {{"threshold", -30.f}}}},
        {"ws_drive",
         {{"drive", 6.f}, {"type", 7.f}},
         {{{"ws_drive", 0.f}}, {{"ws_drive", 100.f}}, {{"ws_drive", 50.f}}}},
        {"ws_lin", {{"drive", 6.f}, {"type", 7.f}}, {{{"ws_lin", 0.f}}, {{"ws_lin", 100.f}}, {{"ws_lin", 50.f}}}},
        {"ws_curve",
         {{"drive", 6.f}, {"type", 7.f}},
         {{{"ws_curve", 0.f}}, {{"ws_curve", 100.f}}, {{"ws_curve", 50.f}}}},
        {"ws_damp", {{"type", 7.f}, {"ws_damp", 50.f}}, {{{"ws_damp", 100.f}}, {{"ws_damp", 50.f}}}},
        {"ws_depth", {{"drive", 6.f}, {"type", 7.f}}, {{{"ws_depth", 100.f}}, {{"ws_depth", 0.f}}}},
        {"ws_period", {{"type", 7.f}, {"ws_depth", 50.f}}, {{{"ws_period", 30.f}}, {{"ws_period", 0.f}}}},
        {"hq", {{"drive", 12.f}, {"type", 3.f}}, {{{"hq", 1.f}}, {{"hq", 0.f}}}, true},
        // (each way the incoming path's Color filters start from silence)
        {"hq with color",
         {{"drive", 12.f}, {"type", 3.f}, {"color", 1.f}, {"base", 18.f}, {"depth", -12.f}, {"freq", 2000.f}},
         {{{"hq", 1.f}}, {{"hq", 0.f}}},
         true},
        {"hq with post clip",
         {{"drive", 12.f}, {"type", 3.f}, {"clip", 1.f}, {"mix", 70.f}},
         {{{"hq", 1.f}}, {{"hq", 0.f}}},
         true},
    };
    const std::vector<sub::ParamInfo> params = builtinInfo("saturator").params;
    const auto valueOf = [&](const Values& values, const std::string& id) {
        float value = 0.f;
        for (const sub::ParamInfo& p : params)
            if (p.id == id) value = p.defaultValue;
        for (const auto& [name, v] : values)
            if (name == id) value = v;
        return value;
    };
    // The settings `f` of the way from `before` to `after`, as the glides move
    // (dB and percent straight, Frequency in log, Color by its gains); none
    // across a list's crossfade.
    const auto partway = [&](const Values& before, const Values& step, float f) -> std::vector<Values> {
        Values values = before;
        for (const auto& [id, to] : step) {
            const float from = valueOf(before, id);
            if (id == "color") {
                const float share = to > 0.5f ? f : 1.f - f;
                values.push_back({"color", 1.f});
                values.push_back({"base", share * valueOf(before, "base")});
                values.push_back({"depth", share * valueOf(before, "depth")});
            } else if (id == "type" || id == "clip" || id == "dc" || id == "hq") {
                return {};
            } else if (id == "freq") {
                values.push_back({id, static_cast<float>(from * std::pow(to / from, f))});
            } else {
                values.push_back({id, from + (to - from) * f});
            }
        }
        return {values};
    };
    for (const Sequence& sequence : sequences) {
        INFO(sequence.name);
        const auto changeAt = [](size_t k) { return seconds(0.2 + 0.4 * static_cast<double>(k)); };
        const double length = 0.4 * static_cast<double>(sequence.steps.size()) + 0.2;
        Samples tone = smoothSine(221.25, length, kSampleRate, 0.3);
        for (float& v : tone) v += sequence.dc;
        std::vector<Change> changes;
        Values settings = sequence.start;
        std::vector<Values> steady = {settings};
        for (size_t k = 0; k < sequence.steps.size(); ++k) {
            for (const auto& [id, value] : sequence.steps[k]) {
                changes.push_back({changeAt(k), id, value, sequence.direct});
                settings.push_back({id, value});
            }
            steady.push_back(settings);
        }
        const Samples out = play(sequence.start, tone, changes);
        CHECK(allFinite(out));
        for (size_t k = 0; k < sequence.steps.size(); ++k) {
            INFO("change " + std::to_string(k + 1));
            const int64_t at = changeAt(k);
            const int64_t from = at - seconds(0.01), to = at + seconds(0.06);
            const Samples before = play(steady[k], tone), after = play(steady[k + 1], tone);
            double s = std::max(clickiness(before, from, to), clickiness(after, from, to));
            for (const float f : {0.1f, 0.25f, 0.5f, 0.75f, 0.9f})
                for (const Values& values : partway(steady[k], sequence.steps[k], f))
                    s = std::max(s, clickiness(play(values, tone), from, to));
            const double bound = std::max(2.0 * s, 1e-3);
            const double measured = clickiness(out, from, to);
            Samples spliced = before;
            std::copy(after.begin() + at, after.end(), spliced.begin() + at);
            const double splice = clickiness(spliced, from, to);
            INFO("steady " + std::to_string(s) + ", through the change " + std::to_string(measured) + ", spliced " +
                 std::to_string(splice));
            CHECK(measured <= bound);
            CHECK(splice >= 10.0 * bound);
        }
    }
}

TEST_CASE("saturator automation plays through the engine sample-accurately") {
    sub::Engine engine;
    engine.setClipFadeMs(0);
    // (221.25 Hz: at its peak where the automation steps, so the first sample changed shows.)
    const Samples tone = smoothSine(221.25, 2.0, kSampleRate, 0.25);
    const std::string path = makeWav(stereo(tone), 2);
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, 0.0, 2.0, 0.0, 1.f)});
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "saturator", -1);
    const Samples untouched = engine.renderOffline(0.0, 2 * kSampleRate);

    // Drive: 0 dB until beat 2 (1 s), then +36 dB.
    using Points = std::vector<sub::AutomationPoint>;
    CHECK_EQ(paramInfo(engine, id, "drive").fromNormalized(0.5f), 0.f);
    CHECK_EQ(paramInfo(engine, id, "drive").fromNormalized(1.f), 36.f);
    engine.setTrackAutomation(track, {{id, "drive", Points{{0.0, 0.5f, 0.f}, {2.0, 0.5f, 0.f}, {2.0, 1.f, 0.f}}}});
    const Samples out = engine.renderOffline(0.0, 2 * kSampleRate);
    CHECK(allFinite(out));
    int64_t first = -1;
    for (size_t i = 0; i < out.size() && first < 0; ++i)
        if (out[i] != untouched[i]) first = static_cast<int64_t>(i) / 2;
    CHECK_EQ(first, int64_t{kSampleRate});

    // No click where it changes: no more than the steady renders either side make.
    engine.setTrackAutomation(track, {});
    setParam(engine, id, "drive", 36.f);
    const Samples hot = engine.renderOffline(0.0, 2 * kSampleRate);
    for (int c = 0; c < 2; ++c) {
        INFO(std::to_string(c));
        const double bound =
            2.0 * (clickiness(channel(untouched, c), kSampleRate / 5) + clickiness(channel(hot, c), kSampleRate / 5));
        CHECK(clickiness(channel(out, c), kSampleRate / 5) <= bound);
    }
}

TEST_CASE("reset and a new sample rate start the saturator cleanly") {
    const Values busy = {{"hq", 1.f}, {"color", 1.f}, {"base", -12.f}, {"depth", 18.f},
                         {"dc", 1.f}, {"type", 7.f},  {"drive", 18.f}};
    Saturator s(kSampleRate, busy);
    s.play(noise(kSampleRate / 2, 4, 1.f));
    s.processor().reset();
    CHECK(allEqual(s.play(Samples(kSampleRate / 4, 0.f)), 0.0));

    // A new rate: the curves as drawn, the latency the same.
    const Samples in = staircase(levels41());
    const Values medium = {{"type", 3.f}, {"drive", 6.f}};
    for (const double rate : {44100.0, 96000.0}) {
        INFO(std::to_string(rate));
        Saturator d(kSampleRate, medium);
        d.play(noise(kSampleRate / 4, 5, 1.f));
        d.processor().prepare(rate, kBlock);
        CHECK_ALLCLOSE(d.play(in), transferOf(3, 6.f, in), 1e-6, 1e-6);
        Saturator fresh(rate, medium);
        CHECK_ALLCLOSE(fresh.play(in), transferOf(3, 6.f, in), 1e-6, 1e-6);
        Saturator hq(rate, {{"hq", 1.f}});
        CHECK_EQ(hq.processor().latencySamples(), kHqLatency);
    }
}

TEST_CASE("the saturator's silence rings out to exact zeros") {
    // Without the renderer's flush-to-zero: every state is flushed once tiny.
    const Values busy = {{"hq", 1.f},       {"color", 1.f},     {"base", -24.f}, {"depth", 24.f},
                         {"freq", 1000.f},  {"width", 0.f},     {"dc", 1.f},     {"type", 7.f},
                         {"ws_damp", 50.f}, {"ws_depth", 50.f}, {"drive", 12.f}};
    Saturator s(kSampleRate, busy);
    Samples x = noise(kSampleRate / 2, 6, 1.f);
    x.resize(static_cast<size_t>(3.5 * kSampleRate), 0.f);
    const Samples out = s.play(x);
    CHECK(allFinite(out));
    CHECK(allEqual(slice(out, seconds(2.5)), 0.0));
    for (const float v : out) CHECK(v == 0.f || std::abs(v) >= std::numeric_limits<float>::min());

    // Denormal input passes as finite numbers, and dies away to zeros too.
    Samples tiny(static_cast<size_t>(2 * kSampleRate), 0.f);
    for (size_t i = 0; i < 1000; ++i) tiny[i] = (i % 2 ? 1e-40f : -1e-41f);
    const Samples quiet = s.play(tiny);
    CHECK(allFinite(quiet));
    CHECK(allEqual(slice(quiet, kSampleRate), 0.0));

    // A slowly decaying section (Amt Hi at 60 Hz) too, at 1x and at 4x: dsp::Biquad clears its two
    // states together (cleared one at a time, it would ring at about 1e-19 for ever).
    for (const float hq : {0.f, 1.f}) {
        INFO(std::to_string(hq));
        Saturator low(kSampleRate, {{"hq", hq}, {"color", 1.f}, {"depth", 6.f}, {"freq", 60.f}, {"drive", 12.f}});
        Samples y = noise(kSampleRate / 2, 10, 1.f);
        y.resize(static_cast<size_t>(2.5 * kSampleRate), 0.f);
        CHECK(allEqual(slice(low.play(y), seconds(1.5)), 0.0));
    }
}

TEST_CASE("the saturator's tail covers its ringing") {
    // The slow case: Color's peak at 30 Hz, narrow, +24 dB (its poles' time
    // constant 0.24 s), saturated hard so emphasis and de-emphasis don't cancel.
    const Values slow = {{"hq", 1.f},     {"dc", 1.f},     {"type", 6.f},  {"drive", 36.f}, {"color", 1.f},
                         {"base", -24.f}, {"depth", 24.f}, {"freq", 30.f}, {"width", 0.f}};
    Saturator s(kSampleRate, slow);
    const int tail = s.processor().tailSamples();
    const double tau = saturator::widthToQ(0.0) * std::pow(10.0, 24.0 / 40.0) / (kPi * 30.0);
    CHECK_EQ(tail, kHqPreroll + static_cast<int>(std::ceil(8.0 * tau * kSampleRate)));
    CHECK(tail <= 5 * kSampleRate);
    const Samples h = s.play(impulse(static_cast<size_t>(6 * kSampleRate)));
    CHECK(energy(h) > 0.0);
    CHECK(energy(slice(h, tail)) < 1e-6 * energy(h));

    // A cut rings longer still (its inverse's poles): a time constant of 0.49 s, a tail of 3.9 s.
    Saturator cut(kSampleRate, slow + Values{{"depth", -36.f}});
    const double cutTau = saturator::widthToQ(0.0) * std::pow(10.0, 36.0 / 40.0) / (kPi * 30.0);
    CHECK_APPROX_TOL(cutTau, 0.486, 0.01, 0.0);
    CHECK_EQ(cut.processor().tailSamples(), kHqPreroll + static_cast<int>(std::ceil(8.0 * cutTau * kSampleRate)));
    const Samples hc = cut.play(impulse(static_cast<size_t>(8 * kSampleRate)));
    CHECK(energy(slice(hc, cut.processor().tailSamples())) < 1e-6 * energy(hc));

    // DC alone: 8 of its filter's time constants; Hi-Quality alone, its filters' memory; Color off
    // (whatever its gains), nothing.
    Saturator dc(kSampleRate, {{"dc", 1.f}});
    CHECK_EQ(dc.processor().tailSamples(), static_cast<int>(std::ceil(8.0 / (2.0 * kPi * 5.0) * kSampleRate)));
    Saturator hq(kSampleRate, {{"hq", 1.f}});
    CHECK_EQ(hq.processor().tailSamples(), kHqPreroll);
    const Samples hh = hq.play(impulse(1024, 0, 0.5f));
    CHECK(energy(slice(hh, kHqPreroll)) < 1e-9 * energy(hh));
    Saturator colorOff(kSampleRate, {{"base", 24.f}, {"depth", 24.f}});
    CHECK_EQ(colorOff.processor().tailSamples(), 0);
    // At most 5 s.
    Saturator longest(22050.0, {{"color", 1.f}, {"depth", -36.f}, {"freq", 30.f}, {"width", 0.f}});
    CHECK(longest.processor().tailSamples() <= 5 * 22050);
}

TEST_CASE("the saturator stays finite and bounded at the extremes") {
    const std::vector<sub::ParamInfo> params = builtinInfo("saturator").params;
    std::vector<Values> sets;
    Values lows, highs;
    for (const sub::ParamInfo& p : params) {
        lows.push_back({p.id, p.minValue});
        highs.push_back({p.id, p.maxValue});
    }
    sets.push_back(lows);
    sets.push_back(highs);
    std::mt19937 random(12);
    std::uniform_real_distribution<float> unit(0.f, 1.f);
    for (int k = 0; k < 50; ++k) {
        Values values;
        for (const sub::ParamInfo& p : params) values.push_back({p.id, p.fromNormalized(unit(random))});
        sets.push_back(values);
    }
    // Post Clip Hard holds the output to the Output level at any Dry/Wet. With Hi-Quality
    // it holds at 4x, and the 4x filters can take the band-limited output a little past it:
    // at most by their worst gain (1.84; hard-clipped saws and sines measure 1.17 at most).
    const double worst = downWorstGain();
    CHECK(worst > 1.0);
    CHECK(worst < 2.0);
    for (const double rate : {44100.0, 48000.0, 192000.0}) {
        const auto n = static_cast<size_t>(rate / 2);
        Samples in = noise(n, 7, 1.f);
        for (size_t i = 0; i < n; ++i) in[i] += (i / static_cast<size_t>(rate / 20)) % 2 ? 1.f : -1.f;
        for (size_t k = 0; k < sets.size(); ++k) {
            INFO(std::to_string(rate) + " Hz, set " + std::to_string(k));
            const Samples out = play(sets[k], in, {}, rate);
            CHECK(allFinite(out));
            const Values clipped = sets[k] + Values{{"clip", 2.f}};
            const Samples held = play(clipped, in, {}, rate);
            float output = 0.f, hq = 0.f;
            for (const auto& [id, value] : clipped) {
                if (id == "output") output = value;
                if (id == "hq") hq = value;
            }
            CHECK(maxAbs(held) <= sub::expDbToGain(output) * (hq >= 0.5f ? worst : 1.0) + 1e-6);
        }
    }
    // Frequency at the top at 22.05 kHz: kept below Nyquist, stable.
    for (const float depth : {-36.f, 36.f}) {
        for (const float width : {0.f, 100.f}) {
            INFO(std::to_string(depth) + " dB, " + std::to_string(width) + " %");
            const Samples out = play(
                {{"color", 1.f}, {"freq", 18500.f}, {"depth", depth}, {"width", width}, {"drive", 24.f}, {"type", 6.f}},
                noise(22050, 8, 1.f), {}, 22050.0);
            CHECK(allFinite(out));
            CHECK(maxAbs(out) < 100.0);
        }
    }
    CHECK_APPROX(saturator::colorFrequency(18500.0, 22050.0), 9922.5);
    CHECK_APPROX(saturator::colorFrequency(18500.0, 44100.0), 18500.0);
}

TEST_CASE("the saturator takes NaN and infinity in its input as silence") {
    // BuiltinProcessor::process() takes what isn't audio as 0 before the device sees it, so the
    // DC filter (which runs even while DC is off), Color's sections and the 4x path's filters
    // never hold it: the output is exactly what zeros there would give, also with DC switched on
    // long after, and the device goes on playing.
    const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity(), 3e38f};
    const std::vector<Values> settings = {
        {{"dc", 1.f}},
        {{"color", 1.f}, {"depth", 10.f}},
        {{"type", 7.f}, {"drive", 12.f}, {"color", 1.f}, {"base", -12.f}, {"depth", 10.f}, {"dc", 1.f}, {"hq", 1.f}},
        {},
    };
    const std::vector<Change> dcLater = {{seconds(0.5), "dc", 1.f}};
    for (const float value : bad) {
        for (size_t k = 0; k < settings.size(); ++k) {
            INFO(std::to_string(value) + ", set " + std::to_string(k));
            Samples in = noise(kSampleRate, 11, 0.5f), zeroed = in;
            for (const size_t at : {1000u, 1001u, 7000u}) {
                in[at] = value;
                zeroed[at] = 0.f;
            }
            const std::vector<Change> changes = settings[k].empty() ? dcLater : std::vector<Change>{};
            const Samples out = play(settings[k], in, changes);
            CHECK(allFinite(out));
            CHECK_ARRAY_EQUAL(out, play(settings[k], zeroed, changes));
            CHECK(rms(slice(out, seconds(0.9))) > 0.1);
        }
    }
}

TEST_CASE("the saturator on one channel, and its channels independent") {
    const Samples a = noise(kSampleRate / 2, 9, 0.8f), b = smoothSine(440.0, 0.5);
    for (const float hq : {0.f, 1.f}) {
        INFO(std::to_string(hq));
        const Values values = {{"type", 7.f},   {"drive", 12.f}, {"color", 1.f},
                               {"depth", 12.f}, {"dc", 1.f},     {"hq", hq}};
        const std::vector<Change> changes = {{9000, "drive", 0.f}, {12000, "type", 3.f}, {15000, "base", 6.f}};
        Saturator stereo(kSampleRate, values), mono(kSampleRate, values);
        Samples l = a, r = b;
        stereo.run({&l, &r}, changes);
        CHECK_ARRAY_EQUAL(mono.play(a, changes), l);
        // Silence on one side stays silence, whatever the other does.
        Saturator s(kSampleRate, values);
        Samples silent(a.size(), 0.f), loud = a;
        s.run({&silent, &loud}, changes);
        CHECK(allEqual(silent, 0.0));
        CHECK(anyNonzero(loud));
    }
}

TEST_CASE("the saturator's displays") {
    const Samples in = sine(1000.0, 1.0, 0.5);
    // In blocks of 256 (two meter values each), and of 100 with automation (that changes
    // nothing) splitting them at odd frames: the meters count across every stretch.
    std::vector<Change> splits;
    for (int64_t at = 0; at < static_cast<int64_t>(in.size()); at += 4096)
        for (const int64_t frame : {333, 1001, 2777}) splits.push_back({at + frame, "drive", 0.f});
    for (const auto& [channels, block] : {std::pair{2, 256}, std::pair{1, 256}, std::pair{2, 100}, std::pair{1, 100}}) {
        INFO(std::to_string(channels) + " channels, blocks of " + std::to_string(block));
        Saturator s(kSampleRate, {{"type", 3.f}});
        Samples l = in, r = in;
        std::vector<float> inPeak, outPeak, input, output;  // (read as it runs: a display keeps 8192 values)
        const auto read = [&] {
            for (auto [id, into] : {std::pair{"in_peak", &inPeak}, std::pair{"out_peak", &outPeak},
                                    std::pair{"input", &input}, std::pair{"output", &output}}) {
                const std::vector<float> values = s.display(id);
                into->insert(into->end(), values.begin(), values.end());
            }
        };
        const std::vector<Change> changes = block == 256 ? std::vector<Change>{} : splits;
        if (channels == 2)
            s.run({&l, &r}, changes, block, [&](int64_t, int) { read(); });
        else
            s.run({&l}, changes, block, [&](int64_t, int) { read(); });
        read();
        CHECK_EQ(inPeak.size(), 375u);
        CHECK_EQ(outPeak.size(), 375u);
        for (size_t i = 1; i < inPeak.size(); ++i) {
            CHECK(inPeak[i] >= 0.498f && inPeak[i] <= 0.5f);
            CHECK(outPeak[i] >= 0.4600f && outPeak[i] <= 0.4622f);
        }
        CHECK_EQ(input.size(), in.size());
        CHECK_EQ(output.size(), in.size());
        CHECK_ARRAY_EQUAL(input, in);
        CHECK_ARRAY_EQUAL(output, l);
    }
    // Mono sums of different channels; the louder channel's peak.
    Saturator s(kSampleRate, {});
    Samples l = sine(1000.0, 0.1, 0.2), r = sine(1000.0, 0.1, 0.6);
    const Samples l0 = l, r0 = r;
    s.run({&l, &r});
    const std::vector<float> input = s.display("input"), inPeak = s.display("in_peak");
    Samples sum(l0.size());
    for (size_t i = 0; i < sum.size(); ++i) sum[i] = (l0[i] + r0[i]) * 0.5f;
    CHECK_ARRAY_EQUAL(input, sum);
    CHECK_APPROX_TOL(inPeak.back(), 0.6, 0.0, 1e-6);
}
