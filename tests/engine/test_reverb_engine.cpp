// The built-in Reverb, after Live's: mono in, stereo out. Its tail decays per
// band as the design says (the curve its editor draws), the reflections land
// where earlyTaps() puts them, Shape moves the diffuse onset, the input filter
// is the band it draws; Stereo, one channel, the levels; Freeze, Cut and Flat;
// the guard; each Density, and a change of it keeping a frozen tail; Spin
// swinging and drifting the reflections as spinPan() and spinDriftMs() say (and
// reaching the tail); Chorus, Diffusion and Scale each doing what they say; no
// metallic ringing; every control and switch changing without a click;
// automation to the sample; reset and a new rate; extremes; input that isn't
// audio (NaN, infinity) taken as silence; silence ringing out to exact zeros and
// waking into silence; its tail; its displays; what it costs.

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "Engine.h"
#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/Dsp.h"
#include "builtin/ReverbDesign.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"
#include "harness/Standalone.h"
#include "harness/ThreadTime.h"
#include "rt/RtUtils.h"

using namespace subtest;
namespace reverb = sub::reverb;

namespace {

using Values = ParamValues;
using Change = ParamChange;

// What most cases start from: nothing moves by itself (Spin and Chorus off),
// the input unfiltered, the reflections at their least (-30 dB), all wet.
const Values kQuiet = {{"spin", 0.f}, {"chorus", 0.f}, {"lo_cut", 0.f}, {"hi_cut", 0.f}, {"reflect", -30.f},
                       {"mix", 100.f}};

int64_t frames(double seconds, double rate = kSampleRate) { return static_cast<int64_t>(std::lround(seconds * rate)); }

// A Reverb on its own, outside an engine (harness/Standalone.h), the renderer's
// flush-to-zero on as it renders; played in stereo too.
class Reverb : public Standalone {
public:
    explicit Reverb(const Values& values = {}, double rate = kSampleRate) : Standalone("reverb", rate, values) {}

    void run(const std::vector<Samples*>& channels, const std::vector<Change>& changes = {}, int block = 256,
             const BeforeBlock& beforeBlock = {}) {
        const sub::ScopedNoDenormals noDenormals;
        Standalone::run(channels, changes, block, beforeBlock);
    }
    // One channel: what comes out.
    Samples play(Samples mono, const std::vector<Change>& changes = {}, int block = 256) {
        run({&mono}, changes, block);
        return mono;
    }
    // Two channels: what comes out of each.
    std::pair<Samples, Samples> play(Samples left, Samples right, const std::vector<Change>& changes = {},
                                     int block = 256) {
        run({&left, &right}, changes, block);
        return {std::move(left), std::move(right)};
    }
};

Samples silence(Samples x, double seconds, double rate = kSampleRate) {
    x.resize(x.size() + static_cast<size_t>(seconds * rate), 0.f);
    return x;
}

// An energy (power) ratio in dB.
double powerDb(double ratio) { return 10.0 * std::log10(std::max(ratio, 1e-300)); }

// A band around `freq`, `octaves` wide: three RBJ band-passes (0 dB at their
// centre) in a row, each of Q 0.51 / (2^(w/2) - 2^(-w/2)) so the three are
// 3 dB down at the band's edges. Steep (an octave band: -22 dB 1.7 octaves
// away; a third: -49 dB), so a band's decay is its own: where the decay time
// changes fast with frequency (a shelf's or a low-pass's skirt), a single
// section's skirts let the slower neighbours' energy outlast the band's own.
std::vector<double> band(const Samples& x, double freq, double rate, double octaves) {
    const double q = 0.5098 / (std::exp2(0.5 * octaves) - std::exp2(-0.5 * octaves));
    const double w = 2.0 * kPi * freq / rate, alpha = std::sin(w) / (2.0 * q), cosw = std::cos(w);
    const double a0 = 1.0 + alpha;
    const double b0 = alpha / a0, b2 = -alpha / a0, a1 = -2.0 * cosw / a0, a2 = (1.0 - alpha) / a0;
    std::vector<double> y(x.begin(), x.end());
    for (int section = 0; section < 3; ++section) {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (double& v : y) {
            const double in = v;
            const double out = b0 * in + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = in;
            y2 = y1;
            y1 = out;
            v = out;
        }
    }
    return y;
}

// The decay time (s, to -60 dB) of an impulse response around `freq` Hz (0:
// all of it): Schroeder's backward integration of its square in a band (a
// third of an octave from 500 Hz up; below, where a third holds too few of the
// network's modes to average their beating out, an octave), a line fitted to
// that in dB from -5 to -25 dB. Given both sides (two different sums of the
// network's lines), their energies add: a steadier reading low down.
double t60(const std::vector<const Samples*>& sides, double freq, double rate = kSampleRate) {
    std::vector<double> energy(sides[0]->size(), 0.0);
    for (const Samples* h : sides) {
        const std::vector<double> x = freq > 0.0 ? band(*h, freq, rate, freq < 500.0 ? 1.0 : 1.0 / 3.0)
                                                 : std::vector<double>(h->begin(), h->end());
        for (size_t i = 0; i < x.size(); ++i) energy[i] += x[i] * x[i];
    }
    std::vector<double> curve(energy.size());
    double sum = 0.0;
    for (size_t i = energy.size(); i-- > 0;) {
        sum += energy[i];
        curve[i] = sum;
    }
    double st = 0, sy = 0, stt = 0, sty = 0, count = 0;
    for (size_t i = 0; i < curve.size(); ++i) {
        const double level = powerDb(curve[i] / curve[0]);
        if (level > -5.0) continue;
        if (level < -25.0) break;
        const double t = static_cast<double>(i) / rate;
        st += t;
        sy += level;
        stt += t * t;
        sty += t * level;
        count += 1;
    }
    const double slope = (count * sty - st * sy) / (count * stt - st * st);
    return -60.0 / slope;
}
double t60(const Samples& h, double freq, double rate = kSampleRate) {
    return t60(std::vector<const Samples*>{&h}, freq, rate);
}

// Abel and Huang's normalized echo density over 20 ms around `seconds`: the
// share of samples beyond the window's standard deviation, over a Gaussian's
// (erfc(1 / sqrt 2)): about 1 once the echoes have blurred into noise.
double echoDensity(const Samples& h, double seconds, double rate = kSampleRate) {
    const auto width = static_cast<int64_t>(0.02 * rate);
    const Samples window = slice(h, frames(seconds, rate) - width / 2, frames(seconds, rate) + width / 2);
    const double sigma = rms(window);
    double beyond = 0.0;
    for (const float v : window) beyond += std::abs(v) > sigma ? 1.0 : 0.0;
    return beyond / static_cast<double>(window.size()) / std::erfc(1.0 / std::sqrt(2.0));
}

// The design's decay time at `freq` for these settings (the editor's curve).
double designDecay(const reverb::DecaySettings& settings, double freq, double rate = kSampleRate) {
    return reverb::decaySeconds(settings, freq, rate);
}

// The impulse response, `seconds` long, both sides (each a sum of all the lines:
// rows 1 and 2 of the network's Hadamard matrix).
struct Response {
    Samples left, right;
    double t60(double freq, double rate = kSampleRate) const {
        return ::t60(std::vector<const Samples*>{&left, &right}, freq, rate);
    }
};
Response impulseResponse(const Values& values, double seconds, double rate = kSampleRate) {
    Reverb r(values, rate);
    const size_t length = static_cast<size_t>(seconds * rate);
    auto [left, right] = r.play(impulse(length), impulse(length));
    return {std::move(left), std::move(right)};
}

}  // namespace

TEST_CASE("the reverb is listed with its parameters") {
    const sub::BuiltinInfo info = builtinInfo("reverb");
    CHECK_EQ(info.name, std::string("Reverb"));
    CHECK(!info.isInstrument());
    struct Expected {
        const char* id;
        const char* name;
        const char* unit;
        float min, max, def;
        bool log;
        std::vector<std::string> labels;
    };
    const std::vector<std::string> onOff = {"Off", "On"};
    const std::vector<Expected> expected = {
        {"predelay", "Predelay", "ms", 0.5f, 250.f, 2.5f, true, {}},
        {"lo_cut", "Lo Cut", "", 0.f, 1.f, 1.f, false, onOff},
        {"hi_cut", "Hi Cut", "", 0.f, 1.f, 1.f, false, onOff},
        {"in_freq", "In Filter Freq", "Hz", 50.f, 18000.f, 830.f, true, {}},
        {"in_width", "In Filter Width", "oct", 0.5f, 9.f, 7.5f, false, {}},
        {"spin", "ER Spin", "", 0.f, 1.f, 1.f, false, onOff},
        {"spin_rate", "ER Spin Rate", "Hz", 0.07f, 1.3f, 0.3f, true, {}},
        {"spin_amount", "ER Spin Amount", "%", 0.f, 100.f, 25.f, false, {}},
        {"shape", "ER Shape", "%", 0.f, 100.f, 50.f, false, {}},
        {"density", "Density", "", 0.f, 3.f, 3.f, false, {"Sparse", "Low", "Mid", "High"}},
        {"smooth", "Size Smoothing", "", 0.f, 2.f, 1.f, false, {"None", "Slow", "Fast"}},
        {"size", "Room Size", "size", 0.22f, 500.f, 100.f, true, {}},
        {"stereo", "Stereo Image", "°", 0.f, 120.f, 100.f, false, {}},
        {"lo_shelf", "Lo Shelf", "", 0.f, 1.f, 1.f, false, onOff},
        {"lo_freq", "Lo Shelf Freq", "Hz", 20.f, 15000.f, 90.f, true, {}},
        {"lo_gain", "Lo Shelf Gain", "%", 20.f, 100.f, 75.f, false, {}},
        {"hi_filter", "Hi Filter", "", 0.f, 1.f, 1.f, false, onOff},
        {"hi_type", "Hi Filter Type", "", 0.f, 1.f, 0.f, false, {"Shelf", "Low-pass"}},
        {"hi_freq", "Hi Filter Freq", "Hz", 20.f, 16000.f, 4500.f, true, {}},
        {"hi_gain", "Hi Shelf Gain", "%", 20.f, 100.f, 70.f, false, {}},
        {"decay", "Decay Time", "ms", 200.f, 60000.f, 1200.f, true, {}},
        {"freeze", "Freeze", "", 0.f, 1.f, 0.f, false, onOff},
        {"flat", "Flat", "", 0.f, 1.f, 1.f, false, onOff},
        {"cut", "Cut", "", 0.f, 1.f, 1.f, false, onOff},
        {"diffusion", "Diffusion", "%", 0.f, 100.f, 70.f, false, {}},
        {"scale", "Scale", "%", 0.f, 100.f, 50.f, false, {}},
        {"chorus", "Chorus", "", 0.f, 1.f, 1.f, false, onOff},
        {"chorus_rate", "Chorus Rate", "Hz", 0.01f, 8.f, 0.8f, true, {}},
        {"chorus_amount", "Chorus Amount", "%", 0.f, 100.f, 20.f, false, {}},
        {"reflect", "Reflect Level", "dB", -30.f, 6.f, 0.f, false, {}},
        {"diffuse", "Diffuse Level", "dB", -30.f, 6.f, 0.f, false, {}},
        {"mix", "Dry/Wet", "%", 0.f, 100.f, 40.f, false, {}},
    };
    REQUIRE(info.params.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        const sub::ParamInfo& p = info.params[i];
        const Expected& e = expected[i];
        INFO(e.id);
        CHECK_EQ(p.id, std::string(e.id));
        CHECK_EQ(p.name, std::string(e.name));
        CHECK_EQ(p.unit, std::string(e.unit));
        CHECK_EQ(p.minValue, e.min);
        CHECK_EQ(p.maxValue, e.max);
        CHECK_EQ(p.defaultValue, e.def);
        CHECK_EQ(p.isLog(), e.log);
        CHECK(p.valueLabels == e.labels);
        CHECK_EQ(p.steps, 0);
        CHECK(p.automatable);
    }

    Reverb r;
    std::vector<std::string> ids;
    std::vector<int> perValue;
    for (const sub::DisplayInfo& d : r.processor().displays()) {
        ids.push_back(d.id);
        perValue.push_back(d.samplesPerValue);
    }
    CHECK(ids == (std::vector<std::string>{"input", "early", "diffuse", "spin", "chorus", "signal", "tail"}));
    CHECK(perValue == (std::vector<int>{256, 256, 256, 256, 256, 1, 1}));
    CHECK_EQ(r.processor().latencySamples(), 0);

    // The engine makes it, and it reports no latency.
    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "reverb", -1);
    const sub::ProcessorInfo processor = engine.processorInfo(id);
    CHECK_EQ(processor.name, std::string("Reverb"));
    CHECK_EQ(processor.latency, 0);
    CHECK(processor.tail > 0);
}

