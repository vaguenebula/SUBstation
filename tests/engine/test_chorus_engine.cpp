// The built-in Chorus-Ensemble: modulated delays added to the sound. Its
// delays are the design's (builtin/ChorusDesign.h, which its editor draws),
// sample by sample, in every mode and at any sample rate; Ensemble beats and
// the voices detune as far as the design says; feedback (none in Vibrato),
// Invert, Warmth, the high-pass, Width, Output and Dry/Wet do what they say;
// every control changes without a click, and so does switching it on in the
// middle of a sound; automation plays to the sample; reset and a new rate start
// it cleanly; it stays stable at the extremes, takes NaN and infinity as
// silence, and silence rings out to exact zeros; one channel plays as the left
// of two; its tail covers its echoes and its displays carry the LFO's phase and
// the wet's level.

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "Engine.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/ChorusDesign.h"
#include "builtin/DspBlocks.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"
#include "harness/Standalone.h"

using namespace subtest;
namespace chorus = sub::chorus;

namespace {

constexpr int kBlock = Standalone::kMaxBlock;

using Values = ParamValues;
using Change = ParamChange;

// A Chorus-Ensemble on its own, as the renderer runs it (Standalone), with
// what its tests add: two channels played at once, and its displays read.
class Chorus : public Standalone {
public:
    explicit Chorus(double rate = kSampleRate, const Values& values = {}) : Standalone("chorus", rate, values) {}

    using Standalone::play;
    // Two channels: what comes out of each.
    std::pair<Samples, Samples> play(Samples left, Samples right, const std::vector<Change>& changes = {},
                                     int block = 256) {
        run({&left, &right}, changes, block);
        return {std::move(left), std::move(right)};
    }

    // The values display `index` published since the last call.
    std::vector<float> display(int index) {
        std::vector<float> out;
        uint64_t& position = positions_[static_cast<size_t>(index)];
        position = processor().readDisplay(index, position, out);
        return out;
    }

private:
    uint64_t positions_[2] = {};
};

// After a reset, what goes into the delays fades in over 5 ms: a sound from here on goes in whole.
constexpr int64_t kSettled = 1024;

// Amount 0, no feedback, no warmth, no high-pass, fully wet at unity: the voices are plain delays.
const Values kPure = {{"amount", 0.f}, {"feedback", 0.f}, {"warmth", 0.f}, {"hp", 0.f},
                      {"mix", 100.f},  {"output", 0.f},   {"width", 100.f}};

Values with(Values base, const Values& more) {
    base.insert(base.end(), more.begin(), more.end());
    return base;
}

std::string describe(const Values& values) {
    std::string text;
    for (const auto& [id, value] : values) text += id + "=" + std::to_string(value) + " ";
    return text;
}

Samples impulse(size_t length, int64_t at = kSettled) {
    Samples x(length, 0.f);
    x[static_cast<size_t>(at)] = 1.f;
    return x;
}

// `x` silent until kSettled, so it goes into the delays whole.
Samples settled(Samples x) {
    std::fill_n(x.begin(), kSettled, 0.f);
    return x;
}

// The largest step between one sample and the next over [from, to).
double largestStep(const Samples& x, int64_t from, int64_t to) {
    double most = 0.0;
    for (int64_t i = std::max<int64_t>(from, 1); i < to; ++i)
        most = std::max(most, std::abs(static_cast<double>(x[static_cast<size_t>(i)]) - x[static_cast<size_t>(i - 1)]));
    return most;
}

Samples noise(size_t length, unsigned seed, float amplitude = 0.5f) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> uniform(-amplitude, amplitude);
    Samples x(length);
    for (float& v : x) v = uniform(random);
    return x;
}

Samples tone(double freq, double seconds, double rate = kSampleRate, double amplitude = 0.5) {
    Samples x(static_cast<size_t>(seconds * rate));
    for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * freq * i / rate));
    return x;
}

int64_t samples(double seconds, double rate = kSampleRate) {
    return static_cast<int64_t>(std::llround(seconds * rate));
}

// The largest 6th difference over [from, to): a steep high-pass, about 8 times
// (18 dB) more sensitive at Nyquist than at a quarter of the sample rate and
// 2 x 10^5 times more than at 2 kHz (48 kHz). A step of d shows as up to 10 d,
// a kink (a step in the slope) of s as up to 6 s; a smooth signal well below
// Nyquist hardly at all.
double clickiness(const Samples& x, int64_t from = 0, int64_t to = -1) {
    std::vector<double> d(x.begin(), x.end());
    for (int k = 0; k < 6; ++k)
        for (size_t i = d.size() - 1; i > 0; --i) d[i] -= d[i - 1];
    if (to < 0) to = static_cast<int64_t>(d.size());
    double worst = 0.0;
    for (int64_t i = std::max<int64_t>(from, 6); i < to; ++i)
        worst = std::max(worst, std::abs(d[static_cast<size_t>(i)]));
    return worst;
}

double energy(const Samples& x) {
    double sum = 0.0;
    for (const float v : x) sum += static_cast<double>(v) * v;
    return sum;
}

double frac(double x) { return x - std::floor(x); }

// The delay each side plays a wrapping ramp at, sample by sample (NaN where the
// samples a read takes straddle a wrap). The ramp, (j mod 16384 - 8192) / 16384,
// rises 2^-14 a sample, so float rounding stays far under a hundredth of a
// sample, and Hermite interpolation reproduces a line exactly.
std::pair<std::vector<double>, std::vector<double>> delaysOnARamp(double rate, const Values& values, double seconds,
                                                                  double longest) {
    const auto n = static_cast<size_t>(seconds * rate);
    Samples x(n);
    for (size_t j = 0; j < n; ++j) x[j] = static_cast<float>((static_cast<double>(j % 16384) - 8192.0) / 16384.0);
    Chorus c(rate, values);
    const auto [left, right] = c.play(x, x);
    const auto delays = [&](const Samples& y) {
        std::vector<double> d(n, std::numeric_limits<double>::quiet_NaN());
        for (size_t j = 0; j < n; ++j) {
            const double segment = 16384.0 * std::floor(j / 16384.0);
            if (static_cast<double>(j) - longest - 3.0 < segment) continue;
            d[j] = static_cast<double>(j) - (16384.0 * y[j] + 8192.0 + segment);
        }
        return d;
    };
    return {delays(left), delays(right)};
}

// The largest difference between measured delays and a model of them (the
// LFO's phase after sample j is (j + 1) rate / sampleRate), from `from` on,
// leaving out samples `skip` says to.
template <typename Model, typename Skip>
double worstDelayError(const std::vector<double>& measured, int64_t from, Model&& model, Skip&& skip) {
    double worst = 0.0;
    int64_t checked = 0;
    for (int64_t j = from; j < static_cast<int64_t>(measured.size()); ++j) {
        const double d = measured[static_cast<size_t>(j)];
        if (std::isnan(d) || skip(j)) continue;
        worst = std::max(worst, std::abs(d - model(j)));
        ++checked;
    }
    REQUIRE(checked > static_cast<int64_t>(measured.size()) / 2);
    return worst;
}
template <typename Model>
double worstDelayError(const std::vector<double>& measured, int64_t from, Model&& model) {
    return worstDelayError(measured, from, model, [](int64_t) { return false; });
}

// Upward zero crossings, interpolated (in samples), from `from` on.
std::vector<double> upwardCrossings(const Samples& x, size_t from) {
    std::vector<double> at;
    for (size_t i = std::max<size_t>(from, 1); i < x.size(); ++i) {
        if (x[i - 1] < 0.f && x[i] >= 0.f) at.push_back(static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]));
    }
    return at;
}

// The frequency a signal plays at over time, from its zero crossings, averaged
// over `periods` periods: (time, frequency) pairs.
std::vector<std::pair<double, double>> instantaneousFrequency(const Samples& x, size_t from, double rate,
                                                              int periods = 5) {
    const std::vector<double> at = upwardCrossings(x, from);
    std::vector<std::pair<double, double>> curve;
    for (size_t k = 0; k + periods < at.size(); ++k) {
        const double span = at[k + periods] - at[k];
        curve.emplace_back(0.5 * (at[k] + at[k + periods]) / rate, periods * rate / span);
    }
    return curve;
}

// A curve's value at `t`, linear between its points.
double valueAt(const std::vector<std::pair<double, double>>& curve, double t) {
    const auto it = std::lower_bound(curve.begin(), curve.end(), std::make_pair(t, -1e300));
    if (it == curve.begin()) return curve.front().second;
    if (it == curve.end()) return curve.back().second;
    const auto& [t1, v1] = *it;
    const auto& [t0, v0] = *(it - 1);
    return v0 + (v1 - v0) * (t - t0) / (t1 - t0);
}

// The level of the tone in bin k of a Hann-windowed spectrum (the window spreads
// a bin-exact tone over its bin and the two beside it).
double binLevel(const std::vector<double>& s, size_t k) {
    double power = 0.0;
    for (size_t b = k - 1; b <= k + 1 && b < s.size(); ++b) power += s[b] * s[b];
    return std::sqrt(power);
}

// A frequency that falls exactly on a bin of an n-point transform at `rate`.
double binFrequency(double approx, size_t n, double rate = kSampleRate) {
    return std::round(approx * static_cast<double>(n) / rate) * rate / static_cast<double>(n);
}
size_t binOf(double freq, size_t n, double rate = kSampleRate) {
    return static_cast<size_t>(std::llround(freq * static_cast<double>(n) / rate));
}

// What isn't a sinusoid at `freq` in x: the RMS of what is left after a least-
// squares fit of a sine and a cosine, against the fit's, in dB.
double residualDb(const Samples& x, double freq, double rate = kSampleRate) {
    double ss = 0.0, cc = 0.0, sc = 0.0, xs = 0.0, xc = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double w = 2.0 * kPi * freq * static_cast<double>(i) / rate;
        const double s = std::sin(w), c = std::cos(w), v = x[i];
        ss += s * s;
        cc += c * c;
        sc += s * c;
        xs += v * s;
        xc += v * c;
    }
    const double det = ss * cc - sc * sc;
    const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
    double fit = 0.0, rest = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double w = 2.0 * kPi * freq * static_cast<double>(i) / rate;
        const double f = a * std::sin(w) + b * std::cos(w);
        fit += f * f;
        rest += (x[i] - f) * (x[i] - f);
    }
    return 10.0 * std::log10(rest / fit);
}

}  // namespace

TEST_CASE("the chorus is listed with its parameters") {
    const sub::BuiltinInfo info = builtinInfo("chorus");
    CHECK_EQ(info.name, std::string("Chorus-Ensemble"));
    CHECK(!info.isInstrument());
    CHECK(paramIds(info.params) == (std::vector<std::string>{"mode", "taps", "time", "rate", "amount", "feedback",
                                                             "fb_invert", "width", "offset", "shape", "warmth", "hp",
                                                             "hp_freq", "output", "mix"}));
    struct Expected {
        std::string name, unit;
        float min, max, def;
        bool log;
        std::vector<std::string> labels;
    };
    const std::vector<std::string> offOn = {"Off", "On"};
    const std::vector<Expected> expected = {
        {"Mode", "", 0.f, 2.f, 0.f, false, {"Chorus", "Ensemble", "Vibrato"}},
        {"Tap Count", "", 0.f, 1.f, 1.f, false, {"1", "2"}},
        {"Delay Time", "", 0.f, 5.f, 0.f, false, {"Auto", "7 ms", "10 ms", "20 ms", "35 ms", "50 ms"}},
        {"Rate", "Hz", 0.1f, 15.f, 0.8f, true, {}},
        {"Amount", "%", 0.f, 100.f, 50.f, false, {}},
        {"Feedback", "%", 0.f, 100.f, 0.f, false, {}},
        {"Feedback Invert", "", 0.f, 1.f, 0.f, false, offOn},
        {"Width", "%", 0.f, 200.f, 100.f, false, {}},
        {"Offset", "\xc2\xb0", 0.f, 180.f, 0.f, false, {}},
        {"Shape", "%", 0.f, 100.f, 0.f, false, {}},
        {"Warmth", "%", 0.f, 100.f, 0.f, false, {}},
        {"High-pass", "", 0.f, 1.f, 0.f, false, offOn},
        {"High-pass Freq", "Hz", 20.f, 2000.f, 100.f, true, {}},
        {"Output", "dB", -36.f, 6.f, 0.f, false, {}},
        {"Dry/Wet", "%", 0.f, 100.f, 50.f, false, {}},
    };
    REQUIRE(info.params.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        const sub::ParamInfo& p = info.params[i];
        const Expected& e = expected[i];
        INFO(p.id);
        CHECK_EQ(p.name, e.name);
        CHECK_EQ(p.unit, e.unit);
        CHECK_APPROX(p.minValue, e.min);
        CHECK_APPROX(p.maxValue, e.max);
        CHECK_APPROX(p.defaultValue, e.def);
        CHECK_EQ(p.isLog(), e.log);
        CHECK(p.valueLabels == e.labels);
        CHECK(p.automatable);
    }
    // Lists land on their steps, automation too.
    const sub::ParamInfo& time = info.params[2];
    CHECK_EQ(time.stepCount(), 5);
    CHECK_EQ(time.fromNormalized(time.toNormalized(4.f)), 4.f);

    // Its displays: the LFO's phase and the wet's level, a value per 128 samples each.
    Chorus c;
    const std::vector<sub::DisplayInfo> displays = c.processor().displays();
    REQUIRE(displays.size() == 2);
    CHECK_EQ(displays[0].id, std::string("phase"));
    CHECK_EQ(displays[0].samplesPerValue, 128);
    CHECK_EQ(displays[1].id, std::string("level"));
    CHECK_EQ(displays[1].samplesPerValue, 128);

    // The engine makes it; no latency (the delay is the effect).
    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "chorus", -1);
    const sub::ProcessorInfo processor = engine.processorInfo(id);
    CHECK_EQ(processor.name, std::string("Chorus-Ensemble"));
    CHECK_EQ(processor.latency, 0);
    CHECK_EQ(processor.tail, 2952);
}

TEST_CASE("the chorus's voices, as designed") {
    using chorus::Layout;
    using chorus::Mode;
    // Indices normalise: what doesn't apply to a mode is the same layout.
    CHECK(chorus::layout(1, 0, 3) == chorus::layout(1, 1, 0));
    CHECK(chorus::layout(2, 0, 5) == chorus::layout(2, 1, 0));
    CHECK(!(chorus::layout(0, 0, 0) == chorus::layout(0, 1, 0)));
    CHECK_EQ(chorus::voices(chorus::layout(0, 0, 0)), 1);
    CHECK_EQ(chorus::voices(chorus::layout(0, 1, 0)), 2);
    CHECK_EQ(chorus::voices(chorus::layout(1, 0, 0)), 3);
    CHECK_EQ(chorus::voices(chorus::layout(2, 1, 0)), 1);
    // The ranges at Amount 100: the editor's axis.
    const Layout autoTime = chorus::layout(0, 1, 0), fixed50 = chorus::layout(0, 1, 5);
    const Layout ensemble = chorus::layout(1, 1, 0), vibrato = chorus::layout(2, 1, 0);
    CHECK_APPROX(chorus::lowestMs(autoTime), 1.5);
    CHECK_APPROX(chorus::highestMs(autoTime), 11.5);
    CHECK_APPROX(chorus::lowestMs(fixed50), 46.0);
    CHECK_APPROX(chorus::highestMs(fixed50), 54.0);
    CHECK_APPROX(chorus::lowestMs(ensemble), 2.5);
    CHECK_APPROX(chorus::highestMs(ensemble), 12.5);
    CHECK_APPROX(chorus::lowestMs(vibrato), 0.5);
    CHECK_APPROX(chorus::highestMs(vibrato), 11.5);
    // Auto's centre follows Amount; a fixed time holds.
    CHECK_APPROX(chorus::centreMs(autoTime, 0.0), 1.5);
    CHECK_APPROX(chorus::centreMs(autoTime, 0.5), 4.0);
    CHECK_APPROX(chorus::centreMs(fixed50, 0.0), 50.0);
    CHECK_APPROX(chorus::centreMs(fixed50, 1.0), 50.0);
    // The voices' phases.
    CHECK_APPROX(chorus::voicePhase(autoTime, 1, 1, 0.0), 0.75);
    CHECK_APPROX(chorus::voicePhase(chorus::layout(0, 0, 0), 1, 0, 0.0), 0.5);
    CHECK_APPROX(chorus::voicePhase(ensemble, 1, 2, 0.0), 5.0 / 6.0);
    CHECK_APPROX(chorus::voicePhase(vibrato, 1, 0, 0.25), 0.25);
    // The triangle is in step with the sine, and the morph between them.
    CHECK_APPROX_TOL(chorus::triangle(0.0), 0.0, 0.0, 1e-12);
    CHECK_APPROX(chorus::triangle(0.25), 1.0);
    CHECK_APPROX(chorus::triangle(0.75), -1.0);
    CHECK_APPROX(chorus::triangle(1.125), 0.5);
    CHECK_APPROX(chorus::lfoValue(Mode::Vibrato, 0.125, 0.5), 0.5 * (std::sin(kPi / 4) + 0.5));
    CHECK_APPROX(chorus::lfoValue(Mode::Chorus, 0.125, 1.0), std::sin(kPi / 4));  // (Shape is Vibrato's)
    // The detune at the defaults (Auto, 50 %, 0.8 Hz): the readout's "±22 ct".
    CHECK_APPROX_TOL(chorus::detuneUpCents(autoTime, 0.8, 0.5, 0.0), 21.6, 0.0, 0.1);
    CHECK_APPROX_TOL(chorus::peakDetuneCents(autoTime, 0.8, 0.5, 0.0), 21.9, 0.0, 0.1);
    // The feedback's limiter: the identity within ±1, smooth there, never beyond 2.
    CHECK_EQ(chorus::limitFeedback(0.75f), 0.75f);
    CHECK_EQ(chorus::limitFeedback(-1.f), -1.f);
    CHECK_APPROX_TOL(chorus::limitFeedback(1.001f), 1.001, 0.0, 1e-6);
    CHECK(chorus::limitFeedback(100.f) <= 2.f);
    CHECK_EQ(chorus::limitFeedback(-100.f), -chorus::limitFeedback(100.f));
    // The warmth: the identity at 0, unity slope at silence, 0 in for 0 out, bounded (a colour, not a
    // limiter: a full-scale value loses only a little).
    for (const double x : {-1.5, -0.3, 0.0, 0.2, 0.9}) CHECK_EQ(chorus::warm(0.0, x), x);
    CHECK_EQ(chorus::warm(1.0, 0.0), 0.0);
    CHECK_APPROX_TOL(chorus::warm(1.0, 1e-6) / 1e-6, 1.0, 0.0, 1e-5);
    CHECK_APPROX_TOL(chorus::warm(1.0, -1e-6) / -1e-6, 1.0, 0.0, 1e-5);
    CHECK_APPROX_TOL(chorus::warm(1.0, 1.0), 0.78, 0.0, 0.01);  // (the curve before: 0.36)
    CHECK_APPROX_TOL(chorus::warm(1.0, -1.0), -0.91, 0.0, 0.01);
    const chorus::WarmCurve curve;
    CHECK_APPROX_TOL(curve(1e9), 1.41, 0.0, 0.01);
    CHECK_APPROX_TOL(curve(-1e9), -1.71, 0.0, 0.01);
    // Its antiderivative is its antiderivative.
    for (const double x : {-3.0, -0.8, -0.15, 0.0, 0.4, 1.2, 2.5}) {
        const double h = 1e-4, slope = (curve.integral(x + h) - curve.integral(x - h)) / (2.0 * h);
        CHECK_APPROX_TOL(slope, curve(x), 0.0, 1e-8);
    }
    // Played through the stage (its bend averaged over each step), a slow sound is the curve's, Warmth 0 the
    // input bit for bit, and silence silence.
    chorus::WarmStage stage, dry;
    double worst = 0.0;
    for (int i = 0; i < 4800; ++i) {
        const double x = 0.9 * std::sin(2.0 * kPi * 50.0 * i / kSampleRate) + (i % 7 == 0 ? 1e-6 : 0.0);
        worst = std::max(worst, std::abs(stage.process(x, 1.0) - chorus::warm(1.0, x)));
        CHECK_EQ(dry.process(x, 0.0), x);
    }
    CHECK(worst < 1e-3);
    stage.reset();
    for (int i = 0; i < 10; ++i) CHECK_EQ(stage.process(0.0, 1.0), 0.0);
    CHECK_EQ(chorus::warmLowpassCoefficient(0.0, 48000.0), 0.0);
    CHECK_APPROX(chorus::warmLowpassCoefficient(1.0, 48000.0), std::exp(-2.0 * kPi * 4000.0 / 48000.0));
    CHECK(chorus::warmLowpassCoefficient(1e-6, 48000.0) < 1e-12);
    // The high-pass's frequency, kept below Nyquist.
    CHECK_APPROX(chorus::highPassFrequency(5.0, 48000.0), 20.0);
    CHECK_APPROX(chorus::highPassFrequency(2000.0, 4000.0), 1800.0);
}