TEST_CASE("the reverb gives exact silence for silence") {
    Reverb r;
    const auto [left, right] = r.play(Samples(static_cast<size_t>(5 * kSampleRate), 0.f),
                                      Samples(static_cast<size_t>(5 * kSampleRate), 0.f));
    CHECK(allEqual(left, 0.0));
    CHECK(allEqual(right, 0.0));
}

TEST_CASE("the reverb fully dry passes the input untouched") {
    const Samples left = noise(kSampleRate, 1), right = noise(kSampleRate, 2);
    Reverb r({{"mix", 0.f}});
    const auto [l, rr] = r.play(left, right);
    CHECK_ARRAY_EQUAL(l, left);
    CHECK_ARRAY_EQUAL(rr, right);
}

TEST_CASE("the reverb decays per band as its design says") {
    // High, every band at the Decay its shelves leave it: the mean line's
    // decay (the editor's curve) against Schroeder's on the impulse response.
    for (const float decay : {500.f, 1200.f, 4000.f}) {
        for (const bool lowpass : {false, true}) {
            reverb::DecaySettings design;
            design.decayMs = decay;
            Values values = with(kQuiet, {{"decay", decay}});
            if (lowpass) {
                design.hiLowpass = true;
                design.hiFreq = 3000.0;
                values = with(values, {{"hi_type", 1.f}, {"hi_freq", 3000.f}});
            }
            const Response h = impulseResponse(values, 2.5 * decay / 1000.0 + 0.5);
            for (const double band : {125.0, 1000.0, 8000.0}) {
                const double want = designDecay(design, band), got = h.t60(band);
                INFO(describe(values) + " at " + std::to_string(band) + " Hz: " + std::to_string(got) +
                     " s, designed " + std::to_string(want));
                if (want >= 0.4) {
                    CHECK_APPROX_TOL(got, want, lowpass && band == 8000.0 ? 0.25 : 0.15, 0.0);
                } else {
                    CHECK(got < 0.5);
                }
            }
        }
    }
    // Both shelves off: every band rings for Decay.
    const Response flat = impulseResponse(with(kQuiet, {{"lo_shelf", 0.f}, {"hi_filter", 0.f}}), 3.5);
    const double low = flat.t60(125.0), mid = flat.t60(1000.0), high = flat.t60(8000.0);
    INFO(std::to_string(low) + " " + std::to_string(mid) + " " + std::to_string(high));
    for (const double t : {low, mid, high}) {
        CHECK_APPROX_TOL(t, 1.2, 0.1, 0.0);
        CHECK_APPROX_TOL(t, mid, 0.1, 0.0);
    }
    // The other densities, in the middle.
    for (const float density : {0.f, 1.f, 2.f}) {
        reverb::DecaySettings design;
        design.density = static_cast<int>(density);
        const Response h = impulseResponse(with(kQuiet, {{"density", density}}), 3.5);
        const double got = h.t60(1000.0), want = designDecay(design, 1000.0);
        INFO("density " + std::to_string(density) + ": " + std::to_string(got) + " s, designed " +
             std::to_string(want));
        CHECK_APPROX_TOL(got, want, 0.2, 0.0);
    }
}

TEST_CASE("the reverb's shelves damp their bands") {
    const auto ratios = [](const Values& values) {
        const Response h = impulseResponse(with(kQuiet, values), 3.5);
        const double mid = h.t60(1000.0);
        return std::pair<double, double>{h.t60(125.0) / mid, h.t60(8000.0) / mid};
    };
    const auto [lowOfHigh, highOfHigh] = ratios({{"hi_gain", 40.f}, {"lo_shelf", 0.f}});
    INFO("hi 40 %: 8 kHz at " + std::to_string(highOfHigh) + " of 1 kHz's");
    CHECK(highOfHigh < 0.6);
    CHECK_APPROX_TOL(lowOfHigh, 1.0, 0.1, 0.0);
    const auto [lowOfLow, highOfLow] = ratios({{"lo_gain", 40.f}, {"lo_freq", 300.f}, {"hi_filter", 0.f}});
    INFO("lo 40 %: 125 Hz at " + std::to_string(lowOfLow) + " of 1 kHz's");
    CHECK(lowOfLow < 0.6);
    CHECK_APPROX_TOL(highOfLow, 1.0, 0.1, 0.0);
    const auto [lowOfNone, highOfNone] = ratios({{"lo_gain", 100.f}, {"hi_gain", 100.f}});
    CHECK_APPROX_TOL(lowOfNone, 1.0, 0.1, 0.0);
    CHECK_APPROX_TOL(highOfNone, 1.0, 0.1, 0.0);
}

TEST_CASE("the reverb's predelay and size place the reflections") {
    // The reflections alone: two renders that differ only in Reflect (0 and -30
    // dB), whose difference is (1 - 10^-1.5) times the reflections at 0 dB (the
    // network's part is the same in both). Each of earlyTaps()' taps lands
    // where it says, with its gain and sign (one channel: the taps' gains into
    // the two sides averaged), read between samples as the device reads them
    // (4-point Hermite: an impulse spreads over the sample before and the two after).
    struct Place {
        float predelay, size;
    };
    for (const Place& place : {Place{2.5f, 100.f}, Place{100.f, 100.f}, Place{2.5f, 400.f}, Place{250.f, 25.f}}) {
        INFO("predelay " + std::to_string(place.predelay) + " ms, size " + std::to_string(place.size));
        const Values values = with(kQuiet, {{"predelay", place.predelay}, {"size", place.size}, {"shape", 0.f},
                                            {"diffuse", -30.f}});
        const double s = reverb::sizeFactor(place.size);
        const size_t length = static_cast<size_t>((place.predelay + 70.0 * s + 5.0) / 1000.0 * kSampleRate);
        Reverb loud(with(values, {{"reflect", 0.f}})), quiet(with(values, {{"reflect", -30.f}}));
        const Samples a = loud.play(impulse(length)), b = quiet.play(impulse(length));
        const double share = 1.0 - std::pow(10.0, -1.5);
        Samples early(length);
        for (size_t i = 0; i < length; ++i) early[i] = static_cast<float>((a[i] - b[i]) / share);

        std::array<reverb::Tap, reverb::kMaxTaps> taps;
        reverb::earlyTaps(s, 0.0, reverb::Density::High, taps);
        std::vector<double> want(length, 0.0);
        const double predelay = place.predelay * kSampleRate / 1000.0;
        for (const reverb::Tap& tap : taps) {
            // (positions as the device works them out: in double, then a float)
            const auto position = static_cast<float>(predelay + tap.ms * kSampleRate / 1000.0);
            const auto whole = static_cast<size_t>(position);
            const float t = position - static_cast<float>(whole);
            const double gain =
                tap.gain * 0.5 * (std::sqrt(0.5 * (1.0 - tap.pan)) + std::sqrt(0.5 * (1.0 + tap.pan)));
            want[whole - 1] += gain * sub::dsp::hermite(1.f, 0.f, 0.f, 0.f, t);
            want[whole] += gain * sub::dsp::hermite(0.f, 1.f, 0.f, 0.f, t);
            want[whole + 1] += gain * sub::dsp::hermite(0.f, 0.f, 1.f, 0.f, t);
            want[whole + 2] += gain * sub::dsp::hermite(0.f, 0.f, 0.f, 1.f, t);
        }
        CHECK_ALLCLOSE(early, want, 0.0, 1e-5);
        // The first sound: the first tap, at the predelay itself (within the interpolation's reach).
        const std::vector<int64_t> heard = above(a, 1e-4);
        REQUIRE(!heard.empty());
        CHECK_EQ(reverb::kTapMs[0], 0.0);
        CHECK_NEAR(static_cast<double>(heard.front()), predelay, 2.0);
    }
}