TEST_CASE("fully dry, the chorus passes the input bit for bit") {
    const Samples left = noise(kSampleRate, 1), right = noise(kSampleRate, 2), third = noise(kSampleRate, 3);
    for (const float mode : {0.f, 1.f, 2.f}) {
        INFO(std::to_string(mode));
        const Values values = {{"mode", mode},    {"feedback", 90.f}, {"warmth", 100.f}, {"mix", 0.f},
                               {"amount", 100.f}, {"rate", 7.f},      {"width", 200.f},  {"output", 6.f}};
        Chorus c(kSampleRate, values);
        Samples l = left, r = right, t = third;
        c.run({&l, &r, &t});
        CHECK_ARRAY_EQUAL(l, left);
        CHECK_ARRAY_EQUAL(r, right);
        CHECK_ARRAY_EQUAL(t, third);

        // The high-pass on, the dry goes through its crossover (in phase with the wet's lows): the crossover's
        // lows and highs summed, an all-pass, flat in level.
        Chorus filtered(kSampleRate, with(values, {{"hp", 1.f}, {"hp_freq", 300.f}}));
        const auto [fl, fr] = filtered.play(left, right);
        const sub::dsp::CrossoverCoefficients coefficients = sub::dsp::CrossoverCoefficients::at(300.0, kSampleRate);
        for (const auto& [in, out] : {std::pair{&left, &fl}, std::pair{&right, &fr}}) {
            sub::dsp::Crossover crossover;
            Samples want(in->size());
            for (size_t i = 0; i < in->size(); ++i) {
                float low = 0.f, high = 0.f;
                crossover.process(coefficients, (*in)[i], low, high);
                want[i] = low + high;
            }
            CHECK_ALLCLOSE(*out, want, 0.0, 1e-6);
            CHECK_APPROX_TOL(rms(*out) / rms(*in), 1.0, 0.0, 0.01);
        }
    }
}

TEST_CASE("at Amount 0 each chorus layout is a plain delay of its centre") {
    struct Case {
        Values values;
        int64_t delay;
    };
    const std::vector<Case> cases = {
        {{{"mode", 0.f}, {"taps", 1.f}, {"time", 0.f}}, 72},   {{{"mode", 0.f}, {"taps", 0.f}, {"time", 0.f}}, 72},
        {{{"mode", 0.f}, {"taps", 1.f}, {"time", 1.f}}, 336},  {{{"mode", 0.f}, {"taps", 0.f}, {"time", 2.f}}, 480},
        {{{"mode", 0.f}, {"taps", 1.f}, {"time", 3.f}}, 960},  {{{"mode", 0.f}, {"taps", 0.f}, {"time", 4.f}}, 1680},
        {{{"mode", 0.f}, {"taps", 1.f}, {"time", 5.f}}, 2400}, {{{"mode", 1.f}}, 360},
        {{{"mode", 2.f}, {"offset", 90.f}, {"shape", 50.f}}, 288},
    };
    for (const Case& k : cases) {
        INFO(describe(k.values));
        Chorus c(kSampleRate, with(kPure, k.values));
        const auto [l, r] = c.play(impulse(4096), impulse(4096));
        for (const Samples* side : {&l, &r}) {
            Samples want(side->size(), 0.f);
            want[static_cast<size_t>(kSettled + k.delay)] = 1.f;
            CHECK_ALLCLOSE(*side, want, 0.0, 1e-6);
        }
    }
}

TEST_CASE("the chorus's delays move as the design says") {
    struct Case {
        std::string what;
        Values values;
        double longest;  // samples
        double centre, swing, rate, rightPhase;
        bool triangle = false;
    };
    const std::vector<Case> cases = {
        {"Auto, 1 tap", {{"taps", 0.f}, {"amount", 100.f}, {"rate", 1.f}}, 552, 312, 240, 1.0, 0.5},
        {"Auto, 1 tap, Amount 50 (the centre follows)", {{"taps", 0.f}, {"amount", 50.f}, {"rate", 1.f}}, 312, 192,
         120, 1.0, 0.5},
        {"Auto, 2 taps (opposite: their mean holds)", {{"taps", 1.f}, {"amount", 100.f}, {"rate", 1.f}}, 552, 312, 0,
         1.0, 0.0},
        {"Ensemble (three a third apart)", {{"mode", 1.f}, {"amount", 100.f}, {"rate", 1.f}}, 600, 360, 0, 1.0, 0.0},
        {"Vibrato, Offset 90", {{"mode", 2.f}, {"amount", 100.f}, {"rate", 1.f}, {"offset", 90.f}}, 552, 288, 264,
         1.0, 0.25},
        {"Vibrato, Shape 100", {{"mode", 2.f}, {"amount", 100.f}, {"rate", 1.f}, {"shape", 100.f}}, 552, 288, 264,
         1.0, 0.0, true},
        {"10 ms, 1 tap, Amount 50, 5 Hz",
         {{"taps", 0.f}, {"time", 2.f}, {"amount", 50.f}, {"rate", 5.f}},
         576,
         480,
         96,
         5.0,
         0.5},
        {"50 ms, 1 tap", {{"taps", 0.f}, {"time", 5.f}, {"amount", 100.f}, {"rate", 1.f}}, 2592, 2400, 192, 1.0, 0.5},
    };
    for (const Case& k : cases) {
        INFO(k.what);
        const auto [left, right] = delaysOnARamp(kSampleRate, with(kPure, k.values), 2.0, k.longest);
        const auto from = static_cast<int64_t>(k.longest) + 1000;
        for (int side = 0; side < 2; ++side) {
            INFO(side == 0 ? "left" : "right");
            const double offset = side == 0 ? 0.0 : k.rightPhase;
            const auto lfo = [&](int64_t j) {
                const double phase = (j + 1) * k.rate / kSampleRate + offset;
                return k.triangle ? chorus::triangle(phase) : std::sin(2.0 * kPi * phase);
            };
            const auto model = [&](int64_t j) { return k.centre + k.swing * lfo(j); };
            if (!k.triangle) {
                CHECK(worstDelayError(side == 0 ? left : right, from, model) < 0.02);
                continue;
            }
            // The triangle's corners: the delay is worked out at each 16-sample chunk's
            // start, middle and end, and follows a parabola through them, which rounds a
            // corner within a chunk off by up to 2 * swing * (16 rate / sampleRate).
            const double chunkCycles = 16.0 * k.rate / kSampleRate;
            const auto nearCorner = [&](int64_t j) {
                const double p = frac((j + 1) * k.rate / kSampleRate + offset);
                return std::abs(p - 0.25) < 2 * chunkCycles || std::abs(p - 0.75) < 2 * chunkCycles;
            };
            CHECK(worstDelayError(side == 0 ? left : right, from, model, nearCorner) < 0.02);
            CHECK(worstDelayError(side == 0 ? left : right, from, model) < 2.0 * k.swing * chunkCycles + 0.02);
        }
    }
    // Fractional delays: 1.5 ms at 44.1 kHz.
    const auto [left, right] = delaysOnARamp(44100.0, with(kPure, {{"taps", 0.f}}), 1.0, 70);
    CHECK(worstDelayError(left, 2000, [](int64_t) { return 66.15; }) < 0.02);
    CHECK(worstDelayError(right, 2000, [](int64_t) { return 66.15; }) < 0.02);
}

TEST_CASE("the chorus's Ensemble voices beat; at Amount 0 they don't") {
    for (const float amount : {100.f, 0.f}) {
        Chorus c(kSampleRate, {{"mode", 1.f}, {"amount", amount}, {"rate", 1.f}, {"mix", 100.f}});
        const Samples out = c.play(tone(1000.0, 3.0));
        const std::vector<double> env = envelope(slice(out, samples(1.0), samples(3.0)), 480);
        const double low = *std::min_element(env.begin() + 480, env.end() - 480);
        const double high = *std::max_element(env.begin() + 480, env.end() - 480);
        const double swingDb = 20.0 * std::log10(high / low);
        INFO("Amount " + std::to_string(amount) + ": " + std::to_string(swingDb) + " dB");
        if (amount > 0.f) {
            CHECK(swingDb > 3.0);
        } else {
            CHECK(swingDb < 0.01);
        }
    }
}

TEST_CASE("the chorus's voices detune as far as the design says") {
    // One tap a side, Auto, Amount 100, 2 Hz: the delay swings 5 ms, so it moves
    // at up to m = 0.005 * 2 pi * 2 of a second a second, and a 1 kHz tone plays
    // between 1000 (1 - m) and 1000 (1 + m) Hz; the right side the other way.
    Chorus c(kSampleRate, {{"taps", 0.f}, {"amount", 100.f}, {"rate", 2.f}, {"mix", 100.f}});
    const Samples in = tone(1000.0, 3.0);
    const auto [left, right] = c.play(in, in);
    const double m = 0.005 * 2.0 * kPi * 2.0;
    const auto leftFreq = instantaneousFrequency(left, samples(0.5), kSampleRate);
    const auto rightFreq = instantaneousFrequency(right, samples(0.5), kSampleRate);
    double highest = 0.0, lowest = 1e9;
    for (const auto& [t, f] : leftFreq) {
        highest = std::max(highest, f);
        lowest = std::min(lowest, f);
    }
    INFO("from " + std::to_string(lowest) + " to " + std::to_string(highest) + " Hz");
    CHECK_APPROX_TOL(highest - 1000.0, 1000.0 * m, 0.1, 0.0);
    CHECK_APPROX_TOL(1000.0 - lowest, 1000.0 * m, 0.1, 0.0);
    // The design's figures for it.
    const chorus::Layout l = chorus::layout(0, 0, 0);
    CHECK_APPROX(chorus::detuneUpCents(l, 2.0, 1.0, 0.0), 1200.0 * std::log2(1.0 + m));
    CHECK_APPROX(chorus::peakDetuneCents(l, 2.0, 1.0, 0.0), -1200.0 * std::log2(1.0 - m));
    // The right side moves the other way.
    std::vector<double> a, b;
    for (double t = 0.6; t < 2.9; t += 0.001) {
        a.push_back(valueAt(leftFreq, t) - 1000.0);
        b.push_back(valueAt(rightFreq, t) - 1000.0);
    }
    CHECK(correlation(a, b) < -0.9);
}

TEST_CASE("the chorus's feedback repeats the echoes; Invert flips them; it holds at 100 %") {
    // 10 ms, one tap, Feedback 50: a loop gain of 0.97 * 0.5 (the impulse at 480, the echoes every 480 after).
    for (const bool invert : {false, true}) {
        INFO(invert ? "inverted" : "positive");
        const float on = invert ? 1.f : 0.f;
        Chorus c(kSampleRate, with(kPure, {{"taps", 0.f}, {"time", 2.f}, {"feedback", 50.f}, {"fb_invert", on}}));
        const Samples h = c.play(impulse(2500, 480));
        const float sign = invert ? -1.f : 1.f;
        CHECK_APPROX_TOL(h[960], 1.0, 0.0, 1e-6);
        CHECK_APPROX_TOL(h[1440], sign * 0.485, 0.0, 1e-3);
        CHECK_APPROX_TOL(h[1920], 0.485 * 0.485, 0.0, 1e-3);
        // Between them, only the DC guard's faint tail (under 7e-4 of an echo each time round).
        for (size_t i = 0; i < h.size(); ++i) {
            if (i % 480 != 0) CHECK(std::abs(h[i]) < 1e-3);
        }
    }
    // Vibrato has no feedback (as Live's): Feedback and Invert change nothing, and it has no echoes.
    {
        const Values vibrato = with(kPure, {{"mode", 2.f}, {"offset", 90.f}});
        const Samples in = noise(kSampleRate, 4);
        const Samples none = Chorus(kSampleRate, vibrato).play(in);
        const std::vector<Values> feedback = {{{"feedback", 100.f}}, {{"feedback", 60.f}, {"fb_invert", 1.f}}};
        for (const Values& more : feedback) {
            INFO(describe(more));
            CHECK_ARRAY_EQUAL(Chorus(kSampleRate, with(vibrato, more)).play(in), none);
        }
        const Samples h = Chorus(kSampleRate, with(vibrato, {{"feedback", 100.f}})).play(impulse(4096));
        CHECK_APPROX_TOL(h[kSettled + 288], 1.0, 0.0, 1e-6);  // (6 ms)
        CHECK(allEqual(slice(h, kSettled + 289), 0.0));
    }
    // Feedback 100 on loud noise: stable (the modulation smears the comb, and a loop gain of 0.97 is stable).
    for (const bool invert : {false, true}) {
        Chorus c(kSampleRate, {{"feedback", 100.f}, {"fb_invert", invert ? 1.f : 0.f}, {"mix", 100.f}});
        const auto [l, r] = c.play(noise(10 * kSampleRate, 5), noise(10 * kSampleRate, 6));
        CHECK(allFinite(l) && allFinite(r));
        INFO("peak " + std::to_string(std::max(maxAbs(l), maxAbs(r))));
        CHECK(std::max(maxAbs(l), maxAbs(r)) < 20.0);
    }
    // A tone on the loop's resonance (100 Hz round 10 ms), still: the echoes would build up towards 0.5 / 0.03,
    // about 17 (10 within these 5 s); the limiter holds what is fed back to 2, so the line holds the tone and 2
    // at most.
    {
        Chorus c(kSampleRate, with(kPure, {{"taps", 0.f}, {"time", 2.f}, {"feedback", 100.f}}));
        const Samples y = c.play(tone(100.0, 5.0));
        INFO("peak " + std::to_string(maxAbs(y)));
        CHECK(maxAbs(y) < 2.6);
        CHECK(maxAbs(y) > 2.0);  // (it does build up to the limiter)
    }
    // A DC offset passes once, as through any delay, and doesn't build up round the loop.
    {
        Chorus c(kSampleRate, {{"feedback", 100.f}, {"mix", 100.f}});
        Samples in = noise(10 * kSampleRate, 7, 0.25f);
        for (float& v : in) v += 0.1f;
        const auto [l, r] = c.play(in, in);
        INFO("mean " + std::to_string(mean(slice(l, samples(9.0)))));
        CHECK_APPROX_TOL(mean(slice(l, samples(9.0))), 0.1, 0.0, 0.03);
        CHECK_APPROX_TOL(mean(slice(r, samples(9.0))), 0.1, 0.0, 0.03);
    }
}