TEST_CASE("the reverb's shape moves the diffuse onset and the reflections' envelope") {
    // The network's part alone: two renders that differ only in Diffuse (0 and
    // -30 dB): the reflections cancel exactly.
    const auto network = [](float shape, float size) {
        const Values values = with(kQuiet, {{"shape", shape}, {"size", size}, {"reflect", 0.f}});
        const size_t length = static_cast<size_t>(0.4 * kSampleRate);
        Reverb a(with(values, {{"diffuse", 0.f}})), b(with(values, {{"diffuse", -30.f}}));
        const Samples x = a.play(impulse(length)), y = b.play(impulse(length));
        Samples d(length);
        for (size_t i = 0; i < length; ++i) d[i] = x[i] - y[i];
        return d;
    };
    const auto onset = [](const Samples& d) {
        const std::vector<int64_t> heard = above(d, 1e-3 * maxAbs(d));
        return heard.empty() ? -1.0 : static_cast<double>(heard.front()) / kSampleRate * 1000.0;
    };
    for (const float size : {100.f, 300.f}) {
        const double s = reverb::sizeFactor(size);
        const double early = onset(network(0.f, size)), late = onset(network(100.f, size));
        INFO("size " + std::to_string(size) + ": " + std::to_string(early) + " ms and " + std::to_string(late) + " ms");
        CHECK_NEAR(late - early, reverb::kOnsetShare * reverb::kTapMs[reverb::kMaxTaps - 1] * s, 2.0);
    }
    // Shape high: the reflections fade faster (their last third against their first).
    const auto fall = [](float shape) {
        const Values values = with(kQuiet, {{"shape", shape}, {"reflect", 0.f}, {"diffuse", -30.f}});
        Reverb r(values);
        const Samples h = r.play(impulse(static_cast<size_t>(0.1 * kSampleRate)));
        const double predelay = 2.5, first = reverb::kTapMs[0], last = reverb::kTapMs[reverb::kMaxTaps - 1];
        const double third = (last - first) / 3.0;
        const auto at = [&](double ms) { return frames((predelay + ms) / 1000.0); };
        return powerDb(energy(slice(h, at(last - third), at(last + 0.5))) /
                       energy(slice(h, at(first - 0.5), at(first + third))));
    };
    const double gentle = fall(0.f), steep = fall(100.f);
    INFO(std::to_string(gentle) + " dB and " + std::to_string(steep) + " dB");
    CHECK(steep < gentle - 10.0);
}

TEST_CASE("the reverb's input filter is the band its design draws") {
    const Values values = with(kQuiet, {{"reflect", 0.f}, {"diffuse", -30.f}, {"in_freq", 1000.f}, {"in_width", 2.f}});
    for (const double freq : {100.0, 500.0, 1000.0, 2000.0, 10000.0}) {
        const Samples tone = smoothSine(freq, 2.0);
        Reverb on(with(values, {{"lo_cut", 1.f}, {"hi_cut", 1.f}})), off(values);
        const Samples a = slice(on.play(tone), kSampleRate), b = slice(off.play(tone), kSampleRate);
        const double got = rmsDb(a) - rmsDb(b);
        const double want = reverb::inputFilterDb(1000.0, 2.0, true, true, freq, kSampleRate);
        INFO(std::to_string(freq) + " Hz: " + std::to_string(got) + " dB, designed " + std::to_string(want));
        CHECK_NEAR(got, want, 1.0);
        if (freq == 100.0 || freq == 10000.0) CHECK(got < -20.0);
    }
}

TEST_CASE("the reverb hears its input in mono") {
    const Samples x = noise(kSampleRate / 2, 3);
    const Samples zero(x.size(), 0.f);
    Reverb a(kQuiet), b(kQuiet);
    const auto [al, ar] = a.play(silence(x, 1.0), silence(zero, 1.0));
    const auto [bl, br] = b.play(silence(zero, 1.0), silence(x, 1.0));
    CHECK_ARRAY_EQUAL(al, bl);
    CHECK_ARRAY_EQUAL(ar, br);
}

TEST_CASE("the reverb's stereo image goes from mono to two independent sides") {
    // As Live's: the lowest setting is mono; the highest (120 degrees) gives each
    // side a reverb independent of the other's; the default (100) a little narrower.
    const auto render = [](float stereo) {
        Reverb r(with(kQuiet, {{"stereo", stereo}}));
        const size_t length = static_cast<size_t>(1.5 * kSampleRate);
        return r.play(impulse(length), impulse(length));
    };
    const auto [monoL, monoR] = render(0.f);
    CHECK_ARRAY_EQUAL(monoL, monoR);
    const auto late = [](const Samples& x) { return slice(x, frames(0.2), frames(1.2)); };
    const auto rho = [&](const Samples& a, const Samples& b) {
        const Samples la = late(a), lb = late(b);
        return correlation(std::vector<double>(la.begin(), la.end()), std::vector<double>(lb.begin(), lb.end()));
    };
    const auto [wl, wr] = render(120.f);
    const auto [l, r] = render(100.f);
    INFO("correlation at 120: " + std::to_string(rho(wl, wr)) + ", at 100: " + std::to_string(rho(l, r)));
    CHECK(std::abs(rho(wl, wr)) < 0.1);
    CHECK(rho(l, r) > 0.05);
    CHECK(rho(l, r) < 0.35);
    const auto sideOverMid = [&](const Samples& a, const Samples& b) {
        double side = 0.0, mid = 0.0;
        const Samples la = late(a), lb = late(b);
        for (size_t i = 0; i < la.size(); ++i) {
            mid += 0.25 * (la[i] + lb[i]) * (la[i] + lb[i]);
            side += 0.25 * (la[i] - lb[i]) * (la[i] - lb[i]);
        }
        return side / mid;
    };
    CHECK_APPROX_TOL(sideOverMid(wl, wr) / sideOverMid(l, r), 1.44, 0.05, 0.0);
    CHECK_EQ(reverb::stereoWidth(120.0), 1.0);
    CHECK_EQ(reverb::stereoWidth(0.0), 0.0);
}

TEST_CASE("the reverb on one channel plays the wet's middle") {
    const Samples x = silence(noise(kSampleRate / 2, 4), 1.0);
    const Values values = {{"mix", 60.f}};
    Reverb stereo(values), mono(values);
    const auto [l, r] = stereo.play(x, x);
    const Samples one = mono.play(x);
    Samples want(x.size());
    for (size_t i = 0; i < x.size(); ++i) want[i] = 0.5f * (l[i] + r[i]);
    CHECK_ALLCLOSE(one, want, 0.0, 1e-6);
}

TEST_CASE("the reverb's levels") {
    // Reflect scales the reflections (Shape 100: the first 20 ms after the
    // predelay hold them alone), Diffuse the tail.
    const auto window = [](float reflect, float diffuse, double from, double to) {
        Reverb r(with(kQuiet, {{"shape", 100.f}, {"reflect", reflect}, {"diffuse", diffuse}}));
        const Samples h = r.play(impulse(static_cast<size_t>(1.6 * kSampleRate)));
        return energy(slice(h, frames(from), frames(to)));
    };
    CHECK_NEAR(powerDb(window(-30.f, 0.f, 0.0025, 0.0225) / window(0.f, 0.f, 0.0025, 0.0225)), -30.0, 0.5);
    CHECK_NEAR(powerDb(window(-30.f, -30.f, 0.5, 1.5) / window(-30.f, 0.f, 0.5, 1.5)), -30.0, 0.5);

    // White noise at -20 dBFS RMS, all wet: the reverb about as loud (a few dB under).
    const float amplitude = static_cast<float>(0.1 * std::sqrt(3.0));  // uniform: RMS = amplitude / sqrt(3)
    const Samples x = noise(static_cast<size_t>(4 * kSampleRate), 5, amplitude);
    Reverb r({{"mix", 100.f}, {"lo_cut", 0.f}, {"hi_cut", 0.f}, {"lo_shelf", 0.f}, {"hi_filter", 0.f}});
    const auto [l, rr] = r.play(x, x);
    const double level = 0.5 * (rmsDb(slice(l, 2 * kSampleRate)) + rmsDb(slice(rr, 2 * kSampleRate)));
    INFO("noise at -20 dBFS gives " + std::to_string(level) + " dBFS");
    CHECK(level > -27.0);
    CHECK(level < -19.0);
}

TEST_CASE("the reverb's freeze holds the tail") {
    // Noise, then frozen (Cut and Flat, by automation), the noise stopping just
    // after: the tail holds, where the same unfrozen dies away.
    const Samples x = noise(static_cast<size_t>(7 * kSampleRate), 6);
    const int64_t at = frames(0.5);
    {
        Samples in = x;
        std::fill(in.begin() + at + frames(0.05), in.end(), 0.f);
        Reverb r(kQuiet), unfrozen(kQuiet);
        const Samples out = r.play(in, {{at, "freeze", 1.f}}), gone = unfrozen.play(in);
        const double early = rmsDb(slice(out, at + frames(1.0), at + frames(2.0)));
        const double late = rmsDb(slice(out, at + frames(5.0), at + frames(6.0)));
        INFO(std::to_string(early) + " dB, then " + std::to_string(late) + " dB");
        CHECK_NEAR(late, early, 1.5);
        CHECK(early > -40.0);
        INFO("unfrozen: " + std::to_string(rmsDb(slice(gone, at + frames(5.0), at + frames(6.0)))) + " dB");
        CHECK(rmsDb(slice(gone, at + frames(5.0), at + frames(6.0))) < early - 40.0);
    }
    // Cut: frozen, new sound no longer reaches the tail. Two devices fed alike up
    // to a second after freezing; then one gets silence and the other a tone
    // for a second: once the tone's reflections are gone they sound the same.
    // Without Cut the tone is added to the frozen tail, and stays.
    for (const float cut : {1.f, 0.f}) {
        INFO("cut " + std::to_string(cut));
        const Values values = with(kQuiet, {{"cut", cut}});
        const int64_t until = at + frames(1.0);
        Samples a = slice(x, 0, until), b = a;
        a = silence(a, 3.0);
        const Samples tone = sine(440.0, 1.0, 0.5);
        b.resize(a.size(), 0.f);
        std::copy(tone.begin(), tone.end(), b.begin() + until);
        Reverb ra(values), rb(values);
        const Samples oa = ra.play(a, {{at, "freeze", 1.f}}), ob = rb.play(b, {{at, "freeze", 1.f}});
        const int64_t after = until + frames(1.3);
        const Samples ta = slice(oa, after), tb = slice(ob, after);
        double worst = 0.0;
        for (size_t i = 0; i < ta.size(); ++i) worst = std::max(worst, std::abs(static_cast<double>(ta[i]) - tb[i]));
        const double peak = maxAbs(ta);
        INFO("differ by " + std::to_string(worst) + " of a tail peaking at " + std::to_string(peak));
        if (cut > 0.f) {
            CHECK(worst < 1e-4 * peak);
        } else {
            CHECK(worst > 0.1 * peak);
        }
    }
    // Released: the noise stops, then Freeze goes off, and the tail decays at Decay's pace.
    {
        Reverb r(kQuiet);
        const Samples in = silence(slice(x, 0, frames(1.0)), 5.0);
        const Samples out = r.play(in, {{frames(0.5), "freeze", 1.f}, {frames(2.0), "freeze", 0.f}});
        const double t = t60(slice(out, frames(2.1)), 0.0);
        INFO("released, it decays in " + std::to_string(t) + " s");
        CHECK_APPROX_TOL(t, 1.2, 0.2, 0.0);
    }
}