TEST_CASE("the chorus's Warmth colours gently, darkens, and doesn't alias much") {
    constexpr size_t kN = 1 << 15;
    const std::vector<double> window = hanning(kN);
    const auto analyse = [&](float warmth, double freq, double amplitude) {
        Chorus c(kSampleRate, with(kPure, {{"taps", 0.f}, {"warmth", warmth}}));
        const Samples out = c.play(tone(freq, 1.5, kSampleRate, amplitude));
        return slice(out, samples(0.5), samples(0.5) + static_cast<int64_t>(kN));
    };
    const double f200 = binFrequency(200.0, kN);
    const size_t k200 = binOf(f200, kN);
    // The fundamental's level against the tone's, in dB, and the THD; `second`: the 2nd harmonic against it.
    const auto harmonics = [&](const Samples& y, double amplitude, double* second) {
        const std::vector<double> s = spectrum(y, window);
        const double fundamental = binLevel(s, k200);
        const double reference = binLevel(spectrum(slice(tone(f200, 1.0, kSampleRate, amplitude), 0,
                                                         static_cast<int64_t>(kN)),
                                                   window),
                                          k200);
        double power = 0.0;
        for (size_t h = 2; h * k200 + 1 < s.size(); ++h) power += std::pow(binLevel(s, h * k200), 2);
        if (second) *second = binLevel(s, 2 * k200) / fundamental;
        return std::pair{20.0 * std::log10(fundamental / reference), std::sqrt(power) / fundamental};
    };
    // Warmth 0: the delayed tone, untouched.
    CHECK(harmonics(analyse(0.f, f200, 0.5), 0.5, nullptr).second < 1e-4);
    // Warmth 100: a little distortion, mostly even (the bias), no DC, and the level kept: a colour, not a
    // limiter (at -6 dBFS 2.5 % and 0.3 dB down; at 0 dBFS 1.1 dB down).
    {
        const Samples y = analyse(100.f, f200, 0.5);
        double second = 0.0;
        const auto [levelDb, distortion] = harmonics(y, 0.5, &second);
        INFO("-6 dBFS: " + std::to_string(levelDb) + " dB, THD " + std::to_string(100.0 * distortion) + " %, 2nd " +
             std::to_string(20.0 * std::log10(second)) + " dBc");
        CHECK(distortion > 0.01);
        CHECK(distortion < 0.04);
        CHECK(20.0 * std::log10(second) > -40.0);
        CHECK(levelDb > -0.6);
        CHECK(std::abs(mean(y)) < 1e-3);
        const auto [fullDb, fullDistortion] = harmonics(analyse(100.f, f200, 1.0), 1.0, nullptr);
        INFO("0 dBFS: " + std::to_string(fullDb) + " dB, THD " + std::to_string(100.0 * fullDistortion) + " %");
        CHECK(fullDb > -1.5);
        CHECK(fullDistortion < 0.08);
    }
    // Quiet, it keeps its level, and the low-pass takes the highs down as its one-pole does.
    {
        const double gain200 = rms(analyse(100.f, f200, 0.01)) / (0.01 / std::sqrt(2.0));
        INFO("200 Hz at -40 dBFS: " + std::to_string(20.0 * std::log10(gain200)) + " dB");
        CHECK(std::abs(20.0 * std::log10(gain200)) < 0.1);
        const double f10k = binFrequency(10000.0, kN);
        const double gain10k = rms(analyse(100.f, f10k, 0.01)) / (0.01 / std::sqrt(2.0));
        const double c = chorus::warmLowpassCoefficient(1.0, kSampleRate);
        const double w = 2.0 * kPi * f10k / kSampleRate;
        const double predicted = (1.0 - c) / std::sqrt(1.0 - 2.0 * c * std::cos(w) + c * c);
        INFO("10 kHz: " + std::to_string(20.0 * std::log10(gain10k)) + " dB, predicted " +
             std::to_string(20.0 * std::log10(predicted)));
        CHECK(std::abs(20.0 * std::log10(gain10k / predicted)) < 0.3);
    }
    // Aliasing: loud tones' harmonics above Nyquist fold back far down (the curve is anti-aliased through
    // its antiderivative): every component neither a harmonic below Nyquist nor DC, against the tone.
    for (const auto& [approx, limitDb] : {std::pair{5000.0, -70.0}, {9000.0, -45.0}, {13000.0, -42.0}}) {
        const double f = binFrequency(approx, kN);
        const size_t k = binOf(f, kN);
        const std::vector<double> s = spectrum(analyse(100.f, f, 0.5), window);
        double worst = 0.0;
        for (size_t b = 3; b < s.size(); ++b) {
            const size_t nearest = (b + k / 2) / k * k;  // the nearest harmonic below Nyquist
            if (nearest > 0 && (b + 2 >= nearest && b <= nearest + 2)) continue;
            worst = std::max(worst, s[b]);
        }
        const double aliasDb = 20.0 * std::log10(worst / s[k]);
        INFO(std::to_string(approx) + " Hz: the largest alias " + std::to_string(aliasDb) + " dB under the tone");
        CHECK(aliasDb < limitDb);
    }
}

TEST_CASE("the chorus's high-pass keeps the lows out of the delays") {
    // Amount 0, 10 ms: the output is the crossover's lows now plus its highs 10 ms ago.
    const Samples in = settled(noise(kSampleRate, 8));
    Chorus c(kSampleRate, with(kPure, {{"time", 2.f}, {"hp", 1.f}, {"hp_freq", 1000.f}}));
    const Samples out = c.play(in);
    sub::dsp::Crossover crossover;
    const sub::dsp::CrossoverCoefficients coefficients = sub::dsp::CrossoverCoefficients::at(1000.0, kSampleRate);
    Samples low(in.size()), high(in.size()), want(in.size());
    for (size_t i = 0; i < in.size(); ++i) crossover.process(coefficients, in[i], low[i], high[i]);
    for (size_t i = 0; i < in.size(); ++i) want[i] = low[i] + (i >= 480 ? high[i - 480] : 0.f);
    CHECK_ALLCLOSE(out, want, 0.0, 1e-5);

    // A 60 Hz tone through a deep, fast chorus: on, it comes out a clean sinusoid; off, it is chorused.
    for (const float on : {1.f, 0.f}) {
        Chorus deep(kSampleRate, {{"amount", 100.f}, {"rate", 5.f}, {"mix", 100.f}, {"hp", on}, {"hp_freq", 1000.f}});
        const Samples y = deep.play(tone(60.0, 2.0));
        const double rest = residualDb(slice(y, samples(0.5)), 60.0);
        INFO((on > 0.f ? "on: " : "off: ") + std::to_string(rest) + " dB");
        if (on > 0.f) {
            CHECK(rest < -40.0);
        } else {
            CHECK(rest > -20.0);
        }
    }

    // Half wet, the lows below the high-pass come out whole: the dry goes through the crossover too, in phase
    // with the wet's lows (a dry left as it was would cancel them round the frequency, by 13 dB at it).
    for (const float amount : {0.f, 50.f}) {
        for (const double freq : {100.0, 1000.0}) {
            for (const double ratio : {0.5, 0.7}) {
                Chorus c(kSampleRate, {{"amount", amount}, {"mix", 50.f}, {"hp", 1.f},
                                       {"hp_freq", static_cast<float>(freq)}});
                const Samples in = tone(ratio * freq, 2.0);
                const Samples y = c.play(in);
                const double gainDb = 20.0 * std::log10(rms(slice(y, samples(1.0))) / rms(slice(in, samples(1.0))));
                INFO("Amount " + std::to_string(amount) + ", " + std::to_string(ratio * freq) + " Hz under " +
                     std::to_string(freq) + " Hz: " + std::to_string(gainDb) + " dB");
                CHECK(std::abs(gainDb) < 1.2);
            }
        }
    }
}

TEST_CASE("the chorus's Width, and one channel or three") {
    const Samples a = noise(kSampleRate, 9), b = noise(kSampleRate, 10);
    const auto playWidth = [&](float width) {
        Chorus c(kSampleRate, {{"width", width}, {"mix", 100.f}, {"feedback", 30.f}});
        return c.play(a, b);
    };
    const auto [l0, r0] = playWidth(0.f);
    CHECK_ALLCLOSE(l0, r0, 0.0, 1e-6);  // mono
    const auto [l1, r1] = playWidth(100.f);
    const auto [l2, r2] = playWidth(200.f);
    Samples side1(a.size()), side2(a.size()), mid1(a.size()), mid2(a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        side1[i] = 2.f * (l1[i] - r1[i]);
        side2[i] = l2[i] - r2[i];
        mid1[i] = l1[i] + r1[i];
        mid2[i] = l2[i] + r2[i];
    }
    CHECK_ALLCLOSE(side2, side1, 0.0, 1e-5);  // the side doubled
    CHECK_ALLCLOSE(mid2, mid1, 0.0, 1e-5);    // the middle as it was

    // Width is Chorus's and Ensemble's: in Vibrato it changes nothing (the sides Offset apart stay as they are).
    {
        const Values vibrato = {{"mode", 2.f}, {"offset", 90.f}, {"mix", 100.f}};
        const auto [lw, rw] = Chorus(kSampleRate, with(vibrato, {{"width", 0.f}})).play(a, b);
        const auto [ln, rn] = Chorus(kSampleRate, with(vibrato, {{"width", 100.f}})).play(a, b);
        CHECK_ARRAY_EQUAL(lw, ln);
        CHECK_ARRAY_EQUAL(rw, rn);
    }

    // A third channel passes untouched, fully wet too.
    {
        const Samples third = noise(kSampleRate, 28);
        Samples l = a, r = b, t = third;
        Chorus(kSampleRate, {{"mix", 100.f}, {"feedback", 50.f}, {"warmth", 50.f}}).run({&l, &r, &t});
        CHECK_ARRAY_EQUAL(t, third);
        CHECK(!allclose(l, a, 0.0, 1e-3));
    }

    // One channel plays as the left of two with the same input on both sides.
    const std::vector<Change> changes = {{9000, "amount", 90.f}, {20000, "mode", 1.f}, {30000, "warmth", 40.f}};
    for (const float mode : {0.f, 1.f, 2.f}) {
        INFO(std::to_string(mode));
        const Values values = {{"mode", mode}, {"feedback", 60.f}, {"hp", 1.f}, {"offset", 120.f}};
        Chorus mono(kSampleRate, values), stereo(kSampleRate, values);
        const Samples one = mono.play(a, changes);
        const auto [left, right] = stereo.play(a, a, changes);
        CHECK_ALLCLOSE(one, left, 0.0, 1e-6);
    }
    // A channel count that changes starts the lines afresh: no history from before.
    Chorus switching(kSampleRate, kPure);
    switching.play(noise(4800, 11), noise(4800, 12));
    Samples silent(4800, 0.f);
    CHECK(allEqual(switching.play(silent), 0.0));
}