TEST_CASE("the reverb's flat keeps every band frozen, or not") {
    // Frozen with the high shelf at 20 %: with Flat the highs hold; without, the
    // shelf goes on taking them away while the middle holds.
    const auto highs = [](float flat) {
        Reverb r(with(kQuiet, {{"hi_gain", 20.f}, {"flat", flat}}));
        const Samples in = silence(noise(kSampleRate, 7), 4.5);
        const Samples out = r.play(in, {{frames(0.8), "freeze", 1.f}});
        // The energy above 6 kHz: a steep high-pass (three 2nd-order sections' worth of differences).
        std::vector<double> d(out.begin(), out.end());
        for (int k = 0; k < 6; ++k)
            for (size_t i = d.size() - 1; i > 0; --i) d[i] -= d[i - 1];
        const auto band = [&](double from, double to) {
            double sum = 0.0;
            for (int64_t i = frames(from); i < frames(to); ++i)
                sum += d[static_cast<size_t>(i)] * d[static_cast<size_t>(i)];
            return sum;
        };
        return powerDb(band(4.0, 4.5) / band(1.0, 1.5));
    };
    const double held = highs(1.f), lost = highs(0.f);
    INFO("highs over 3 s: " + std::to_string(held) + " dB with Flat, " + std::to_string(lost) + " dB without");
    CHECK(std::abs(held) < 3.0);
    CHECK(lost < -20.0);
}

TEST_CASE("the reverb's guard keeps it bounded") {
    // Full-scale noise into a frozen tail that still takes its input, or into a
    // minute's decay: they settle at a level (about +0.5 dBFS RMS), finite and
    // below the guard (+18 dBFS), which leaves them alone.
    const Samples loud = noise(static_cast<size_t>(20 * kSampleRate), 8, 1.f);
    for (const Values& values : {Values{{"freeze", 1.f}, {"cut", 0.f}, {"mix", 100.f}},
                                 Values{{"decay", 60000.f}, {"mix", 100.f}}}) {
        INFO(describe(values));
        Reverb r(values);
        const auto [l, rr] = r.play(loud, loud);
        CHECK(allFinite(l) && allFinite(rr));
        INFO("peak " + std::to_string(std::max(maxAbs(l), maxAbs(rr))));
        CHECK(std::max(maxAbs(l), maxAbs(rr)) < 8.0);
    }
    // +12 dBFS of noise (a hot bus) into a frozen tail that takes it: left alone
    // it would settle about +24 dBFS; the guard eases the loops down and holds the
    // tail's peaks near +18 dBFS, and the level stops growing.
    const Samples hot = noise(static_cast<size_t>(20 * kSampleRate), 9, 4.f);
    Reverb r({{"freeze", 1.f}, {"cut", 0.f}, {"mix", 100.f}, {"reflect", -30.f}});
    const auto [l, rr] = r.play(hot, hot);
    CHECK(allFinite(l) && allFinite(rr));
    const double peak = std::max(maxAbs(slice(l, 10 * kSampleRate)), maxAbs(slice(rr, 10 * kSampleRate)));
    const double held = rmsDb(slice(l, 10 * kSampleRate, 12 * kSampleRate)), later = rmsDb(slice(l, 18 * kSampleRate));
    INFO("peak " + std::to_string(peak) + ", RMS at 10..12 s " + std::to_string(held) + " dBFS and at 18..20 s " +
         std::to_string(later));
    CHECK(peak < 12.0);
    CHECK(later < held + 1.0);
}

TEST_CASE("the reverb's design helpers for its editor") {
    // Spin's pan law: at rest each reflection's own pan, swung within the field.
    for (int k = 0; k < reverb::kMaxTaps; ++k) {
        INFO("tap " + std::to_string(k));
        CHECK_APPROX_TOL(reverb::spinPan(k, 0.0, 0.3), reverb::kTapPan[static_cast<size_t>(k)], 0.0, 1e-12);
        double lowest = 1.0, highest = -1.0;
        for (double phase = 0.0; phase < 1.0; phase += 1.0 / 64.0) {
            const double pan = reverb::spinPan(k, 1.0, phase);
            lowest = std::min(lowest, pan);
            highest = std::max(highest, pan);
        }
        CHECK(lowest >= -1.0);
        CHECK(highest <= 1.0);
        CHECK(highest - lowest > 0.15);  // every reflection moves (those at an edge least)
    }
    // Near the middle about +-0.7 (tap 5 sits at 0.17).
    double lowest = 1.0, highest = -1.0;
    for (double phase = 0.0; phase < 1.0; phase += 1.0 / 256.0) {
        lowest = std::min(lowest, reverb::spinPan(5, 1.0, phase));
        highest = std::max(highest, reverb::spinPan(5, 1.0, phase));
    }
    CHECK(highest - lowest > 1.3);
    // The early taps share kEarlyGain's energy whatever the Density and Shape.
    for (const reverb::Density d : {reverb::Density::Sparse, reverb::Density::High}) {
        for (const double shape : {0.0, 50.0, 100.0}) {
            std::array<reverb::Tap, reverb::kMaxTaps> taps;
            const int used = reverb::earlyTaps(1.0, shape, d, taps);
            double energy = 0.0;
            int heard = 0;
            for (const reverb::Tap& tap : taps) {
                energy += tap.gain * tap.gain;
                heard += tap.gain != 0.0 ? 1 : 0;
            }
            CHECK_APPROX(energy, reverb::kEarlyGain * reverb::kEarlyGain);
            CHECK_EQ(heard, used);
            CHECK_EQ(used, d == reverb::Density::Sparse ? 6 : 12);
        }
    }
    // The decay curve: frozen with Cut and Flat about 1000 s everywhere; frozen
    // without Flat, the shelves still take their bands away.
    reverb::DecaySettings frozen;
    frozen.freeze = true;
    for (const double f : {30.0, 1000.0, 15000.0}) CHECK(reverb::decaySeconds(frozen, f, kSampleRate) > 900.0);
    frozen.flat = false;
    CHECK(reverb::decaySeconds(frozen, 10000.0, kSampleRate) < 10.0);
    CHECK(reverb::decaySeconds(frozen, 1000.0, kSampleRate) > 20.0);
    // The input filter: flat in the middle of a wide band, Butterworth at its edges.
    CHECK_NEAR(reverb::inputFilterDb(830.0, 7.5, true, true, 830.0, kSampleRate), 0.0, 0.01);
    CHECK_NEAR(reverb::inputFilterDb(1000.0, 2.0, true, false, 500.0, kSampleRate), -3.01, 0.01);
    CHECK_NEAR(reverb::inputFilterDb(1000.0, 2.0, false, true, 2000.0, kSampleRate), -3.01, 0.01);
    CHECK_EQ(reverb::inputFilterDb(1000.0, 2.0, false, false, 20.0, kSampleRate), 0.0);
}

TEST_CASE("the reverb's densities") {
    // Each decays at Decay's pace.
    for (const float density : {0.f, 1.f, 2.f, 3.f}) {
        const Response h = impulseResponse(with(kQuiet, {{"density", density}}), 3.5);
        CHECK(allFinite(h.left) && allFinite(h.right));
        const double t = h.t60(1000.0);
        INFO("density " + std::to_string(density) + ": " + std::to_string(t) + " s");
        CHECK_APPROX_TOL(t, 1.2, 0.2, 0.0);
    }
    // The echoes blur into noise: the echo density at the defaults is a
    // Gaussian's from about 150 ms on, and early on the richer networks are
    // denser. (On the network's own sum of its lines: Stereo at 120, the side as
    // it is. The measure counts sharp echoes as sparser than smeared ones, so
    // where the input falls between samples moves it about 0.1 around 150 ms:
    // each 20 ms window from 160 ms on, and their mean from 150 ms on.)
    const Samples high = impulseResponse(with(kQuiet, {{"stereo", 120.f}}), 1.0).left;
    for (const double at : {0.16, 0.2, 0.3, 0.4, 0.6}) {
        INFO("at " + std::to_string(at) + " s: " + std::to_string(echoDensity(high, at)));
        CHECK(echoDensity(high, at) >= 0.85);
    }
    double blur = 0.0;
    for (int k = 0; k <= 15; ++k) blur += echoDensity(high, 0.15 + 0.01 * k) / 16.0;
    INFO("from 150 to 300 ms: " + std::to_string(blur) + " on average");
    CHECK(blur >= 0.9);
    const Samples sparse = impulseResponse(with(kQuiet, {{"density", 0.f}, {"stereo", 120.f}}), 1.0).left;
    INFO("at 60 ms: High " + std::to_string(echoDensity(high, 0.06)) + ", Sparse " +
         std::to_string(echoDensity(sparse, 0.06)));
    CHECK(echoDensity(high, 0.06) > echoDensity(sparse, 0.06));
}

TEST_CASE("the reverb's density changes keep a frozen tail") {
    // Frozen (Cut and Flat), a change of Density crossfades the two networks: the lines they share keep
    // what they hold, never faded through silence. High and Mid share every line (High adds an all-pass in
    // each loop): switched back and forth, the frozen tail holds as one never switched does, and no 5 ms
    // of it dips. Low's lines are all High's too: from Low to High nothing is lost.
    const Values values = with(kQuiet, {{"decay", 4000.f}});
    const Samples in = silence(noise(kSampleRate, 20, 0.3f), 6.5);
    const auto level = [](const Samples& x, double from, double to) {
        return rmsDb(slice(x, frames(from), frames(to)));
    };
    const auto lowest = [&](const Samples& x, double from, double to) {
        double low = 1e9;
        for (double t = from; t < to; t += 0.005) low = std::min(low, level(x, t, t + 0.005));
        return low;
    };
    {
        std::vector<Change> changes = {{frames(1.0), "freeze", 1.f}};
        for (int k = 0; k < 4; ++k) changes.push_back({frames(2.5 + k), "density", k % 2 == 0 ? 2.f : 3.f});
        Reverb switched(values), held(values);
        const Samples a = switched.play(in, changes), b = held.play(in, {{frames(1.0), "freeze", 1.f}});
        for (const double t : {3.0, 4.0, 5.0, 6.0}) {
            INFO("at " + std::to_string(t) + " s: " + std::to_string(level(a, t, t + 0.4)) + " dB, never switched " +
                 std::to_string(level(b, t, t + 0.4)));
            CHECK_NEAR(level(a, t, t + 0.4), level(b, t, t + 0.4), 1.0);
        }
        INFO("lowest 5 ms: " + std::to_string(lowest(a, 2.3, 6.9)) + " dB, never switched " +
             std::to_string(lowest(b, 2.3, 6.9)));
        CHECK(lowest(a, 2.3, 6.9) > lowest(b, 2.3, 6.9) - 3.0);
    }
    {
        Reverb r(with(values, {{"density", 1.f}}));
        const Samples a = r.play(in, {{frames(1.0), "freeze", 1.f}, {frames(2.5), "density", 3.f}});
        INFO("Low " + std::to_string(level(a, 2.0, 2.4)) + " dB, then High " + std::to_string(level(a, 3.0, 3.4)));
        CHECK_NEAR(level(a, 3.0, 3.4), level(a, 2.0, 2.4), 1.0);
        CHECK(lowest(a, 2.3, 3.5) > level(a, 2.0, 2.4) - 6.0);
    }
}

TEST_CASE("the reverb's density changes start what joins from silence") {
    // Loud noise into High, then Sparse: the lines, loop all-passes and diffusers Sparse doesn't run keep
    // what they held. A faint input keeps it awake until, a second later, High comes back: what joins must
    // start from silence (it is let go, and cleared just ahead of its reads), not play the old tail back.
    const Values values = with(kQuiet, {{"decay", 200.f}});
    Samples in = noise(2 * kSampleRate, 24, 1e-5f);
    const Samples loud = noise(kSampleRate / 2, 25, 0.3f);
    std::copy(loud.begin(), loud.end(), in.begin());
    Reverb r(values);
    const Samples out = r.play(in, {{frames(0.5), "density", 0.f}, {frames(1.5), "density", 3.f}});
    INFO("while loud " + std::to_string(maxAbs(slice(out, frames(0.2), frames(0.5)))) + ", before the change " +
         std::to_string(maxAbs(slice(out, frames(1.3), frames(1.5)))) + ", after " +
         std::to_string(maxAbs(slice(out, frames(1.5)))));
    CHECK(maxAbs(slice(out, frames(0.2), frames(0.5))) > 0.05);
    CHECK(maxAbs(slice(out, frames(1.5))) < 1e-4);
}

TEST_CASE("the reverb's spin swings and drifts each reflection as its design says") {
    // The reflections alone (two renders that differ only in Reflect, as above), Spin at its deepest and
    // fastest, Stereo at 120 (the sides as they are), impulses 80 ms apart: each tap's gain into each side
    // is the equal-power pan reverb::spinPan() gives at the LFO's phase then, and it comes
    // reverb::spinDriftMs() after its place at rest. (A Hermite read's four weights sum to one and their
    // centroid is the position read: each tap's sum and centroid read its gains and its time out; a tap
    // drifting at v samples a sample spreads an impulse over 1 / (1 - v) as many: Doppler.)
    const double rate = 1.3;
    const Values values = with(kQuiet, {{"spin", 1.f}, {"spin_amount", 100.f}, {"spin_rate", static_cast<float>(rate)},
                                        {"shape", 0.f}, {"stereo", 120.f}, {"diffuse", -30.f}});
    const auto length = static_cast<size_t>(kSampleRate);
    Samples in(length, 0.f);
    const int64_t spacing = frames(0.08);
    std::vector<int64_t> hits;
    for (int64_t at = frames(0.01); at + spacing < static_cast<int64_t>(length); at += spacing) {
        in[static_cast<size_t>(at)] = 1.f;
        hits.push_back(at);
    }
    Reverb loud(with(values, {{"reflect", 0.f}})), quiet(with(values, {{"reflect", -30.f}}));
    const auto [al, ar] = loud.play(in, in);
    const auto [bl, br] = quiet.play(in, in);
    const double share = 1.0 - std::pow(10.0, -1.5);
    std::array<reverb::Tap, reverb::kMaxTaps> taps;
    reverb::earlyTaps(1.0, 0.0, reverb::Density::High, taps);
    const double depth = 2.0 * reverb::kSpinDepthMs * kSampleRate / 1000.0;  // (the most it drifts, in samples)
    double worstGain = 0.0, worstPan = 0.0, worstDrift = 0.0, widest = 0.0, latest = 0.0;
    for (const int64_t hit : hits) {
        for (int k = 0; k < reverb::kMaxTaps; ++k) {
            const reverb::Tap& tap = taps[static_cast<size_t>(k)];
            const double rest = (2.5 + tap.ms) * kSampleRate / 1000.0;
            double left = 0.0, right = 0.0, moment = 0.0;
            const auto from = static_cast<int64_t>(hit + rest) - 3, to = static_cast<int64_t>(hit + rest + depth) + 4;
            for (int64_t n = from; n <= to; ++n) {
                const size_t i = static_cast<size_t>(n);
                const double l = (al[i] - bl[i]) / share, r = (ar[i] - br[i]) / share;
                left += l;
                right += r;
                moment += static_cast<double>(n) * (l + r);
            }
            const double at = moment / (left + right);
            const double phase = rate * (at + 1.0) / kSampleRate;  // (the LFO's phase where the tap is read)
            const double angle = reverb::tapAngle(k) + reverb::spinAngle(k, 1.0, phase);
            const double speed = reverb::kSpinDepthMs * kSampleRate / 1000.0 * 2.0 * kPi * rate / kSampleRate *
                                 std::cos(2.0 * kPi * (phase + k / 12.0));
            const double doppler = 1.0 / (1.0 - speed);
            worstGain = std::max({worstGain, std::abs(left - tap.gain * std::cos(angle) * doppler),
                                  std::abs(right - tap.gain * std::sin(angle) * doppler)});
            const double pan = (right * right - left * left) / (right * right + left * left);
            worstPan = std::max(worstPan, std::abs(pan - reverb::spinPan(k, 1.0, phase)));
            const double drift = at - static_cast<double>(hit) - rest;
            worstDrift = std::max(worstDrift,
                                  std::abs(drift - reverb::spinDriftMs(k, 1.0, phase, 1.0) * kSampleRate / 1000.0));
            widest = std::max(widest, std::abs(pan - tap.pan));
            latest = std::max(latest, drift);
        }
    }
    INFO("gains within " + std::to_string(worstGain) + ", pans within " + std::to_string(worstPan) +
         ", drifts within " + std::to_string(worstDrift) + " samples; swung up to " + std::to_string(widest) +
         ", drifted up to " + std::to_string(latest) + " samples");
    CHECK(worstGain < 2e-4);
    CHECK(worstPan < 1e-4);
    CHECK(worstDrift < 0.01);
    CHECK(widest > 0.5);    // (it does swing them)
    CHECK(latest > 80.0);   // (and drift them, later only: up to 2 ms)

    // The network hears the input where the first reflection is, drifting with it: Spin reaches the tail.
    // Off, or on at no depth, the tail is the same; deep, it is not.
    const auto network = [](const Values& spin) {
        const Values v = with(with(kQuiet, {{"mix", 100.f}, {"reflect", 0.f}}), spin);
        const Samples x = silence(noise(kSampleRate / 2, 24), 0.5);
        Reverb a(with(v, {{"diffuse", 0.f}})), b(with(v, {{"diffuse", -30.f}}));
        const Samples ya = a.play(x), yb = b.play(x);
        Samples d(ya.size());
        for (size_t i = 0; i < d.size(); ++i) d[i] = ya[i] - yb[i];
        return d;
    };
    const Samples off = network({{"spin", 0.f}}), still = network({{"spin", 1.f}, {"spin_amount", 0.f}});
    const Samples deep = network({{"spin", 1.f}, {"spin_amount", 100.f}, {"spin_rate", 1.3f}});
    CHECK_ARRAY_EQUAL(still, off);
    double moved = 0.0;
    for (size_t i = 0; i < off.size(); ++i) moved = std::max(moved, std::abs(static_cast<double>(deep[i]) - off[i]));
    INFO("the tail moved by " + std::to_string(moved) + " of a peak of " + std::to_string(maxAbs(off)));
    CHECK(moved > 0.05 * maxAbs(off));
}