TEST_CASE("the chorus's Output sets the wet's level; Dry/Wet blends it with the input") {
    const Samples in = settled(noise(kSampleRate, 13));
    Chorus quieter(kSampleRate, with(kPure, {{"time", 2.f}, {"output", -6.f}}));
    const Samples out = quieter.play(in);
    Samples want(in.size(), 0.f);
    const float g = sub::dbToGain(-6.f);
    for (size_t i = 480; i < in.size(); ++i) want[i] = g * in[i - 480];
    CHECK_ALLCLOSE(out, want, 0.0, 1e-6);

    Chorus half(kSampleRate, with(kPure, {{"time", 2.f}, {"mix", 50.f}}));
    const Samples blend = half.play(in);
    for (size_t i = 0; i < in.size(); ++i) want[i] = 0.5f * in[i] + (i >= 480 ? 0.5f * in[i - 480] : 0.f);
    CHECK_ALLCLOSE(blend, want, 0.0, 1e-6);
}

TEST_CASE("changing any chorus control is click-free") {
    // A 440 Hz tone through a moderate chorus, each control jumping as
    // automation's steps make it at 0.5 s. Around the change the output is no
    // clickier than twice the clickier of the steady renders at the settings
    // before and after, plus 1e-5 (a step of 1e-6, a kink of 2e-6: a linear
    // 20 ms ramp of Dry/Wet would make 3e-3).
    struct Case {
        std::string what;
        Values before;
        std::vector<Change> changes;
        Samples (*input)() = nullptr;
    };
    const auto s = [](double seconds) { return samples(seconds); };
    const Values base = {{"amount", 50.f}, {"feedback", 50.f}};
    const Values loud = {{"amount", 50.f}, {"feedback", 90.f}};
    const std::vector<Case> cases = {
        {"Mode to Ensemble, then Vibrato", base, {{s(0.5), "mode", 1.f}, {s(1.5), "mode", 2.f}}},
        {"Taps 2 to 1", base, {{s(0.5), "taps", 0.f}}},
        {"Time Auto to 50 ms", base, {{s(0.5), "time", 5.f}}},
        {"Invert on at Feedback 90", loud, {{s(0.5), "fb_invert", 1.f}}},
        {"Chorus to Vibrato with Invert on at Feedback 90", with(loud, {{"fb_invert", 1.f}}), {{s(0.5), "mode", 2.f}}},
        {"Vibrato to Chorus at Feedback 90", with(loud, {{"mode", 2.f}}), {{s(0.5), "mode", 0.f}}},
        {"High-pass on, then off", base, {{s(0.5), "hp", 1.f}, {s(1.5), "hp", 0.f}}},
        {"Rate 0.1 to 15 Hz", with(base, {{"rate", 0.1f}}), {{s(0.5), "rate", 15.f}}},
        {"Rate 15 to 0.1 Hz", with(base, {{"rate", 15.f}}), {{s(0.5), "rate", 0.1f}}},
        {"Amount 0 to 100, Auto", with(base, {{"amount", 0.f}}), {{s(0.5), "amount", 100.f}}},
        {"Amount 100 to 0, Auto", with(base, {{"amount", 100.f}}), {{s(0.5), "amount", 0.f}}},
        {"Amount 0 to 100, 10 ms", with(base, {{"amount", 0.f}, {"time", 2.f}}), {{s(0.5), "amount", 100.f}}},
        {"Width 0 to 200", with(base, {{"width", 0.f}}), {{s(0.5), "width", 200.f}}},
        {"Width 200 to 0", with(base, {{"width", 200.f}}), {{s(0.5), "width", 0.f}}},
        {"Offset 0 to 180, Vibrato", with(base, {{"mode", 2.f}}), {{s(0.5), "offset", 180.f}}},
        {"Offset 180 to 0, Vibrato", with(base, {{"mode", 2.f}, {"offset", 180.f}}), {{s(0.5), "offset", 0.f}}},
        {"Shape 0 to 100, Vibrato", with(base, {{"mode", 2.f}, {"amount", 100.f}}), {{s(0.5), "shape", 100.f}}},
        {"Shape 100 to 0, Vibrato",
         with(base, {{"mode", 2.f}, {"amount", 100.f}, {"shape", 100.f}}),
         {{s(0.5), "shape", 0.f}}},
        {"Warmth 0 to 100", base, {{s(0.5), "warmth", 100.f}}},
        {"Warmth 100 to 0", with(base, {{"warmth", 100.f}}), {{s(0.5), "warmth", 0.f}}},
        {"Output -36 to +6 dB", with(base, {{"output", -36.f}}), {{s(0.5), "output", 6.f}}},
        {"Output +6 to -36 dB", with(base, {{"output", 6.f}}), {{s(0.5), "output", -36.f}}},
        {"Dry/Wet 0 to 100", with(base, {{"mix", 0.f}}), {{s(0.5), "mix", 100.f}}},
        {"Dry/Wet 100 to 0", with(base, {{"mix", 100.f}}), {{s(0.5), "mix", 0.f}}},
        {"High-pass 20 to 2000 Hz", with(base, {{"hp", 1.f}, {"hp_freq", 20.f}}), {{s(0.5), "hp_freq", 2000.f}}},
        {"High-pass 2000 to 20 Hz", with(base, {{"hp", 1.f}, {"hp_freq", 2000.f}}), {{s(0.5), "hp_freq", 20.f}}},
        {"Warmth 50 to 0 on a 10 kHz tone",
         {{"amount", 0.f}, {"warmth", 50.f}, {"mix", 100.f}},
         {{s(0.5), "warmth", 0.f}},
         [] { return tone(10000.0, 2.0); }},
    };
    for (const Case& k : cases) {
        INFO(k.what);
        const Samples in = k.input ? k.input() : tone(440.0, 2.0);
        Chorus changing(kSampleRate, k.before);
        const Samples out = changing.play(in, k.changes);
        CHECK(allFinite(out));
        // The settings after each change, steady.
        std::vector<Samples> steady;
        steady.push_back(Chorus(kSampleRate, k.before).play(in));
        Values after = k.before;
        for (const Change& change : k.changes) {
            after.emplace_back(change.id, change.value);
            steady.push_back(Chorus(kSampleRate, after).play(in));
        }
        for (const Change& change : k.changes) {
            const int64_t from = change.frame - s(0.1), to = change.frame + s(0.2);
            double calm = 0.0;
            for (const Samples& y : steady) calm = std::max(calm, clickiness(y, from, to));
            const double measured = clickiness(out, from, to);
            INFO("at " + std::to_string(change.frame) + ": " + std::to_string(measured) + " against steady " +
                 std::to_string(calm));
            CHECK(measured < 2.0 * calm + 1e-5);
        }
    }

    // What the measure makes of a click: Chorus switched to Vibrato at once, unfaded.
    const Samples in = tone(440.0, 2.0);
    Samples spliced = Chorus(kSampleRate, {{"amount", 50.f}, {"feedback", 50.f}}).play(in);
    const Samples vibrato = Chorus(kSampleRate, {{"amount", 50.f}, {"feedback", 50.f}, {"mode", 2.f}}).play(in);
    std::copy(vibrato.begin() + s(0.5), vibrato.end(), spliced.begin() + s(0.5));
    CHECK(clickiness(spliced, s(0.4), s(0.7)) > 1000 * 1e-5);
}