TEST_CASE("the reverb's chorus, diffusion and scale do what they say") {
    // Chorus: the lines' delays drift, so a steady tone's tail spreads in pitch. Off, the network is
    // time-invariant and all of it stays on the tone; at the default 20 % a little, at 100 % most of it
    // leaves the bins around the tone (1 Hz each, between 900 Hz and 1.1 kHz).
    const auto onTone = [](float amount) {
        Reverb r(with(kQuiet, {{"chorus", amount > 0.f ? 1.f : 0.f}, {"chorus_amount", amount}}));
        const Samples tail = slice(r.play(sine(1000.0, 3.0, 0.5)), frames(1.5), frames(2.5));
        const std::vector<double> m = spectrum(tail, hanning(tail.size()));
        double near = 0.0, around = 0.0;
        for (size_t k = 900; k <= 1100; ++k) {
            around += m[k] * m[k];
            if (k >= 998 && k <= 1002) near += m[k] * m[k];
        }
        return near / around;
    };
    const double still = onTone(0.f), light = onTone(20.f), deep = onTone(100.f);
    INFO("on the tone: " + std::to_string(still) + " off, " + std::to_string(light) + " at 20 %, " +
         std::to_string(deep) + " at 100 %");
    CHECK(still > 0.9999);
    CHECK(light < 0.99);
    CHECK(deep < 0.5);

    // Diffusion: the all-passes' gains, so the echoes blur sooner (the echo density at 80 ms, on the
    // network's own sum).
    const auto blurred = [](float diffusion) {
        const Values v = with(kQuiet, {{"diffusion", diffusion}, {"stereo", 120.f}});
        return echoDensity(impulseResponse(v, 0.3).left, 0.08);
    };
    const double none = blurred(0.f), full = blurred(100.f);
    INFO("echo density at 80 ms: " + std::to_string(none) + " at 0 %, " + std::to_string(full) + " at 100 %");
    CHECK(none < 0.2);
    CHECK(full > 0.5);

    // Scale: the input diffusers' lengths. The network alone (the difference of Diffuse at 0 and -30 dB),
    // Sparse (its first line, 34 ms, has the time to itself), Diffusion 100 %, Shape 0: after that line's
    // first echo (what passes the diffusers at once), the next comes the shorter diffuser's length later.
    for (const float scale : {0.f, 50.f, 100.f}) {
        const Values v = with(kQuiet, {{"density", 0.f}, {"diffusion", 100.f}, {"shape", 0.f}, {"scale", scale},
                                       {"stereo", 120.f}});
        Reverb a(with(v, {{"diffuse", 0.f}})), b(with(v, {{"diffuse", -30.f}}));
        const Samples ya = a.play(impulse(frames(0.2))), yb = b.play(impulse(frames(0.2)));
        Samples h(ya.size());
        for (size_t i = 0; i < h.size(); ++i) h[i] = ya[i] - yb[i];
        const std::vector<int64_t> heard = above(h, 1e-3 * maxAbs(h));
        REQUIRE(!heard.empty());
        auto first = static_cast<size_t>(heard.front());
        for (size_t i = first; i < first + 4; ++i)
            if (std::abs(h[i]) > std::abs(h[first])) first = i;
        const double want = reverb::kDiffuserMs[1] * reverb::scaleFactor(scale) * kSampleRate / 1000.0;
        size_t next = first + 6;
        while (next < first + static_cast<size_t>(1.25 * want) + 6 && std::abs(h[next]) < 0.4 * std::abs(h[first]))
            ++next;
        while (std::abs(h[next + 1]) > std::abs(h[next])) ++next;  // (to its peak)
        INFO("scale " + std::to_string(scale) + ": the next echo " + std::to_string(next - first) +
             " samples after the first, the diffuser " + std::to_string(want));
        CHECK_NEAR(static_cast<double>(next - first), want, 1.5);
    }
}

TEST_CASE("the reverb's tail has no metallic ringing") {
    // The late tail's power spectrum: no bin far above its third-octave's average.
    for (const float chorus : {0.f, 1.f}) {
        for (const float size : {30.f, 100.f, 300.f}) {
            const Values values = with(kQuiet, {{"chorus", chorus}, {"size", size}});
            const Samples h = impulseResponse(values, 0.7).left;
            const Samples tail = slice(h, frames(0.15), frames(0.65));
            const std::vector<double> magnitude = spectrum(tail, hanning(tail.size()));
            std::vector<double> power(magnitude.size());
            for (size_t k = 0; k < power.size(); ++k) power[k] = magnitude[k] * magnitude[k];
            const double binHz = kSampleRate / static_cast<double>(tail.size());
            double worst = -1e9;
            for (size_t k = static_cast<size_t>(200.0 / binHz); k <= static_cast<size_t>(5000.0 / binHz); ++k) {
                const double f = k * binHz;
                const auto from = static_cast<size_t>(f * std::pow(2.0, -1.0 / 6.0) / binHz);
                const auto to = static_cast<size_t>(f * std::pow(2.0, 1.0 / 6.0) / binHz);
                double sum = 0.0;
                for (size_t j = from; j <= to; ++j) sum += power[j];
                worst = std::max(worst, powerDb(power[k] / (sum / static_cast<double>(to - from + 1))));
            }
            INFO(describe(values) + ": a bin " + std::to_string(worst) + " dB over its third-octave");
            CHECK(worst < 14.0);
        }
    }
}

TEST_CASE("the reverb changes every control without a click") {
    // A 220 Hz tone through the defaults (Spin and Chorus on), every control and
    // switch jumping in turn as automation's steps make them: the largest 6th
    // difference stays within three times the steady render's, or 1e-4 (a step of
    // 1e-5, -100 dB) where that is more. Size at Smooth's Fast is the one control
    // allowed more: a jump from 100 to 300 sweeps the whole tail's pitch by 15 %
    // within 0.1 s (every delay growing at up to 0.15 samples a sample), and the
    // measure then sees what is left of the lines' reads' error in the sweep, -85
    // dB under the tone (a step of 1e-4, -80 dB, would score as much), not a click.
    // So is Smooth switched while Size glides: the sweep speeds up or slows down.
    struct Group {
        std::vector<Change> changes;
        double bar = 0.0;  // 0: the default bar
    };
    const Samples tone = smoothSine(220.0, 3.0);
    const auto s = [](double seconds) { return frames(seconds); };
    const std::vector<Group> groups = {
        {{{s(0.4), "predelay", 120.f}, {s(0.8), "predelay", 10.f}, {s(1.2), "scale", 90.f}, {s(1.6), "scale", 10.f}}},
        {{{s(0.4), "size", 300.f}, {s(1.4), "size", 60.f}}},  // (Smooth Slow)
        {{{s(0.4), "smooth", 2.f}, {s(0.4), "size", 300.f}, {s(1.4), "size", 100.f}}, 1e-3},
        {{{s(0.4), "size", 300.f}, {s(0.5), "smooth", 0.f}, {s(1.0), "smooth", 1.f}, {s(1.2), "size", 120.f},
          {s(1.3), "smooth", 2.f}, {s(1.4), "smooth", 0.f}, {s(1.45), "smooth", 1.f}},
         1e-3},
        {{{s(0.4), "decay", 6000.f}, {s(0.8), "decay", 400.f}, {s(1.2), "in_freq", 3000.f}, {s(1.4), "in_width", 2.f},
          {s(1.6), "lo_freq", 400.f}, {s(1.7), "lo_gain", 30.f}, {s(1.8), "hi_freq", 1500.f}, {s(1.9), "hi_gain", 30.f},
          {s(2.0), "diffusion", 100.f}, {s(2.1), "diffusion", 20.f}, {s(2.4), "shape", 0.f}, {s(2.5), "shape", 100.f}}},
        {{{s(0.4), "spin_amount", 100.f}, {s(0.5), "spin_rate", 1.3f}, {s(0.6), "chorus_amount", 100.f},
          {s(0.7), "chorus_rate", 8.f}, {s(1.5), "chorus_rate", 0.1f}, {s(2.0), "chorus_amount", 5.f},
          {s(2.2), "spin_rate", 0.07f}, {s(2.4), "spin_amount", 0.f}}},
        {{{s(0.4), "stereo", 0.f}, {s(0.6), "stereo", 120.f}, {s(0.8), "reflect", -30.f}, {s(1.0), "reflect", 6.f},
          {s(1.2), "diffuse", -30.f}, {s(1.4), "diffuse", 6.f}, {s(1.6), "mix", 0.f}, {s(1.8), "mix", 100.f},
          {s(2.0), "mix", 40.f}}},
        {{{s(0.4), "lo_cut", 0.f}, {s(0.5), "hi_cut", 0.f}, {s(0.6), "lo_cut", 1.f}, {s(0.7), "hi_cut", 1.f},
          {s(0.8), "spin", 0.f}, {s(0.9), "chorus", 0.f}, {s(1.0), "spin", 1.f}, {s(1.1), "chorus", 1.f},
          {s(1.2), "lo_shelf", 0.f}, {s(1.3), "hi_filter", 0.f}, {s(1.4), "hi_filter", 1.f}, {s(1.5), "hi_type", 1.f},
          {s(1.6), "lo_shelf", 1.f}, {s(1.7), "hi_type", 0.f}}},
        {{{s(0.4), "freeze", 1.f}, {s(0.7), "flat", 0.f}, {s(0.9), "flat", 1.f}, {s(1.0), "cut", 0.f},
          {s(1.3), "cut", 1.f}, {s(1.5), "freeze", 0.f}}},
        {{{s(0.4), "density", 0.f}, {s(0.8), "density", 3.f}, {s(1.2), "density", 1.f}, {s(1.21), "density", 2.f},
          {s(1.8), "density", 0.f}, {s(2.2), "density", 1.f}, {s(2.6), "density", 3.f}}},
    };
    Reverb steady;
    const Samples plain = steady.play(tone);
    const double base = clickiness(plain, s(0.3));
    const double bar = std::max(3.0 * base, 1e-4);
    for (size_t g = 0; g < groups.size(); ++g) {
        Reverb r;
        const Samples out = r.play(tone, groups[g].changes);
        CHECK(allFinite(out));
        const double got = clickiness(out, s(0.3));
        INFO("group " + std::to_string(g) + ": " + std::to_string(got) + ", steady " + std::to_string(base));
        CHECK(got < (groups[g].bar > 0.0 ? groups[g].bar : bar));
    }
    // What the measure makes of a click: 1 ms of the output gone, unfaded.
    Samples gap = plain;
    std::fill(gap.begin() + s(1.5), gap.begin() + s(1.501), 0.f);
    CHECK(clickiness(gap, s(0.3)) > 50.0 * 1e-3);

    // Smooth None: Size jumps (a fast pitch sweep: the point), bounded.
    Reverb none({{"smooth", 0.f}});
    const Samples swept = none.play(tone, {{s(0.5), "size", 400.f}, {s(1.0), "size", 0.22f}, {s(1.5), "size", 100.f}});
    CHECK(allFinite(swept));
    INFO("swept peak " + std::to_string(maxAbs(swept)) + ", steady " + std::to_string(maxAbs(plain)));
    CHECK(maxAbs(swept) < 4.0 * maxAbs(plain));
}

TEST_CASE("the reverb's automation plays through the engine sample-accurately") {
    sub::Engine engine;
    engine.setClipFadeMs(0);
    // (221.25 Hz: at its peak where the automation steps, so the first sample changed shows.)
    const Samples tone = smoothSine(221.25, 3.0);
    const std::string path = makeWav(stereo(tone), 2);
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, 0.0, 3.0, 0.0, 1.f)});
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "reverb", -1);
    setParam(engine, id, "mix", 0.f);
    const Samples dry = engine.renderOffline(0.0, 3 * kSampleRate);

    // Dry/Wet: none until beat 2 (1 s), then all of it.
    using Points = std::vector<sub::AutomationPoint>;
    engine.setTrackAutomation(track, {{id, "mix", Points{{0.0, 0.f, 0.f}, {2.0, 0.f, 0.f}, {2.0, 1.f, 0.f}}}});
    const Samples out = engine.renderOffline(0.0, 3 * kSampleRate);
    CHECK(allFinite(out));
    int64_t first = -1;
    for (size_t i = 0; i < out.size() && first < 0; ++i)
        if (out[i] != dry[i]) first = static_cast<int64_t>(i) / 2;
    CHECK_EQ(first, int64_t{kSampleRate});
    CHECK(!allclose(frames(out, kSampleRate + 2000, 2 * kSampleRate), frames(dry, kSampleRate + 2000, 2 * kSampleRate),
                    0.0, 0.01));
}

TEST_CASE("the reverb's reset and a new sample rate start it from silence") {
    // Spin and Chorus on: their LFOs start again too.
    const Values values = {{"mix", 100.f}};
    Reverb fresh(values);
    const Samples want = fresh.play(impulse(kSampleRate));
    Reverb r(values);
    r.play(noise(kSampleRate, 9));
    r.processor().reset();
    CHECK_ARRAY_EQUAL(r.play(impulse(kSampleRate)), want);

    // A new rate: tuned to it, from silence, as a device made at it.
    r.play(noise(kSampleRate, 10));
    r.prepare(96000.0);
    CHECK_EQ(r.rate(), 96000.0);
    Reverb at96(values, 96000.0);
    CHECK_ARRAY_EQUAL(r.play(impulse(96000)), at96.play(impulse(96000)));
    // And Size glides at Smooth's pace at the new rate, as on a device made at it.
    Reverb moved(values), made(values, 96000.0);
    moved.play(noise(kSampleRate / 2, 21));
    moved.prepare(96000.0);
    const Samples tone96 = smoothSine(440.0, 1.0, 96000.0);
    const std::vector<Change> grow = {{48000, "size", 300.f}};
    CHECK_ARRAY_EQUAL(moved.play(tone96, grow), made.play(tone96, grow));
    // The decay is Decay's at any rate.
    for (const double rate : {44100.0, 96000.0, 192000.0}) {
        const double t = impulseResponse(kQuiet, 3.5, rate).t60(1000.0, rate);
        INFO(std::to_string(rate) + " Hz: " + std::to_string(t) + " s");
        CHECK_APPROX_TOL(t, designDecay({}, 1000.0, rate), 0.15, 0.0);
    }
}

TEST_CASE("the reverb stays finite and decays at the extremes") {
    const std::vector<Values> settings = {
        {{"size", 0.22f}},
        {{"size", 500.f}},
        {{"decay", 200.f}},
        {{"decay", 60000.f}},
        {{"predelay", 250.f}, {"size", 500.f}, {"shape", 100.f}},
        {{"scale", 0.f}, {"size", 0.22f}},
        {{"scale", 100.f}, {"size", 500.f}},
        {{"diffusion", 0.f}},
        {{"diffusion", 100.f}, {"scale", 100.f}},
        {{"spin_amount", 100.f}, {"spin_rate", 1.3f}, {"chorus_amount", 100.f}, {"chorus_rate", 8.f}},
        {{"spin_amount", 100.f}, {"spin_rate", 1.3f}, {"chorus_amount", 100.f}, {"chorus_rate", 8.f},
         {"size", 0.22f}},
        {{"density", 0.f}, {"size", 0.22f}, {"decay", 60000.f}},
        {{"density", 1.f}, {"diffusion", 100.f}},
        {{"density", 2.f}, {"scale", 0.f}},
        {{"reflect", 6.f}, {"diffuse", 6.f}, {"decay", 60000.f}, {"size", 500.f}},
        {{"lo_gain", 20.f}, {"hi_gain", 20.f}, {"lo_freq", 15000.f}, {"hi_freq", 20.f}},
        {{"hi_type", 1.f}, {"hi_freq", 20.f}, {"in_freq", 18000.f}, {"in_width", 9.f}},
        {{"in_freq", 50.f}, {"in_width", 0.5f}, {"stereo", 120.f}},
    };
    const Samples x = silence(noise(static_cast<size_t>(2 * kSampleRate), 11), 4.0);
    for (const Values& values : settings) {
        INFO(describe(values));
        Reverb r(with(values, {{"mix", 100.f}}));
        const auto [l, rr] = r.play(x, x);
        REQUIRE(allFinite(l) && allFinite(rr));
        CHECK(std::max(maxAbs(l), maxAbs(rr)) < 20.0);
        const double first = energy(slice(l, frames(2.0), frames(3.0))), last = energy(slice(l, frames(5.0)));
        CHECK(last < first);
    }
}

TEST_CASE("the reverb takes NaN, infinity and absurd levels in its input as silence") {
    // One would circulate in the feedback network for good, and the guard can't see a NaN: the
    // device's input is cleaned before it (BuiltinProcessor::process()). A bad sample on one side, in
    // noise that goes on, plays exactly as a 0 there would: at the defaults, fully dry, and frozen
    // with the input still feeding the tail.
    const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity(), 1e31f};
    const Samples left = noise(2 * kSampleRate, 26), right = noise(2 * kSampleRate, 27);
    const int64_t at = frames(0.5) + 17;
    for (const Values& values : {Values{}, Values{{"mix", 0.f}}, Values{{"freeze", 1.f}, {"cut", 0.f}}}) {
        Samples zeroed = left;
        zeroed[static_cast<size_t>(at)] = 0.f;
        Reverb clean(values);
        const auto [wantL, wantR] = clean.play(zeroed, right);
        for (const float value : bad) {
            INFO(describe(values) + " given " + std::to_string(value));
            Samples broken = left;
            broken[static_cast<size_t>(at)] = value;
            Reverb r(values);
            const auto [l, rr] = r.play(broken, right);
            CHECK_ARRAY_EQUAL(l, wantL);
            CHECK_ARRAY_EQUAL(rr, wantR);
        }
    }
    // The loudest input it takes (BuiltinProcessor::kMaxInput), frozen with the input feeding the
    // tail, the guard holding it: every sample finite.
    const Samples loud = noise(kSampleRate, 28, sub::BuiltinProcessor::kMaxInput);
    Reverb frozen({{"freeze", 1.f}, {"cut", 0.f}, {"mix", 100.f}});
    const auto [l, rr] = frozen.play(loud, loud);
    CHECK(allFinite(l));
    CHECK(allFinite(rr));
}

TEST_CASE("the reverb's silence rings out to exact zeros") {
    // After a sound, once its tail dies away, it sleeps: exact zeros, never denormals.
    Reverb r;
    const Samples x = silence(noise(kSampleRate / 2, 12), 5.5);
    const auto [l, rr] = r.play(x, x);
    for (const Samples* out : {&l, &rr}) {
        const std::vector<int64_t> sounding = nonzero(*out);
        REQUIRE(!sounding.empty());
        INFO("last sound at " + std::to_string(static_cast<double>(sounding.back()) / kSampleRate) + " s");
        CHECK(sounding.back() < frames(4.5));
        for (const float v : *out) CHECK(v == 0.f || std::abs(v) >= std::numeric_limits<float>::min());
    }
    // Denormal input passes as finite numbers and dies away to zeros too (all
    // wet: the dry would pass it on).
    Reverb wet({{"mix", 100.f}});
    Samples tiny(static_cast<size_t>(3 * kSampleRate), 0.f);
    for (size_t i = 0; i < 1000; ++i) tiny[i] = (i % 2 ? 1e-40f : -1e-41f);
    const Samples quiet = wet.play(tiny);
    CHECK(allFinite(quiet));
    CHECK(allEqual(slice(quiet, kSampleRate), 0.0));
    // Frozen, it never sleeps: still sounding 10 s on.
    Reverb frozen({{"mix", 100.f}});
    const Samples held = frozen.play(silence(noise(kSampleRate / 2, 13), 10.0), {{frames(0.4), "freeze", 1.f}});
    CHECK(maxAbs(slice(held, frames(10.0))) > 1e-3);

    // Waking: a click after the noise and 10 s of silence plays as on a fresh device.
    Reverb woken(kQuiet), fresh(kQuiet);
    const Samples before = silence(noise(kSampleRate / 2, 14), 10.0);
    woken.play(before);
    const Samples click = impulse(kSampleRate);
    CHECK_ALLCLOSE(woken.play(click), fresh.play(click), 0.0, 1e-5);

    // Waking into silence: Predelay, Size and Shape raised while it slept reach further back into its
    // buffers, which hold nothing of what it heard before (cleared as it slept): a hit then plays as on a
    // device that never heard anything, given the same changes.
    {
        const Values values = {{"decay", 200.f}, {"mix", 100.f}, {"shape", 0.f}};
        Samples heard = silence(noise(kSampleRate, 22, 0.02f), 2.0), never(heard.size(), 0.f);
        heard[static_cast<size_t>(frames(2.0))] = never[static_cast<size_t>(frames(2.0))] = 0.5f;
        const std::vector<Change> grow = {
            {frames(1.5), "predelay", 250.f}, {frames(1.5), "size", 500.f}, {frames(1.5), "shape", 100.f}};
        Reverb a(values), b(values);
        const auto [al, ar] = a.play(heard, heard, grow);
        const auto [bl, br] = b.play(never, never, grow);
        CHECK(allEqual(slice(al, frames(1.5), frames(2.0)), 0.0));  // (asleep)
        CHECK_ARRAY_EQUAL(slice(al, frames(2.0)), slice(bl, frames(2.0)));
        CHECK_ARRAY_EQUAL(slice(ar, frames(2.0)), slice(br, frames(2.0)));
    }
    // The same just after it fell asleep, its buffers still being cleared a slice at a time: what the reads
    // reach is cleared just ahead of them, so it plays as one that never heard anything all the same.
    {
        const Values values = {{"decay", 200.f}, {"mix", 100.f}, {"shape", 0.f}};
        const Samples heard = silence(noise(kSampleRate / 2, 23, 0.3f), 2.0);
        Reverb probe(values);
        const int64_t asleep = nonzero(probe.play(heard)).back() + 1;  // (the wet is exact zeros from there)
        REQUIRE(asleep < frames(2.0));
        for (const int64_t after : {int64_t{32}, int64_t{100}, int64_t{400}}) {
            INFO("woken " + std::to_string(after) + " samples after falling asleep");
            const int64_t wake = asleep + after;
            Samples a = heard, b(heard.size(), 0.f);
            a[static_cast<size_t>(wake)] = b[static_cast<size_t>(wake)] = 0.5f;
            const std::vector<Change> grow = {
                {wake - 16, "predelay", 250.f}, {wake - 16, "size", 500.f}, {wake - 16, "shape", 100.f}};
            Reverb x(values), y(values);
            const Samples ax = x.play(a, grow), by = y.play(b, grow);
            CHECK(maxAbs(slice(ax, wake)) > 0.01);
            CHECK_ARRAY_EQUAL(slice(ax, wake), slice(by, wake));
        }
        // And with the delays jumping as it wakes (at each Smooth, turning back and on mid-glide): what the
        // reads reach is cleared as far as their glides have come, a sub-chunk ahead, and it plays the same.
        for (const float smooth : {0.f, 1.f, 2.f}) {
            INFO("woken as the delays jump, Smooth " + std::to_string(static_cast<int>(smooth)));
            const int64_t wake = asleep + 100;
            Samples a = heard, b(heard.size(), 0.f);
            for (size_t i = static_cast<size_t>(wake); i < a.size(); ++i)
                a[i] = b[i] = 0.3f * static_cast<float>(std::sin(0.01 * static_cast<double>(i)));
            const std::vector<Change> jump = {{wake, "size", 500.f},  {wake, "predelay", 250.f},
                                              {wake, "scale", 100.f}, {wake + 2000, "size", 1.f},
                                              {wake + 4000, "size", 400.f}};
            const Values smoothed = with(values, {{"smooth", smooth}});
            Reverb x(smoothed), y(smoothed);
            const Samples ax = x.play(a, jump), by = y.play(b, jump);
            CHECK(maxAbs(slice(ax, wake)) > 0.01);
            CHECK_ARRAY_EQUAL(slice(ax, wake), slice(by, wake));
        }
    }

    // Asleep with Spin on, its phase goes on (the editor's particles drift on).
    Reverb spinning({{"mix", 100.f}});
    spinning.play(silence(noise(kSampleRate / 4, 15), 6.0));
    const std::vector<float> phases = spinning.display("spin");
    REQUIRE(phases.size() > 100);
    const double step = 0.3 * 256 / kSampleRate;
    for (size_t i = phases.size() - 50; i < phases.size(); ++i) {
        const double d = phases[i] - phases[i - 1];
        CHECK_NEAR(d - std::round(d), step, 1e-4);
    }
}