TEST_CASE("the chorus's automation plays through the engine sample-accurately") {
    sub::Engine engine;
    engine.setClipFadeMs(0);
    const std::string path = makeWav(interleave({tone(440.0, 2.0), tone(660.0, 2.0)}), 2);
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, 0.0, 2.0, 0.0, 1.f)});
    const Samples untouched = engine.renderOffline(0.0, 2 * kSampleRate);

    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "chorus", -1);
    setParam(engine, id, "mix", 0.f);
    setParam(engine, id, "feedback", 40.f);
    // Dry/Wet: 0 until beat 1 (24000 samples), then 100.
    using Points = std::vector<sub::AutomationPoint>;
    engine.setTrackAutomation(track, {{id, "mix", Points{{0.0, 0.f, 0.f}, {1.0, 0.f, 0.f}, {1.0, 1.f, 0.f}}}});
    const Samples out = engine.renderOffline(0.0, 2 * kSampleRate);
    CHECK(allFinite(out));
    CHECK_ARRAY_EQUAL(frames(out, 0, 24000), frames(untouched, 0, 24000));  // the input, bit for bit
    CHECK(!allclose(frames(out, 24000, 24100), frames(untouched, 24000, 24100), 0.0, 1e-7));
    // Once Dry/Wet has glided there (two one-poles of 6 ms: 125 ms), as a render fully wet throughout.
    engine.setTrackAutomation(track, {});
    setParam(engine, id, "mix", 100.f);
    const Samples wet = engine.renderOffline(0.0, 2 * kSampleRate);
    CHECK_ALLCLOSE(frames(out, 24000 + 6000), frames(wet, 24000 + 6000), 0.0, 1e-6);
    // And no click where it changes.
    for (int c = 0; c < 2; ++c) {
        INFO(std::to_string(c));
        const double calm =
            std::max(clickiness(channel(untouched, c), 20000, 30000), clickiness(channel(wet, c), 20000, 30000));
        CHECK(clickiness(channel(out, c), 20000, 30000) < 2.0 * calm + 1e-5);
    }
}

TEST_CASE("reset and a new sample rate start the chorus from silence") {
    Chorus c(kSampleRate, {{"feedback", 80.f}, {"warmth", 30.f}, {"hp", 1.f}});
    c.play(noise(kSampleRate, 14), noise(kSampleRate, 15));
    c.processor().reset();
    const auto [l, r] = c.play(Samples(kSampleRate, 0.f), Samples(kSampleRate, 0.f));
    CHECK(allEqual(l, 0.0));
    CHECK(allEqual(r, 0.0));

    // The LFO restarts too: the same input after a reset renders the same, bit for bit.
    const Samples in = noise(kSampleRate, 16);
    c.processor().reset();
    const Samples first = c.play(in);
    c.processor().reset();
    CHECK_ARRAY_EQUAL(c.play(in), first);

    // A new rate: the delays in samples follow it.
    c.processor().prepare(96000.0, kBlock);
    for (const auto& [id, value] : with(kPure, {{"taps", 0.f}, {"amount", 100.f}, {"rate", 1.f}})) c.set(id, value);
    c.processor().reset();
    const auto n = static_cast<size_t>(2 * 96000);
    Samples x(n);
    for (size_t j = 0; j < n; ++j) x[j] = static_cast<float>((static_cast<double>(j % 16384) - 8192.0) / 16384.0);
    const Samples y = c.play(x);
    double worst = 0.0;
    for (size_t j = 3000; j < n; ++j) {
        const double segment = 16384.0 * std::floor(j / 16384.0);
        if (static_cast<double>(j) - 1104 - 3 < segment) continue;
        const double d = static_cast<double>(j) - (16384.0 * y[j] + 8192.0 + segment);
        worst = std::max(worst, std::abs(d - (624.0 + 480.0 * std::sin(2.0 * kPi * (j + 1) / 96000.0))));
    }
    CHECK(worst < 0.02);
}

TEST_CASE("the chorus stays stable at the extremes and at any sample rate") {
    const Values lowest = {{"mode", 0.f}, {"taps", 0.f}, {"time", 0.f}, {"rate", 0.1f}, {"amount", 0.f},
                           {"feedback", 0.f}, {"fb_invert", 0.f}, {"width", 0.f}, {"offset", 0.f}, {"shape", 0.f},
                           {"warmth", 0.f}, {"hp", 0.f}, {"hp_freq", 20.f}, {"output", -36.f}, {"mix", 0.f}};
    const Values highest = {{"mode", 0.f}, {"taps", 1.f}, {"time", 5.f}, {"rate", 15.f}, {"amount", 100.f},
                            {"feedback", 100.f}, {"fb_invert", 1.f}, {"width", 200.f}, {"warmth", 100.f},
                            {"hp", 1.f}, {"hp_freq", 2000.f}, {"output", 6.f}, {"mix", 100.f}};
    const Values vibrato = with(highest, {{"mode", 2.f}, {"shape", 100.f}, {"offset", 180.f}});
    const Values ensemble = with(highest, {{"mode", 1.f}, {"time", 0.f}});
    const Values lowestWet = with(lowest, {{"mix", 100.f}});
    for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
        for (const Values* values : {&lowest, &lowestWet, &highest, &vibrato, &ensemble}) {
            INFO(std::to_string(rate) + ": " + describe(*values));
            Chorus c(rate, *values);
            const auto length = static_cast<size_t>(5 * rate);
            const auto [l, r] = c.play(noise(length, 17, 1.f), noise(length, 18, 1.f));
            CHECK(allFinite(l) && allFinite(r));
            const double peak = std::max(maxAbs(l), maxAbs(r));
            INFO("peak " + std::to_string(peak));
            CHECK(peak < 20.0);
            const auto second = static_cast<int64_t>(rate);
            const double first = energy(slice(l, 0, second)) + energy(slice(r, 0, second));
            const double last = energy(slice(l, 4 * second)) + energy(slice(r, 4 * second));
            INFO("energy, first second " + std::to_string(first) + ", last " + std::to_string(last));
            CHECK(last <= 2.0 * first);
        }
    }
    // Every control jumping at random, every 1 to 50 ms, for 10 s: finite and bounded.
    {
        std::mt19937 random(24);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        Chorus c(kSampleRate);
        const auto& params = c.processor().params();
        std::vector<Change> changes;
        for (int64_t at = 0; at < 10 * kSampleRate;) {
            at += static_cast<int64_t>(kSampleRate * (0.001 + 0.049 * unit(random)));
            const sub::ParamInfo& p = params[static_cast<size_t>(unit(random) * params.size()) % params.size()];
            changes.push_back({at, p.id, p.fromNormalized(static_cast<float>(unit(random)))});
        }
        const auto [l, r] = c.play(noise(10 * kSampleRate, 25), noise(10 * kSampleRate, 26), changes, 128);
        CHECK(allFinite(l) && allFinite(r));
        INFO("peak " + std::to_string(std::max(maxAbs(l), maxAbs(r))));
        CHECK(std::max(maxAbs(l), maxAbs(r)) < 20.0);
    }
    // Mode turning round every 64 samples: each fade waits for the last.
    std::vector<Change> changes;
    for (int64_t at = 64, k = 1; at < kSampleRate; at += 64, ++k)
        changes.push_back({at, "mode", static_cast<float>(k % 3)});
    Chorus c(kSampleRate, {{"amount", 100.f}, {"feedback", 70.f}, {"mix", 100.f}});
    const auto [l, r] = c.play(noise(kSampleRate, 19), noise(kSampleRate, 20), changes);
    CHECK(allFinite(l) && allFinite(r));
    CHECK(std::max(maxAbs(l), maxAbs(r)) < 4.0);
}

TEST_CASE("the chorus plays the same in blocks of any size") {
    // The modulation is worked out per chunk of up to 16 samples, which a block's
    // end (or automation) cuts short; the delays follow the same curve however
    // the chunks fall, so what comes out differs only by rounding.
    const Values values = {{"mode", 1.f}, {"amount", 100.f}, {"rate", 3.f}, {"feedback", 70.f}, {"warmth", 40.f},
                           {"hp", 1.f}, {"width", 150.f}, {"mix", 100.f}};
    const std::vector<Change> changes = {{7001, "amount", 30.f}, {13333, "warmth", 0.f}, {20000, "mode", 2.f},
                                         {30011, "hp_freq", 900.f}};
    const Samples in = noise(kSampleRate, 27);
    const Samples want = Chorus(kSampleRate, values).play(in, changes, kBlock);
    for (const int block : {1, 7, 100, 256}) {
        INFO(std::to_string(block));
        CHECK_ALLCLOSE(Chorus(kSampleRate, values).play(in, changes, block), want, 0.0, 5e-5);
    }
}

TEST_CASE("silence rings out of the chorus to exact zeros") {
    // After a sound the lines drain once the feedback has died below its gate,
    // and every filter is flushed: the output is exact zeros, never denormals.
    Chorus c(kSampleRate, {{"feedback", 90.f}, {"warmth", 50.f}, {"hp", 1.f}, {"mix", 100.f}});
    const int tail = c.processor().tailSamples();
    Samples l = noise(kSampleRate, 21), r = noise(kSampleRate, 22);
    l.resize(static_cast<size_t>(8 * kSampleRate), 0.f);
    r.resize(static_cast<size_t>(8 * kSampleRate), 0.f);
    const auto [left, right] = c.play(l, r);
    for (const Samples* side : {&left, &right}) {
        const std::vector<int64_t> sounding = nonzero(*side);
        REQUIRE(!sounding.empty());
        INFO("last sound " + std::to_string(sounding.back() - kSampleRate) + " samples after the input, the tail " +
             std::to_string(tail));
        CHECK(sounding.back() < kSampleRate + tail + 3 * kSampleRate);
        for (const float v : *side) CHECK(v == 0.f || std::abs(v) >= std::numeric_limits<float>::min());
    }
    // Denormal input passes as finite numbers, and dies away to zeros too.
    Samples tiny(static_cast<size_t>(2 * kSampleRate), 0.f);
    for (size_t i = 0; i < 1000; ++i) tiny[i] = (i % 2 ? 1e-40f : -1e-41f);
    const Samples quiet = c.play(tiny);
    CHECK(allFinite(quiet));
    CHECK(allEqual(slice(quiet, kSampleRate), 0.0));
}