TEST_CASE("the reverb's tail covers its ringing") {
    for (const Values& values :
         {Values{{"decay", 500.f}}, Values{{"decay", 1200.f}}, Values{{"decay", 5000.f}},
          Values{{"predelay", 250.f}, {"size", 500.f}}, Values{{"decay", 200.f}, {"size", 500.f}}}) {
        INFO(describe(values));
        Reverb r(with(values, {{"mix", 100.f}}));
        const auto tail = static_cast<size_t>(r.processor().tailSamples());
        const Samples h = r.play(impulse(2 * tail + kSampleRate));
        CHECK(energy(slice(h, static_cast<int64_t>(tail))) < 1e-6 * energy(h));
    }
    Reverb frozen({{"freeze", 1.f}});
    CHECK_EQ(frozen.processor().tailSamples(), 60 * kSampleRate);
    Reverb longest({{"decay", 60000.f}, {"predelay", 250.f}, {"size", 500.f}});
    CHECK(longest.processor().tailSamples() <= 60 * kSampleRate);
    CHECK(longest.processor().tailSamples() > 50 * kSampleRate);
}

TEST_CASE("the reverb's displays") {
    Reverb r;
    const Samples tone = sine(1000.0, 0.5, 0.5);
    // (Read before each block and after the last: a reader keeps up with the latest 8192 values of a display.)
    std::vector<float> signal, tail;
    const auto collect = [&] {
        const std::vector<float> s = r.display("signal"), t = r.display("tail");
        signal.insert(signal.end(), s.begin(), s.end());
        tail.insert(tail.end(), t.begin(), t.end());
    };
    Samples left = tone, right = tone;
    r.run({&left, &right}, {}, 256, [&](int64_t, int) { collect(); });
    collect();
    // The input's level (the mono sum: the tone itself), the input sample by sample.
    const std::vector<float> input = r.display("input");
    REQUIRE(input.size() == static_cast<size_t>(0.5 * kSampleRate) / 256);
    CHECK_NEAR(input.back(), -6.02, 0.5);
    CHECK_ALLCLOSE(signal, tone, 0.0, 0.0);
    const std::vector<float> early = r.display("early");
    CHECK(early.back() > -30.f);
    CHECK_EQ(r.display("diffuse").size(), input.size());
    CHECK_EQ(tail.size(), signal.size());

    // Once it stops, the tail's level falls at Decay's pace.
    r.play(Samples(static_cast<size_t>(4 * kSampleRate), 0.f), Samples(static_cast<size_t>(4 * kSampleRate), 0.f));
    const std::vector<float> diffuse = r.display("diffuse");
    const float peak = *std::max_element(diffuse.begin(), diffuse.end());
    double st = 0, sy = 0, stt = 0, sty = 0, count = 0;
    for (size_t i = 0; i < diffuse.size(); ++i) {
        if (diffuse[i] > peak - 10.f || diffuse[i] < peak - 40.f) continue;
        const double t = i * 256.0 / kSampleRate;
        st += t;
        sy += diffuse[i];
        stt += t * t;
        sty += t * diffuse[i];
        count += 1;
    }
    const double slope = (count * sty - st * sy) / (count * stt - st * st);
    INFO("the tail's level falls " + std::to_string(-slope) + " dB/s");
    CHECK_APPROX_TOL(-slope, 60.0 / 1.2, 0.25, 0.0);
    const std::vector<float> asleep = r.display("early");
    CHECK_EQ(asleep.back(), -90.f);
    CHECK_EQ(r.display("input").back(), -90.f);

    // The LFOs' phases, 256 samples apart, however the blocks fall.
    for (const int block : {256, 100}) {
        INFO("blocks of " + std::to_string(block));
        Reverb lfo({{"chorus_rate", 2.f}});
        lfo.play(noise(kSampleRate, 16), {}, block);
        const std::vector<float> spin = lfo.display("spin"), chorus = lfo.display("chorus");
        REQUIRE(spin.size() == static_cast<size_t>(kSampleRate) / 256);
        CHECK_EQ(chorus.size(), spin.size());
        const double ratio = 1.0 + 0.23 * (0.0 / 15.0 - 0.5);  // the first line's (High: line 0)
        for (size_t i = 1; i < spin.size(); ++i) {
            const double d = spin[i] - spin[i - 1], e = chorus[i] - chorus[i - 1];
            CHECK_NEAR(d - std::floor(d + 0.5), 0.3 * 256 / kSampleRate, 1e-4);
            CHECK_NEAR(e - std::floor(e + 0.5), 2.0 * ratio * 256 / kSampleRate, 1e-4);
        }
        CHECK_NEAR(spin[0], 0.3 * 256 / kSampleRate, 1e-5);
    }
    // Switched while it sleeps, they go (-1) or come (stepping) all the same.
    for (const float to : {0.f, 1.f}) {
        INFO(to > 0.f ? "switched on asleep" : "switched off asleep");
        Reverb asleep({{"spin", 1.f - to}, {"chorus", 1.f - to}});
        asleep.play(silence(noise(kSampleRate / 4, 23), 6.0));
        asleep.display("spin");
        asleep.display("chorus");
        CHECK_EQ(asleep.display("early").back(), -90.f);  // (asleep)
        asleep.play(Samples(static_cast<size_t>(2 * kSampleRate), 0.f), {{0, "spin", to}, {0, "chorus", to}});
        const std::vector<float> spin = asleep.display("spin"), chorus = asleep.display("chorus");
        REQUIRE(spin.size() > 100);
        if (to == 0.f) {
            CHECK_EQ(spin.back(), -1.f);
            CHECK_EQ(chorus.back(), -1.f);
        } else {
            const double ratio = 1.0 + 0.23 * (0.0 / 15.0 - 0.5);
            for (size_t i = spin.size() - 50; i < spin.size(); ++i) {
                const double d = spin[i] - spin[i - 1], e = chorus[i] - chorus[i - 1];
                CHECK_NEAR(d - std::floor(d + 0.5), 0.3 * 256 / kSampleRate, 1e-4);
                CHECK_NEAR(e - std::floor(e + 0.5), 0.8 * ratio * 256 / kSampleRate, 1e-4);
            }
        }
    }
    // Switched off, once its amount has glided away: -1.
    Reverb off;
    off.play(noise(kSampleRate, 17), {{kSampleRate / 2, "spin", 0.f}, {kSampleRate / 2, "chorus", 0.f}});
    const std::vector<float> spin = off.display("spin"), chorus = off.display("chorus");
    const size_t settled = static_cast<size_t>(0.8 * kSampleRate) / 256;
    CHECK(spin[settled - 100] >= 0.f);
    for (size_t i = settled; i < spin.size(); ++i) {
        CHECK_EQ(spin[i], -1.f);
        CHECK_EQ(chorus[i], -1.f);
    }
}

#ifdef NDEBUG
TEST_CASE("the reverb costs little") {
    // 10 s of stereo noise at the defaults (High, Spin and Chorus on), timed in the thread's CPU time (a busy
    // machine takes the core away, not the device's cost), the best of two: well under a tenth of real time
    // (it takes about 1.5 % of a core; the bound only catches a gross regression, such as denormals).
    const auto length = static_cast<size_t>(10 * kSampleRate);
    const Samples l = noise(length, 18), r = noise(length, 19);
    double best = 1e9;
    for (int attempt = 0; attempt < 2; ++attempt) {
        Reverb reverb;
        const double start = threadSeconds();
        reverb.play(l, r, {}, 256);
        best = std::min(best, threadSeconds() - start);
    }
    INFO("10 s took " + std::to_string(best) + " s");
    CHECK(best < 1.0);
}
#endif