TEST_CASE("the chorus's tail covers its echoes; it has no latency") {
    Chorus defaults;
    CHECK_EQ(defaults.processor().latencySamples(), 0);
    CHECK_EQ(defaults.processor().tailSamples(), 2952);  // 11.5 ms, and 50 ms for the filters
    Chorus dry(kSampleRate, {{"mix", 0.f}});
    CHECK_EQ(dry.processor().tailSamples(), 0);
    Chorus dryFiltered(kSampleRate, {{"mix", 0.f}, {"hp", 1.f}});
    CHECK_EQ(dryFiltered.processor().tailSamples(), 2400);  // (the dry's crossover)
    Chorus longest(kSampleRate, {{"time", 5.f}, {"feedback", 100.f}});
    CHECK(longest.processor().tailSamples() > 12 * kSampleRate);
    CHECK(longest.processor().tailSamples() <= 60 * kSampleRate);

    // An impulse at Feedback 100, each layout at its deepest: after the tail,
    // what is left is 60 dB under its first echo.
    const std::vector<Values> layouts = {{{"taps", 1.f}},
                                         {{"taps", 0.f}, {"time", 5.f}},
                                         {{"mode", 1.f}},
                                         {{"mode", 2.f}, {"offset", 90.f}},
                                         {{"taps", 0.f}, {"time", 1.f}, {"warmth", 100.f}, {"fb_invert", 1.f}}};
    for (const Values& layout : layouts) {
        INFO(describe(layout));
        Chorus c(kSampleRate, with(with(kPure, {{"amount", 100.f}, {"feedback", 100.f}}), layout));
        const auto tail = static_cast<int64_t>(c.processor().tailSamples());
        const Samples h = c.play(impulse(static_cast<size_t>(kSettled + tail + kSampleRate)));
        const double first = maxAbs(slice(h, kSettled, kSettled + 2700));
        const double after = maxAbs(slice(h, kSettled + tail));
        INFO("first echo " + std::to_string(first) + ", after the tail " + std::to_string(after));
        CHECK(after < 1e-3 * first);
    }
}

TEST_CASE("the chorus's displays: the LFO's phase and the wet's level") {
    // At 1.5 Hz, value k is the phase after 128 (k + 1) samples, however the blocks fall.
    const Samples in = noise(kSampleRate, 23);
    struct Run {
        int block;
        std::vector<Change> changes;
    };
    const std::vector<Run> runs = {
        {256, {}}, {1000, {}}, {256, {{1001, "mix", 30.f}, {5003, "mix", 70.f}, {17777, "mix", 40.f}}}};
    for (const Run& run : runs) {
        INFO(std::to_string(run.block) + " " + std::to_string(run.changes.size()));
        Chorus c(kSampleRate, {{"rate", 1.5f}});
        c.play(in, in, run.changes, run.block);
        const std::vector<float> phase = c.display(0), level = c.display(1);
        REQUIRE(phase.size() == 375);
        CHECK_EQ(level.size(), size_t{375});
        double worst = 0.0;
        for (size_t k = 0; k < phase.size(); ++k) {
            CHECK(phase[k] >= 0.f && phase[k] < 1.f);
            const double error = frac(phase[k] - 1.5 * 128.0 * static_cast<double>(k + 1) / kSampleRate);
            worst = std::max(worst, std::min(error, 1.0 - error));  // (round the cycle)
        }
        CHECK(worst < 1e-6);
    }
    // The level: the wet's peak after Output, in dB.
    for (const float output : {0.f, -12.f}) {
        INFO("Output " + std::to_string(output));
        Chorus c(kSampleRate, with(kPure, {{"taps", 0.f}, {"output", output}}));
        c.play(tone(1000.0, 1.0));
        const std::vector<float> level = c.display(1);
        REQUIRE(level.size() == 375);
        for (size_t k = 38; k < level.size(); ++k) CHECK_APPROX_TOL(level[k], -6.02 + output, 0.0, 0.2);
    }
    Chorus c(kSampleRate, with(kPure, {{"taps", 0.f}}));
    c.play(tone(1000.0, 1.0));
    c.display(1);
    c.play(Samples(kSampleRate, 0.f));
    const std::vector<float> silent = c.display(1);
    for (size_t k = 10; k < silent.size(); ++k) CHECK_EQ(silent[k], -90.f);
}

TEST_CASE("the chorus switched on in the middle of a sound doesn't click") {
    // As the renderer switches a device on: reset at a block's start, its output then faded in from its input
    // over 5 ms. Its lines start empty; what goes into them fades in too, so each voice's copy of the sound
    // starts as smoothly, a delay later (mostly after the renderer's fade), instead of with a step mid-waveform.
    // The largest step from one sample to the next, from 10 ms before the switch until 20 ms after the longest
    // delay, is no larger than 1.25 times the larger of the tone's own and the device's always on, plus 0.005.
    const Samples in = tone(440.0, 1.0, kSampleRate, 0.3);
    const int64_t at = 24064;          // (a block's start)
    const auto fade = samples(0.005);  // Renderer::kSwitchFade
    const std::vector<Values> settings = {
        {},
        {{"mode", 1.f}},
        {{"mode", 2.f}},
        {{"mode", 2.f}, {"amount", 100.f}, {"offset", 180.f}, {"mix", 100.f}},
        {{"time", 5.f}},
        {{"time", 5.f}, {"taps", 0.f}, {"amount", 100.f}, {"feedback", 80.f}, {"mix", 100.f}},
        {{"time", 3.f}, {"hp", 1.f}, {"warmth", 100.f}, {"feedback", 50.f}, {"fb_invert", 1.f}},
        {{"mix", 100.f}, {"amount", 100.f}, {"rate", 15.f}},
    };
    for (const Values& values : settings) {
        INFO(describe(values));
        const Samples on = Chorus(kSampleRate, values).play(in);
        Chorus c(kSampleRate, values);
        c.play(slice(in, 0, at));  // (heard before it was switched off)
        c.processor().reset();
        const Samples rest = slice(in, at);
        const Samples y = c.play(rest);
        Samples switched = in;
        for (size_t s = 0; s < y.size(); ++s) {
            const float gain = std::min(1.f, static_cast<float>(s + 1) / static_cast<float>(fade));
            switched[static_cast<size_t>(at) + s] = rest[s] + gain * (y[s] - rest[s]);
        }
        const double longest = chorus::kMaxDelayMs;  // (50 ms and its swing, at most)
        const int64_t from = at - samples(0.01), to = at + samples((longest + 20.0) / 1000.0);
        const double steepest = std::max(largestStep(on, from, to), largestStep(in, from, to));
        const double step = largestStep(switched, from, to);
        INFO("a step of " + std::to_string(step) + ", " + std::to_string(steepest) + " without the switch");
        CHECK(step <= 1.25 * steepest + 0.005);
    }
}

TEST_CASE("the chorus takes NaN and infinity in its input as silence") {
    // BuiltinProcessor::process() takes what isn't audio (NaN, infinity, beyond 1e30) as 0 before the device
    // sees it, so nothing of it stays in the lines, the loop or the filters: what comes out is what the same
    // input with 0 there gives, bit for bit, through the sound and the silence after it.
    const std::vector<Values> settings = {
        {},
        {{"feedback", 100.f}, {"warmth", 100.f}, {"hp", 1.f}, {"mix", 100.f}},
        {{"mode", 1.f}, {"feedback", 90.f}, {"fb_invert", 1.f}, {"width", 200.f}},
        {{"mode", 2.f}, {"amount", 100.f}, {"shape", 50.f}, {"mix", 0.f}, {"hp", 1.f}},
    };
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity(), 3e38f}) {
        for (const Values& values : settings) {
            INFO(std::to_string(bad) + ": " + describe(values));
            Samples left = noise(kSampleRate, 29), right = noise(kSampleRate, 30);
            left.resize(static_cast<size_t>(2 * kSampleRate), 0.f);
            right.resize(static_cast<size_t>(2 * kSampleRate), 0.f);
            Samples cleanLeft = left, cleanRight = right;
            for (const size_t i : {size_t{1000}, size_t{20000}}) {
                left[i] = right[i + 7] = bad;
                cleanLeft[i] = cleanRight[i + 7] = 0.f;
            }
            const auto [l, r] = Chorus(kSampleRate, values).play(left, right);
            const auto [cl, cr] = Chorus(kSampleRate, values).play(cleanLeft, cleanRight);
            CHECK(allFinite(l) && allFinite(r));
            CHECK_ARRAY_EQUAL(l, cl);
            CHECK_ARRAY_EQUAL(r, cr);
        }
    }
}
