// The built-in Erosion: a 2 ms delay whose read position a sine or band-passed
// noise wobbles, phase-modulating the input. At Amount 0 it is a clean delay of
// its latency, which the engine compensates; a sine puts sidebands where theory
// says (Bessel functions of the modulation index) and Stereo puts the sides a
// quarter cycle apart; the noise modulates as hard at any Frequency and Width
// (its exact normalisation), and goes on doing so while the band moves a long
// way; two devices set alike don't share their noise; Stereo decorrelates its
// sides, highs erode before lows, Width spreads the sidebands; the output
// doesn't depend on how blocks are split; every control changes without a click
// and lands where it was turned; automation through the engine lands in its
// chunk, aligned; reset and a new rate start it afresh; it stays finite at the
// extremes; silence comes out as exact zeros, NaN and infinity in as silence;
// one channel is the left of two;
// its displays carry what the editor draws, and the band the editor draws is the
// filter that plays.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "Engine.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/Dsp.h"
#include "builtin/DspBlocks.h"
#include "builtin/ErosionDesign.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"
#include "harness/Standalone.h"

using namespace subtest;
namespace erosion = sub::erosion;

namespace {

constexpr int kD = 96;  // the delay's centre (the latency) at 48 kHz

using Values = ParamValues;
using Change = ParamChange;

// An Erosion on its own, outside an engine (harness/Standalone.h), its displays
// gathered from the start: read after every block (a stream keeps only its
// latest 8192 values). Each instance salts its noise with a number of its own;
// `alike` makes it as the first one made (the count set back to 0), so a test's
// devices share their noise, whichever tests ran before.
class Erosion : public Standalone {
public:
    explicit Erosion(double rate = kSampleRate, const Values& values = {}, bool alike = true)
        : Standalone(kind(alike), rate, values) {
        const std::vector<sub::DisplayInfo> infos = processor().displays();
        for (const sub::DisplayInfo& info : infos) ids_.push_back(info.id);
        positions_.assign(infos.size(), 0);
        displays_.assign(infos.size(), {});
    }

    // Processes one or two channels of equal length in place, `block` frames at a time (each a
    // Standalone run of its own, its changes from where it starts), the displays read after each.
    void run(const std::vector<Samples*>& channels, const std::vector<Change>& changes = {}, int block = 256) {
        const auto frames = static_cast<int64_t>(channels[0]->size());
        std::vector<Samples> pieces(channels.size());
        std::vector<Samples*> pointers;
        for (Samples& piece : pieces) pointers.push_back(&piece);
        size_t next = 0;
        for (int64_t start = 0; start < frames; start += block) {
            const int64_t end = std::min<int64_t>(start + block, frames);
            for (size_t c = 0; c < channels.size(); ++c)
                pieces[c].assign(channels[c]->begin() + start, channels[c]->begin() + end);
            std::vector<Change> here;
            for (; next < changes.size() && changes[next].frame < end; ++next) {
                here.push_back(changes[next]);
                here.back().frame = std::max<int64_t>(0, changes[next].frame - start);
            }
            Standalone::run(pointers, here, block);
            for (size_t c = 0; c < channels.size(); ++c)
                std::copy(pieces[c].begin(), pieces[c].end(), channels[c]->begin() + start);
            readDisplays();
        }
    }
    // One channel: what comes out.
    Samples play(Samples mono, const std::vector<Change>& changes = {}, int block = 256) {
        run({&mono}, changes, block);
        return mono;
    }
    // Two channels: what comes out of each.
    std::pair<Samples, Samples> playStereo(Samples left, Samples right, const std::vector<Change>& changes = {},
                                           int block = 256) {
        run({&left, &right}, changes, block);
        return {std::move(left), std::move(right)};
    }

    // Every value of a display since the device was made.
    const Samples& display(const std::string& id) const {
        for (size_t i = 0; i < ids_.size(); ++i)
            if (ids_[i] == id) return displays_[i];
        INFO(id);
        REQUIRE(false);
        return displays_[0];
    }

private:
    static std::string kind(bool alike) {
        if (alike) erosion::instancesMade.store(0);
        return "erosion";
    }

    void readDisplays() {
        for (size_t i = 0; i < ids_.size(); ++i)
            positions_[i] = processor().readDisplay(static_cast<int>(i), positions_[i], displays_[i]);
    }

    std::vector<std::string> ids_;
    std::vector<uint64_t> positions_;
    std::vector<Samples> displays_;
};

Samples noise(size_t length, unsigned seed, float amplitude = 0.5f) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> uniform(-amplitude, amplitude);
    Samples x(length);
    for (float& v : x) v = uniform(random);
    return x;
}

// A sine from phase 0, `amplitude` peak.
Samples tone(double freq, size_t length, double amplitude = 0.5, double rate = kSampleRate) {
    Samples x(length);
    for (size_t i = 0; i < length; ++i)
        x[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate));
    return x;
}

// A sine that fades in over 100 ms (no click of its own where it starts).
Samples smoothSine(double freq, double seconds, double rate = kSampleRate, double amplitude = 0.5) {
    Samples x(static_cast<size_t>(seconds * rate));
    const double fadeIn = 0.1 * rate;
    for (size_t i = 0; i < x.size(); ++i) {
        const double t = std::min(1.0, static_cast<double>(i) / fadeIn);
        x[i] = static_cast<float>(t * t * (3.0 - 2.0 * t) * amplitude * std::sin(2.0 * kPi * freq * i / rate));
    }
    return x;
}

// The DFT of x[from, from + n) at `freq` (one bin: n should hold whole cycles of it).
std::complex<double> binAt(const Samples& x, double freq, int64_t from, int64_t n, double rate = kSampleRate) {
    const std::complex<double> turn = std::polar(1.0, -2.0 * kPi * freq / rate);
    std::complex<double> w = 1.0, sum = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        sum += static_cast<double>(x[static_cast<size_t>(from + i)]) * w;
        w *= turn;
    }
    return sum;
}

// A component's amplitude: 2 |X| / n.
double amplitudeAt(const Samples& x, double freq, int64_t from, int64_t n, double rate = kSampleRate) {
    return 2.0 * std::abs(binAt(x, freq, from, n, rate)) / static_cast<double>(n);
}

// The energy of x[from, from + n) between lo and hi Hz (rfft bins).
double bandEnergy(const Samples& x, double lo, double hi, int64_t from, int64_t n, double rate = kSampleRate) {
    const std::vector<double> s = spectrum(slice(x, from, from + n));
    double sum = 0.0;
    for (size_t k = 0; k < s.size(); ++k) {
        const double f = static_cast<double>(k) * rate / static_cast<double>(n);
        if (f >= lo && f <= hi) sum += s[k] * s[k];
    }
    return sum;
}

// The largest 6th difference over [from, to): a steep high-pass (gain
// (2 sin(pi f / rate))^6), 8 times (18 dB) more sensitive at Nyquist than at a
// quarter of the sample rate and about 2·10^5 times more than at 2 kHz (48 kHz).
// A step of d shows as up to 20 d; a smooth signal well below Nyquist hardly at
// all.
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

// The Bessel function of the first kind J_n(x), from its series (25 terms: plenty below x = 3).
double besselJ(int n, double x) {
    double sum = 0.0, factM = 1.0, factMN = 1.0;
    for (int k = 1; k <= n; ++k) factMN *= k;
    for (int m = 0; m < 25; ++m) {
        if (m > 0) {
            factM *= m;
            factMN *= m + n;
        }
        sum += (m % 2 ? -1.0 : 1.0) / (factM * factMN) * std::pow(0.5 * x, 2 * m + n);
    }
    return sum;
}

std::vector<double> toDouble(const Samples& x) { return {x.begin(), x.end()}; }

// Pearson's correlation of two streams over [from, from + n).
double correlationOf(const Samples& a, const Samples& b, int64_t from, int64_t n) {
    return correlation(toDouble(slice(a, from, from + n)), toDouble(slice(b, from, from + n)));
}

// The input delayed by `delay` samples (zeros first).
Samples delayed(const Samples& x, int delay) {
    Samples out(x.size(), 0.f);
    for (size_t i = static_cast<size_t>(delay); i < x.size(); ++i) out[i] = x[i - static_cast<size_t>(delay)];
    return out;
}

// The noise's band-pass pair, as the device runs it: two TPT state-variable
// sections, each one's band-pass output (k v1) into the next.
struct Pair {
    sub::dsp::SvfCoefficients c;
    sub::dsp::Svf one, two;

    Pair(double freq, double width, double rate) {
        const erosion::Band band = erosion::band(freq, width, rate);
        c = sub::dsp::SvfCoefficients(static_cast<float>(band.g), static_cast<float>(band.k));
    }
    float tick(float x) {
        x = c.k * one.tick(c, x).band;
        return c.k * two.tick(c, x).band;
    }
};

// The same in double (the design's own numbers, to check its closed forms closely).
struct PairInDouble {
    double k, a1, a2, a3;
    double s[2][2] = {};

    PairInDouble(double freq, double width, double rate) {
        const erosion::Band band = erosion::band(freq, width, rate);
        k = band.k;
        a1 = 1.0 / (1.0 + band.g * (band.g + k));
        a2 = band.g * a1;
        a3 = band.g * a2;
    }
    double tick(double x) {
        for (auto& state : s) {
            const double v3 = x - state[1];
            const double v1 = a1 * state[0] + a2 * v3;
            const double v2 = state[1] + a2 * state[0] + a3 * v3;
            state[0] = 2.0 * v1 - state[0];
            state[1] = 2.0 * v2 - state[1];
            x = k * v1;
        }
        return x;
    }
};

}  // namespace

TEST_CASE("erosion is listed with its parameters") {
    const sub::BuiltinInfo info = builtinInfo("erosion");
    CHECK_EQ(info.name, std::string("Erosion"));
    CHECK(!info.isInstrument());
    CHECK(paramIds(info.params) == (std::vector<std::string>{"freq", "width", "amount", "blend", "stereo"}));
    struct Expected {
        const char* name;
        const char* unit;
        float min, max, def;
        bool log;
    };
    const Expected expected[] = {
        {"Frequency", "Hz", 20.f, 18000.f, 1000.f, true},   {"Filter Width", "oct", 0.1f, 10.f, 2.5f, true},
        {"Amount", "%", 0.f, 100.f, 25.f, false},           {"Noise Blend", "%", 0.f, 100.f, 100.f, false},
        {"Stereo Width", "%", 0.f, 100.f, 0.f, false},
    };
    for (size_t i = 0; i < info.params.size(); ++i) {
        const sub::ParamInfo& p = info.params[i];
        INFO(p.id);
        CHECK_EQ(p.name, std::string(expected[i].name));
        CHECK_EQ(p.unit, std::string(expected[i].unit));
        CHECK_EQ(p.minValue, expected[i].min);
        CHECK_EQ(p.maxValue, expected[i].max);
        CHECK_EQ(p.defaultValue, expected[i].def);
        CHECK_EQ(p.isLog(), expected[i].log);
        CHECK(p.valueLabels.empty());
        CHECK_EQ(p.stepCount(), 0);
        CHECK(p.automatable);
    }
    Erosion device;
    const std::vector<sub::DisplayInfo> displays = device.processor().displays();
    REQUIRE(displays.size() == 5);
    const std::pair<const char*, int> wanted[] = {
        {"input", 1}, {"output", 1}, {"erosion", 256}, {"mod_l", 1}, {"mod_r", 1}};
    for (size_t i = 0; i < displays.size(); ++i) {
        CHECK_EQ(displays[i].id, std::string(wanted[i].first));
        CHECK_EQ(displays[i].samplesPerValue, wanted[i].second);
    }

    // The engine makes it; it reports its 2 ms as latency and twice that as tail.
    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "erosion", -1);
    const sub::ProcessorInfo processor = engine.processorInfo(id);
    CHECK_EQ(processor.name, std::string("Erosion"));
    CHECK_EQ(processor.latency, kD);
    CHECK_EQ(processor.tail, 2 * kD);
}

TEST_CASE("erosion's latency is 2 ms at any rate") {
    const auto fresh = sub::BuiltinRegistry::instance().create("erosion");
    CHECK_EQ(fresh->latencySamples(), 96);  // unprepared: as at 48 kHz
    for (const auto& [rate, latency] : std::vector<std::pair<double, int>>{
             {44100.0, 88}, {48000.0, 96}, {96000.0, 192}, {192000.0, 384}, {22050.0, 44}}) {
        INFO(std::to_string(rate));
        Erosion device(rate);
        CHECK_EQ(device.processor().latencySamples(), latency);
        CHECK_EQ(device.processor().tailSamples(), 2 * latency);
        CHECK_EQ(erosion::centreDelaySamples(rate), latency);
    }
    CHECK_EQ(erosion::centreDelaySamples(1000.0), 4);  // never less than the interpolation needs
    CHECK_APPROX(erosion::maxExcursionSamples(48000.0), 94.0 / std::sqrt(2.0));
    CHECK_APPROX(erosion::excursionSamples(25.0, 48000.0), erosion::maxExcursionSamples(48000.0) / 16.0);
    CHECK_APPROX_TOL(erosion::excursionMs(100.0, 48000.0), 1.385, 0.0, 0.001);
    CHECK_APPROX_TOL(erosion::excursionMs(25.0, 48000.0), 0.0866, 0.0, 0.0001);
}

TEST_CASE("at Amount 0 erosion is a clean delay of its latency") {
    const Samples left = noise(kSampleRate / 2, 1), right = noise(kSampleRate / 2, 2);
    for (const Values& values : {Values{{"blend", 100.f}, {"stereo", 0.f}, {"freq", 1000.f}, {"width", 2.5f}},
                                 Values{{"blend", 0.f}, {"stereo", 100.f}, {"freq", 18000.f}, {"width", 0.1f}},
                                 Values{{"blend", 50.f}, {"stereo", 50.f}, {"freq", 20.f}, {"width", 10.f}}}) {
        Values all = values;
        all.push_back({"amount", 0.f});
        INFO(std::to_string(values[0].second) + " " + std::to_string(values[1].second));
        Erosion device(kSampleRate, all);
        const auto [l, r] = device.playStereo(left, right);
        CHECK_ARRAY_EQUAL(l, delayed(left, kD));
        CHECK_ARRAY_EQUAL(r, delayed(right, kD));
        CHECK(allEqual(slice(l, 0, kD), 0.0));
        const Samples& meter = device.display("erosion");
        CHECK_EQ(meter.size(), size_t{kSampleRate / 2 / 256});
        CHECK(allEqual(meter, -90.0));
        // The modulators run on all the same (the scope traces them, and when Amount rises the device goes
        // on as if it had eroded all along): they are what they are at any Amount.
        Values eroding = values;
        eroding.push_back({"amount", 25.f});
        Erosion other(kSampleRate, eroding);
        const auto eroded = other.playStereo(left, right);
        CHECK(!allclose(eroded.first, l, 0.0, 0.01));
        CHECK_ARRAY_EQUAL(device.display("mod_l"), other.display("mod_l"));
        CHECK_ARRAY_EQUAL(device.display("mod_r"), other.display("mod_r"));
    }
}

TEST_CASE("through the engine erosion at Amount 0 is transparent and aligned") {
    sub::Engine engine;
    engine.setClipFadeMs(0);
    // A click on the left of a dry track, and on the right of one through Erosion.
    constexpr int64_t kClick = 1000;
    stereoClickTrack(engine, 1.f, 0.f, kClick, 1.0);
    const uint32_t wet = stereoClickTrack(engine, 0.f, 1.f, kClick, 1.0);
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(wet), "erosion", -1);
    setParam(engine, id, "amount", 0.f);
    CHECK_EQ(engine.processorInfo(id).latency, kD);
    const Samples out = engine.renderOffline(0.0, kSampleRate / 2);
    const Samples left = channel(out, 0), right = channel(out, 1);
    CHECK_EQ(nonzero(left), (std::vector<int64_t>{kClick}));
    CHECK_EQ(nonzero(right), (std::vector<int64_t>{kClick}));  // where it was: the latency is compensated
    CHECK_EQ(right[kClick], left[kClick]);
}

TEST_CASE("erosion's sine modulates the phase as theory says") {
    // A 1 kHz tone through a 3 kHz sine at Amount 30: E = 66.468 * 0.09 samples, so a
    // modulation index of 2 pi 1000 E / 48000; the carrier keeps J0 of it, the first
    // sidebands (2 and 4 kHz) J1, the second (5 and 7 kHz) J2: within 0.1 % (they
    // agree to 4e-5, the Hermite read's error).
    const double e = erosion::maxExcursionSamples(kSampleRate) * 0.09;
    CHECK_APPROX_TOL(e, 5.9821, 0.0, 1e-4);
    const double beta = 2.0 * kPi * 1000.0 * e / kSampleRate;
    CHECK_APPROX_TOL(beta, 0.78306, 0.0, 1e-5);
    const Samples in = tone(1000.0, static_cast<size_t>(1.5 * kSampleRate));
    Erosion device(kSampleRate, {{"blend", 0.f}, {"stereo", 0.f}, {"freq", 3000.f}, {"amount", 30.f}});
    const auto [l, r] = device.playStereo(in, in);
    CHECK_ARRAY_EQUAL(l, r);
    const int64_t from = kSampleRate / 2, n = 4800;
    const std::pair<double, int> components[] = {{1000.0, 0}, {2000.0, 1}, {4000.0, 1}, {5000.0, 2}, {7000.0, 2}};
    for (const auto& [freq, order] : components) {
        INFO(std::to_string(freq) + " Hz");
        CHECK_APPROX_REL(amplitudeAt(l, freq, from, n), 0.5 * std::abs(besselJ(order, beta)), 1e-3);
    }
    CHECK_APPROX_TOL(0.5 * besselJ(0, beta), 0.4262, 0.0, 1e-4);
    CHECK_APPROX_TOL(0.5 * besselJ(1, beta), 0.1811, 0.0, 1e-4);
    CHECK_APPROX_TOL(0.5 * besselJ(2, beta), 0.0364, 0.0, 1e-4);
}

TEST_CASE("erosion's Frequency moves the sidebands") {
    const Samples in = tone(1000.0, static_cast<size_t>(1.5 * kSampleRate));
    const int64_t from = kSampleRate / 2, n = 4800;
    const auto run = [&](float freq, float width = 2.5f) {
        Erosion device(kSampleRate, {{"blend", 0.f}, {"amount", 20.f}, {"freq", freq}, {"width", width}});
        return device.play(in);
    };
    const Samples low = run(2500.f), high = run(6000.f);
    CHECK(amplitudeAt(low, 3500.0, from, n) > 10.0 * amplitudeAt(low, 7000.0, from, n));
    CHECK(amplitudeAt(high, 7000.0, from, n) > 10.0 * amplitudeAt(high, 3500.0, from, n));
    CHECK(amplitudeAt(low, 3500.0, from, n) > 0.01);
    CHECK(amplitudeAt(high, 7000.0, from, n) > 0.01);
    // And Filter Width does nothing to the sine: the noise's weight is exactly 0.
    CHECK_ARRAY_EQUAL(run(1000.f, 0.1f), run(1000.f, 10.f));
}

TEST_CASE("erosion's Stereo puts the sine's sides a quarter cycle apart") {
    const Samples in = tone(1000.0, static_cast<size_t>(1.5 * kSampleRate));
    const int64_t from = kSampleRate / 2, n = 4800;
    const auto degrees = [](std::complex<double> a, std::complex<double> b) {
        double d = (std::arg(a) - std::arg(b)) * 180.0 / kPi;
        while (d > 180.0) d -= 360.0;
        while (d < -180.0) d += 360.0;
        return d;
    };
    {
        Erosion device(kSampleRate, {{"blend", 0.f}, {"stereo", 100.f}, {"freq", 3000.f}, {"amount", 30.f}});
        const auto [l, r] = device.playStereo(in, in);
        CHECK_APPROX_TOL(std::abs(degrees(binAt(l, 4000.0, from, n), binAt(r, 4000.0, from, n))), 90.0, 0.0, 2.0);
        CHECK_APPROX_TOL(degrees(binAt(l, 1000.0, from, n), binAt(r, 1000.0, from, n)), 0.0, 0.0, 0.5);
        // The modulators themselves (every sample: whole cycles of 3 kHz, 16 samples each).
        CHECK(std::abs(correlationOf(device.display("mod_l"), device.display("mod_r"), from, n)) < 0.005);
    }
    {
        Erosion device(kSampleRate, {{"blend", 0.f}, {"stereo", 50.f}, {"freq", 3000.f}, {"amount", 30.f}});
        device.playStereo(in, in);
        CHECK_APPROX_TOL(correlationOf(device.display("mod_l"), device.display("mod_r"), from, n), 0.70711, 0.0, 0.005);
    }
    {
        Erosion device(kSampleRate, {{"blend", 0.f}, {"stereo", 0.f}, {"freq", 3000.f}, {"amount", 30.f}});
        device.playStereo(in, in);
        CHECK_ARRAY_EQUAL(device.display("mod_l"), device.display("mod_r"));
    }
}

TEST_CASE("erosion's noise modulates as hard at any Frequency and Width") {
    const Samples silence(static_cast<size_t>(4.25 * kSampleRate), 0.f);
    for (const auto& [freq, width] : std::vector<std::pair<float, float>>{
             {200.f, 1.f}, {500.f, 0.5f}, {2000.f, 2.5f}, {12000.f, 8.f}, {5000.f, 0.1f}}) {
        INFO(std::to_string(freq) + " Hz, " + std::to_string(width) + " oct");
        Erosion device(kSampleRate, {{"blend", 100.f}, {"stereo", 0.f}, {"freq", freq}, {"width", width}});
        device.playStereo(silence, silence);
        const Samples& mod = device.display("mod_l");
        REQUIRE(mod.size() == silence.size());
        CHECK_APPROX_REL(rms(slice(mod, kSampleRate / 4)), erosion::kNoiseRms, 0.05);
    }

    // The design: the pair's power gain in closed form is the sum of the squares of
    // its impulse response (in double; the float sections that play, close to it).
    for (const auto& [freq, width, rate] : std::vector<std::tuple<double, double, double>>{
             {1000.0, 2.5, 48000.0}, {5000.0, 0.1, 48000.0}, {12000.0, 8.0, 48000.0}, {18000.0, 10.0, 44100.0},
             {200.0, 1.0, 48000.0}}) {
        INFO(std::to_string(freq) + " Hz, " + std::to_string(width) + " oct at " + std::to_string(rate));
        const double g = std::tan(kPi * freq / rate), k = 1.0 / erosion::bandQ(width), a0 = 1.0 + g * k + g * g;
        const double gain = erosion::noisePowerGain(g * k / a0, 2.0 * (g * g - 1.0) / a0, (1.0 - g * k + g * g) / a0);
        PairInDouble exact(freq, width, rate);
        Pair played(freq, width, rate);
        double sum = 0.0, floatSum = 0.0;
        for (int i = 0; i < (1 << 18); ++i) {
            const double y = exact.tick(i == 0 ? 1.0 : 0.0);
            const double z = played.tick(i == 0 ? 1.f : 0.f);
            sum += y * y;
            floatSum += z * z;
        }
        CHECK_APPROX_REL(gain, sum, 1e-9);
        CHECK_APPROX_REL(gain, floatSum, 1e-4);
    }
    // Uniform noise through the pair, times the scale: RMS 1/sqrt 2.
    Pair pair(5000.0, 0.1, 48000.0);
    const double scale = erosion::band(5000.0, 0.1, 48000.0).scale;
    sub::dsp::Noise white;
    white.seed(12345);
    double sum = 0.0;
    constexpr int kCount = 1 << 20;
    for (int i = 0; i < kCount; ++i) {
        const double y = scale * pair.tick(white.next());
        sum += y * y;
    }
    CHECK_APPROX_REL(std::sqrt(sum / kCount), erosion::kNoiseRms, 0.01);
}

TEST_CASE("erosion's noise modulates as hard while its band moves a long way") {
    // The sections hold the energy they built at the old band, and the noise's gain is the new band's: moved
    // far down or widened, that energy would modulate several times too hard, and narrowed too weakly, for a
    // few hundred ms, until it decayed at the new band's rate. The modulator's RMS in each 100 ms after the
    // step, over 32 devices (each its own noise: a narrow band's level wanders slowly), stays near 1/sqrt 2.
    struct Move {
        const char* id;
        float to;
        float freq, width;
    };
    const Move moves[] = {{"freq", 30.f, 5000.f, 0.1f},
                          {"freq", 20.f, 18000.f, 0.1f},
                          {"width", 0.1f, 20.f, 10.f},
                          {"width", 10.f, 20.f, 0.1f}};
    constexpr int kRuns = 32, kWindows = 3;
    const int64_t window = kSampleRate / 10;
    for (const Move& move : moves) {
        INFO(std::string(move.id) + " to " + std::to_string(move.to) + " from " + std::to_string(move.freq) + " Hz, " +
             std::to_string(move.width) + " oct");
        erosion::instancesMade.store(0);  // (the devices' noises the same each time the test runs)
        std::vector<double> power(kWindows, 0.0);
        for (int run = 0; run < kRuns; ++run) {
            Erosion device(kSampleRate,
                           {{"blend", 100.f}, {"amount", 50.f}, {"freq", move.freq}, {"width", move.width}}, false);
            const int64_t at = kSampleRate + 661 * run;  // (the band settled; a little later each time)
            device.play(Samples(static_cast<size_t>(at + kWindows * window), 0.f), {{at, move.id, move.to}});
            const Samples& mod = device.display("mod_l");
            for (int w = 0; w < kWindows; ++w) {
                const double level = rms(slice(mod, at + w * window, at + (w + 1) * window));
                power[static_cast<size_t>(w)] += level * level / kRuns;
            }
        }
        for (int w = 0; w < kWindows; ++w) {
            const double level = std::sqrt(power[static_cast<size_t>(w)]);
            INFO("from " + std::to_string(100 * w) + " ms: " + std::to_string(level));
            CHECK(level < 1.5 * erosion::kNoiseRms);
            CHECK(level > erosion::kNoiseRms / 1.5);
        }
    }
}

TEST_CASE("two erosions set alike don't share their noise") {
    // Each instance salts its noises' seeds with a number of its own: two on double-tracked parts don't
    // wobble in lockstep, nor after a reset made together (an export's). Each repeats itself after one.
    const Values values = {{"blend", 100.f}, {"freq", 2000.f}, {"width", 5.f}, {"amount", 50.f}};
    const Samples in = tone(440.0, kSampleRate / 2);
    Erosion one(kSampleRate, values), two(kSampleRate, values, false);
    const Samples first = one.play(in), second = two.play(in);
    const int64_t from = 4800, n = kSampleRate / 2 - 4800;
    CHECK(!allclose(first, second, 0.0, 0.01));
    CHECK(std::abs(correlationOf(one.display("mod_l"), two.display("mod_l"), from, n)) < 0.05);
    one.processor().reset();
    two.processor().reset();
    CHECK_ARRAY_EQUAL(one.play(in), first);
    CHECK_ARRAY_EQUAL(two.play(in), second);
    const int64_t again = static_cast<int64_t>(in.size()) + from;
    CHECK(std::abs(correlationOf(one.display("mod_l"), two.display("mod_l"), again, n)) < 0.05);
    // The salts: the first made has the plain seeds; consecutive ones unrelated; a salted seed never 0.
    CHECK_EQ(sub::dsp::hash32(0), 0u);
    CHECK(sub::dsp::hash32(1) != sub::dsp::hash32(2));
    CHECK_EQ(erosion::saltedSeed(erosion::kMidSeed, erosion::kMidSeed), erosion::kMidSeed);
}

TEST_CASE("erosion's wide noise is independent per side") {
    const int64_t from = kSampleRate / 4, n = 2 * kSampleRate;
    const Samples in = tone(1000.0, static_cast<size_t>(from + n));
    const auto run = [&](float stereo) {
        Erosion device(kSampleRate, {{"blend", 100.f}, {"freq", 2000.f}, {"width", 5.f}, {"stereo", stereo}});
        const auto [l, r] = device.playStereo(in, in);
        return std::tuple<Samples, Samples, Samples, Samples>{l, r, device.display("mod_l"), device.display("mod_r")};
    };
    {
        const auto [l, r, ml, mr] = run(0.f);
        CHECK_ARRAY_EQUAL(ml, mr);
        CHECK_ARRAY_EQUAL(l, r);
    }
    {
        const auto [l, r, ml, mr] = run(50.f);
        CHECK_APPROX_TOL(correlationOf(ml, mr, from, n), 0.6, 0.0, 0.08);
    }
    {
        const auto [l, r, ml, mr] = run(100.f);
        CHECK(std::abs(correlationOf(ml, mr, from, n)) < 0.08);
        CHECK(!allclose(l, r, 0.0, 0.01));
        CHECK_APPROX_REL(rms(slice(ml, from)), erosion::kNoiseRms, 0.05);  // each side as strong as in mono
        CHECK_APPROX_REL(rms(slice(mr, from)), erosion::kNoiseRms, 0.05);
    }
    // The design: the sides' correlation is (1 - w²) / (1 + w²), each keeping its RMS.
    const erosion::Spread half = erosion::stereoSpread(50.0);
    CHECK_APPROX(half.mid * half.mid - half.side * half.side, 0.6);
    CHECK_APPROX(half.mid * half.mid + half.side * half.side, 1.0);
    CHECK_APPROX(erosion::stereoSpread(100.0).half, kPi / 4.0);
}

TEST_CASE("erosion takes high frequencies first") {
    // Noise at 1 kHz, 2.5 octaves, Amount 40 (E = 10.63 samples): the phase noise
    // is 0.2 rad on 200 Hz, 4.9 rad on 5 kHz.
    const int64_t length = 2 * kSampleRate;
    Samples in(static_cast<size_t>(length));
    const Samples a = tone(200.0, in.size(), 0.25), b = tone(5000.0, in.size(), 0.25);
    for (size_t i = 0; i < in.size(); ++i) in[i] = a[i] + b[i];
    Erosion device(kSampleRate, {{"blend", 100.f}, {"freq", 1000.f}, {"width", 2.5f}, {"amount", 40.f}});
    const Samples out = device.play(in);
    const int64_t from = kSampleRate / 2, n = kSampleRate;
    INFO("200 Hz " + std::to_string(amplitudeAt(out, 200.0, from, n)) + ", 5 kHz " +
         std::to_string(amplitudeAt(out, 5000.0, from, n)));
    CHECK(amplitudeAt(out, 200.0, from, n) >= 0.95 * 0.25);
    CHECK(amplitudeAt(out, 5000.0, from, n) <= 0.1 * 0.25);
    // What wasn't kept went into hiss around it, not away: the level stays.
    CHECK_APPROX_REL(rms(slice(out, from, from + n)), rms(slice(in, from, from + n)), 0.05);
}

TEST_CASE("erosion's Width spreads the noise's sidebands") {
    const Samples in = tone(1000.0, static_cast<size_t>(1.5 * kSampleRate));
    const int64_t from = kSampleRate / 2, n = kSampleRate;
    const auto share = [&](float width) {
        Erosion device(kSampleRate, {{"blend", 100.f}, {"freq", 6000.f}, {"amount", 20.f}, {"width", width}});
        const Samples out = device.play(in);
        const double all = bandEnergy(out, 0.0, 24000.0, from, n) - bandEnergy(out, 950.0, 1050.0, from, n);
        return (bandEnergy(out, 4000.0, 8000.0, from, n)) / all;
    };
    const double narrow = share(0.2f), wide = share(6.f);
    INFO("narrow " + std::to_string(narrow) + ", wide " + std::to_string(wide));
    CHECK(narrow > 0.8);
    CHECK(wide < 0.35);
}

TEST_CASE("erosion renders the same however the block is split") {
    const Samples left = noise(kSampleRate / 2, 3), right = noise(kSampleRate / 2, 4);
    // Every parameter, at odd frames (several within one 16-sample chunk) and on chunk starts (multiples of 16).
    const std::vector<Change> changes = {
        {1001, "freq", 3000.f},  {1003, "amount", 70.f},  {1009, "width", 0.5f},  {1024, "blend", 20.f},
        {1024, "stereo", 100.f}, {5003, "freq", 300.f},   {6400, "amount", 10.f}, {9001, "width", 8.f},
        {12000, "blend", 90.f},  {15005, "stereo", 0.f},  {15006, "amount", 95.f}, {15007, "freq", 12000.f},
        {17777, "width", 0.1f},  {20000, "freq", 20.f},   {20001, "blend", 0.f},  {22011, "stereo", 60.f},
    };
    const Values values = {{"blend", 50.f}, {"stereo", 50.f}, {"amount", 40.f}};
    Erosion reference(kSampleRate, values);
    const auto [l, r] = reference.playStereo(left, right, changes, 1024);
    CHECK(!allclose(l, delayed(left, kD), 0.0, 0.01));
    for (const int block : {1, 7, 64}) {
        INFO("blocks of " + std::to_string(block));
        Erosion device(kSampleRate, values);
        const auto [bl, br] = device.playStereo(left, right, changes, block);
        CHECK_ARRAY_EQUAL(bl, l);
        CHECK_ARRAY_EQUAL(br, r);
        for (const char* id : {"input", "output", "erosion", "mod_l", "mod_r"}) {
            INFO(id);
            CHECK_ARRAY_EQUAL(device.display(id), reference.display(id));
        }
    }
}

TEST_CASE("changing any of erosion's controls is click-free") {
    // A 220 Hz tone through a 40 Hz sine at Amount 60, each control jumping at 1 s,
    // as automation's steps make them; against the same jump made by splicing a
    // device at the old value to one at the new (what an unsmoothed change would be).
    const Samples in = smoothSine(220.0, 2.0);
    constexpr int64_t kAt = kSampleRate;
    const int64_t from = kAt - 480, to = kAt + 4800;
    const Values base = {{"blend", 0.f}, {"freq", 40.f}, {"width", 0.2f}, {"stereo", 0.f}, {"amount", 60.f}};
    const auto with = [&](Values values, const std::string& id, float value) {
        for (auto& [name, v] : values)
            if (name == id) v = value;
        return values;
    };
    struct Step {
        std::string id;
        float before, after;
        Values base;
    };
    const Values noisy = with(base, "blend", 100.f);
    const std::vector<Step> steps = {{"amount", 0.f, 60.f, base},
                                     {"freq", 40.f, 80.f, base},
                                     {"stereo", 0.f, 100.f, base},
                                     {"blend", 0.f, 100.f, base},
                                     {"width", 0.2f, 0.4f, noisy}};
    for (const Step& step : steps) {
        INFO(step.id);
        const auto clickinessOf = [&](const std::pair<Samples, Samples>& out) {
            return std::max(clickiness(out.first, from, to), clickiness(out.second, from, to));
        };
        Erosion smooth(kSampleRate, with(step.base, step.id, step.before));
        const auto out = smooth.playStereo(in, in, {{kAt, step.id, step.after}});
        CHECK(allFinite(out.first) && allFinite(out.second));
        const double smoothed = clickinessOf(out);

        Erosion before(kSampleRate, with(step.base, step.id, step.before));
        Erosion after(kSampleRate, with(step.base, step.id, step.after));
        auto spliced = before.playStereo(in, in);
        const auto later = after.playStereo(in, in);
        std::copy(later.first.begin() + kAt, later.first.end(), spliced.first.begin() + kAt);
        std::copy(later.second.begin() + kAt, later.second.end(), spliced.second.begin() + kAt);
        const double click = clickinessOf(spliced);
        INFO("smoothed " + std::to_string(smoothed) + ", spliced " + std::to_string(click));
        CHECK(smoothed < 3e-4);
        CHECK(click > 30.0 * smoothed);

        // And it lands where it was turned: from 100 ms after the step (95 % of a glide takes about 50 ms)
        // the device plays as one set there all along does (the same noise: alike devices).
        const int64_t landed = kAt + 4800, n = static_cast<int64_t>(in.size()) - landed;
        if (step.id == "freq") {  // (the sine's phase differs, so where its energy is: 80 Hz, not 40)
            CHECK(amplitudeAt(smooth.display("mod_l"), 80.0, landed, kSampleRate / 2) > 0.98);
            CHECK(amplitudeAt(smooth.display("mod_l"), 40.0, landed, kSampleRate / 2) < 0.02);
        } else {
            for (const char* id : {"mod_l", "mod_r"}) {
                INFO(id);
                CHECK(correlationOf(smooth.display(id), after.display(id), landed, n) > 0.999);
            }
            CHECK(correlationOf(out.first, later.first, landed, n) > 0.999);
            CHECK(correlationOf(out.second, later.second, landed, n) > 0.999);
        }
    }
}

TEST_CASE("erosion's automation plays through the engine") {
    // (221.25 Hz: at its peak where the automation steps, so the first sample changed shows.)
    const Samples clipTone = smoothSine(221.25, 3.0);
    const std::string path = makeWav(stereo(clipTone), 2);
    const auto render = [&](bool device, bool automate) {
        sub::Engine engine;
        engine.setClipFadeMs(0);
        engine.loadSource(path);
        const uint32_t track = engine.addTrack();
        engine.setTrackClips(track, {clip(path, 0.0, 3.0, 0.0, 1.f)});
        if (device) {
            const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "erosion", -1);
            setParam(engine, id, "amount", 0.f);
            if (automate) {
                using Points = std::vector<sub::AutomationPoint>;
                CHECK_EQ(paramInfo(engine, id, "amount").fromNormalized(0.5f), 50.f);
                const Points points{{0.0, 0.f, 0.f}, {2.0, 0.f, 0.f}, {2.0, 0.5f, 0.f}};
                engine.setTrackAutomation(track, {{id, "amount", points}});
            }
        }
        return engine.renderOffline(0.0, 3 * kSampleRate);
    };
    const Samples clipAlone = render(false, false);
    const Samples untouched = render(true, false);  // Amount 0: the clip itself, latency compensated
    CHECK_ARRAY_EQUAL(untouched, clipAlone);

    // Amount from 0 to 50 % at beat 2 (1 s): the read position at t reads the input
    // from t - D, so (aligned) the change is heard D samples early, in the chunk it lands in.
    const Samples out = render(true, true);
    CHECK(allFinite(out));
    int64_t first = -1;
    for (size_t i = 0; i < out.size() && first < 0; ++i)
        if (out[i] != untouched[i]) first = static_cast<int64_t>(i) / 2;
    INFO("first difference at " + std::to_string(first));
    CHECK(first >= kSampleRate - kD);
    CHECK(first <= kSampleRate - kD + 32);
    CHECK(!allclose(frames(out, kSampleRate * 21 / 20, 2 * kSampleRate),
                    frames(untouched, kSampleRate * 21 / 20, 2 * kSampleRate), 0.0, 0.01));
}

TEST_CASE("reset and a new sample rate start erosion afresh") {
    const Values values = {{"blend", 60.f}, {"stereo", 70.f}, {"amount", 50.f}, {"freq", 15000.f}};
    const Samples a = noise(kSampleRate / 2, 5), b = noise(kSampleRate / 2, 6);
    Erosion fresh(kSampleRate, values);
    const auto want = fresh.playStereo(a, b);
    Erosion device(kSampleRate, values);
    device.playStereo(b, a);
    device.processor().reset();
    const auto got = device.playStereo(a, b);
    CHECK_ARRAY_EQUAL(got.first, want.first);
    CHECK_ARRAY_EQUAL(got.second, want.second);

    // A new rate: its latency and output as a fresh device's there (at 32 kHz the modulator is held to
    // 0.45 of the rate, 14.4 kHz).
    for (const auto& [rate, latency] : {std::pair{96000.0, 192}, std::pair{32000.0, 64}}) {
        INFO(std::to_string(rate));
        device.processor().prepare(rate, Erosion::kMaxBlock);
        CHECK_EQ(device.processor().latencySamples(), latency);
        Erosion there(rate, values);
        const Samples c = noise(48000, 7);
        CHECK_ARRAY_EQUAL(device.play(c), there.play(c));
    }
}

TEST_CASE("erosion stays stable at the extremes") {
    for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
        const Samples left = noise(static_cast<size_t>(rate), 8, 1.f), right = noise(static_cast<size_t>(rate), 9, 1.f);
        for (const float freq : {20.f, 18000.f}) {
            for (const float width : {0.1f, 10.f}) {
                for (const float blend : {0.f, 50.f, 100.f}) {
                    for (const float stereo : {0.f, 100.f}) {
                        INFO(std::to_string(rate) + ": " + std::to_string(freq) + " Hz, " + std::to_string(width) +
                             " oct, blend " + std::to_string(blend) + ", stereo " + std::to_string(stereo));
                        Erosion device(rate, {{"freq", freq}, {"width", width}, {"blend", blend},
                                              {"stereo", stereo}, {"amount", 100.f}});
                        const auto [l, r] = device.playStereo(left, right, {}, 1024);
                        CHECK(allFinite(l) && allFinite(r));
                        CHECK(maxAbs(l) <= 1.3);
                        CHECK(maxAbs(r) <= 1.3);
                        CHECK(allFinite(device.display("mod_l")) && allFinite(device.display("mod_r")));
                    }
                }
            }
        }
    }
    // Every control swept across its range at once, every few milliseconds.
    std::mt19937 random(11);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::vector<Change> changes;
    for (int64_t at = 0; at < 3 * kSampleRate;) {
        at += static_cast<int64_t>(kSampleRate * (0.002 + 0.05 * unit(random)));
        changes.push_back({at, "freq", static_cast<float>(20.0 * std::pow(900.0, unit(random)))});
        changes.push_back({at, "width", static_cast<float>(0.1 * std::pow(100.0, unit(random)))});
        changes.push_back({at, "amount", static_cast<float>(100.0 * unit(random))});
        changes.push_back({at, "blend", static_cast<float>(100.0 * unit(random))});
        changes.push_back({at, "stereo", static_cast<float>(100.0 * unit(random))});
    }
    Erosion device(kSampleRate, {{"amount", 100.f}});
    const auto [l, r] = device.playStereo(noise(3 * kSampleRate, 10, 1.f), noise(3 * kSampleRate, 11, 1.f), changes);
    CHECK(allFinite(l) && allFinite(r));
    CHECK(maxAbs(l) <= 1.3);
    CHECK(allFinite(device.display("mod_l")));
    CHECK(maxAbs(device.display("mod_l")) < 10.0);
}

TEST_CASE("silence comes out of erosion as exact zeros") {
    Erosion device(kSampleRate, {{"amount", 100.f}, {"blend", 50.f}, {"stereo", 100.f}});
    Samples x = noise(kSampleRate / 2, 12);
    x.resize(static_cast<size_t>(1.5 * kSampleRate), 0.f);
    const auto [l, r] = device.playStereo(x, x);
    for (const Samples* out : {&l, &r}) {
        CHECK(anyNonzero(slice(*out, kSampleRate / 2 - 100, kSampleRate / 2 + 2 * kD)));
        CHECK(allEqual(slice(*out, kSampleRate / 2 + 2 * kD), 0.0));
        for (const float v : *out) CHECK(v == 0.f || std::abs(v) >= std::numeric_limits<float>::min());
    }
    // Denormal input passes as finite numbers, and is gone 2D samples after it.
    Samples tiny(kSampleRate / 2, 0.f);
    for (size_t i = 0; i < 1000; ++i) tiny[i] = (i % 2 ? 1e-40f : -1e-41f);
    const auto [tl, tr] = device.playStereo(tiny, tiny);
    CHECK(allFinite(tl) && allFinite(tr));
    CHECK(allEqual(slice(tl, 1000 + 2 * kD), 0.0));
    CHECK(allEqual(slice(tr, 1000 + 2 * kD), 0.0));
}

TEST_CASE("erosion takes NaN and infinity in its input as silence") {
    // BuiltinProcessor::process() zeroes them before the device hears them, so its line never holds one: what
    // comes out is exactly what the same input with a 0 there gives, finite throughout, and it plays on.
    const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity(), 3e38f};
    const Values values = {{"amount", 60.f}, {"blend", 50.f}, {"stereo", 100.f}};
    const Samples left = noise(kSampleRate / 2, 21), right = noise(kSampleRate / 2, 22);
    Samples zeroedLeft = left, zeroedRight = right;
    zeroedLeft[1000] = 0.f;
    zeroedRight[3001] = 0.f;
    Erosion clean(kSampleRate, values);
    const auto want = clean.playStereo(zeroedLeft, zeroedRight);
    CHECK(!allclose(want.first, delayed(left, kD), 0.0, 0.01));
    for (const float value : bad) {
        INFO(std::to_string(value));
        Samples l = left, r = right;
        l[1000] = value;
        r[3001] = value;
        Erosion device(kSampleRate, values);
        const auto [gotLeft, gotRight] = device.playStereo(l, r);
        CHECK(allFinite(gotLeft) && allFinite(gotRight));
        CHECK_ARRAY_EQUAL(gotLeft, want.first);
        CHECK_ARRAY_EQUAL(gotRight, want.second);
        for (const char* id : {"input", "output", "erosion"}) {
            INFO(id);
            CHECK(allFinite(device.display(id)));
        }
    }
}

TEST_CASE("erosion on one channel is the left of two") {
    const Samples x = noise(kSampleRate / 2, 13);
    for (const float stereo : {0.f, 100.f}) {
        INFO(std::to_string(stereo));
        const Values values = {{"blend", 50.f}, {"stereo", stereo}, {"amount", 60.f}};
        Erosion mono(kSampleRate, values), both(kSampleRate, values);
        const Samples out = mono.play(x);
        const auto [l, r] = both.playStereo(x, x);
        CHECK_ARRAY_EQUAL(out, l);
        CHECK_ARRAY_EQUAL(mono.display("mod_l"), both.display("mod_l"));
        CHECK_ARRAY_EQUAL(mono.display("mod_r"), mono.display("mod_l"));
        if (stereo > 0.f) CHECK(!allclose(l, r, 0.0, 0.01));
    }
    // From two channels to one and back: the second starts from silence again (what it held from before the
    // one-channel stretch is stale), as it does from one channel to two.
    Erosion device(kSampleRate, {{"amount", 0.f}});
    {
        Samples a = noise(4800, 19), b = noise(4800, 20);
        device.run({&a, &b});
    }
    device.play(noise(4800, 14));
    Samples l = noise(4800, 15), r = noise(4800, 16);
    const Samples rightIn = r;
    device.run({&l, &r});
    CHECK_ARRAY_EQUAL(r, delayed(rightIn, kD));
}

TEST_CASE("erosion's displays") {
    const Samples left = noise(9600, 17), right = noise(9600, 18);
    Erosion device(kSampleRate, {{"amount", 50.f}, {"blend", 50.f}, {"stereo", 50.f}});
    const auto [l, r] = device.playStereo(left, right, {}, 960);
    Samples inSum(left.size()), outSum(left.size());
    for (size_t i = 0; i < left.size(); ++i) {
        inSum[i] = 0.5f * (left[i] + right[i]);
        outSum[i] = 0.5f * (l[i] + r[i]);
    }
    CHECK_ARRAY_EQUAL(device.display("input"), inSum);
    CHECK_ARRAY_EQUAL(device.display("output"), outSum);
    CHECK_EQ(device.display("mod_l").size(), size_t{9600});
    CHECK_EQ(device.display("mod_r").size(), size_t{9600});
    CHECK_EQ(device.display("erosion").size(), size_t{37});  // the meter runs on across blocks

    // One channel: the channel itself.
    Erosion mono(kSampleRate, {{"amount", 50.f}});
    const Samples out = mono.play(left, {}, 960);
    CHECK_ARRAY_EQUAL(mono.display("input"), left);
    CHECK_ARRAY_EQUAL(mono.display("output"), out);

    // The sine's modulator reaches its peak (every sample is published).
    Erosion sine(kSampleRate, {{"blend", 0.f}, {"freq", 1000.f}, {"stereo", 0.f}});
    sine.playStereo(left, right, {}, 960);
    const double peak = maxAbs(sine.display("mod_l"));
    CHECK(peak >= 0.9999);
    CHECK(peak <= 1.0001);

    // The `erosion` meter: what changed, in dB (exactly the floor at Amount 0).
    const Samples high = tone(3000.0, 9600);
    Erosion eroding(kSampleRate, {{"amount", 50.f}});
    eroding.play(high);
    const Samples meter = slice(eroding.display("erosion"), 2);
    CHECK(!meter.empty());
    CHECK(*std::min_element(meter.begin(), meter.end()) > -30.f);
    Erosion clean(kSampleRate, {{"amount", 0.f}});
    clean.play(high);
    CHECK(allEqual(clean.display("erosion"), -90.0));
}

TEST_CASE("the erosion band the editor draws is the filter that plays") {
    struct Setting {
        double freq, width, rate;
    };
    for (const Setting& s : {Setting{1000.0, 2.5, 48000.0}, Setting{15000.0, 0.5, 44100.0},
                             Setting{30.0, 10.0, 48000.0}, Setting{20.0, 0.1, 48000.0}}) {
        INFO(std::to_string(s.freq) + " Hz, " + std::to_string(s.width) + " oct at " + std::to_string(s.rate));
        Pair pair(s.freq, s.width, s.rate);
        Samples h(1 << 20);
        for (size_t i = 0; i < h.size(); ++i) h[i] = pair.tick(i == 0 ? 1.f : 0.f);
        const int64_t n = static_cast<int64_t>(h.size());
        int compared = 0;
        for (int i = 0; i < 20; ++i) {
            const double f = 10.0 * std::pow(0.45 * s.rate / 10.0, i / 19.0);
            const double drawn = erosion::bandMagnitude(s.freq, s.width, s.rate, f);
            if (20.0 * std::log10(drawn) < -60.0) continue;
            INFO("at " + std::to_string(f) + " Hz");
            const double played = std::abs(binAt(h, f, 0, n, s.rate));
            CHECK_APPROX_TOL(20.0 * std::log10(played), 20.0 * std::log10(drawn), 0.0, 0.01);
            ++compared;
        }
        CHECK(compared >= 5);
        // Its -3 dB edges, and 1 at its centre.
        const auto [lo, hi] = erosion::bandEdges(s.freq, s.width, s.rate);
        CHECK(lo < s.freq);
        CHECK(hi > s.freq);
        CHECK_APPROX_TOL(20.0 * std::log10(erosion::bandMagnitude(s.freq, s.width, s.rate, lo)), -3.0103, 0.0, 0.01);
        CHECK_APPROX_TOL(20.0 * std::log10(erosion::bandMagnitude(s.freq, s.width, s.rate, hi)), -3.0103, 0.0, 0.01);
        CHECK_APPROX(erosion::bandMagnitude(s.freq, s.width, s.rate, s.freq), 1.0);
    }
    // Width octaves between the edges (analog, so exactly away from Nyquist).
    const auto [lo, hi] = erosion::bandEdges(1000.0, 2.5, 192000.0);
    CHECK_APPROX_TOL(std::log2(hi / lo), 2.5, 0.0, 0.01);
    CHECK_EQ(erosion::bandMagnitude(1000.0, 2.5, 48000.0, 24000.0), 0.0);
    CHECK_APPROX(erosion::modFrequency(18000.0, 22050.0), 9922.5);

    // Noise Blend's weights are equal power, and exact at the ends.
    CHECK_EQ(erosion::blendWeights(0.0).sine, 1.0);
    CHECK_EQ(erosion::blendWeights(0.0).noise, 0.0);
    CHECK_EQ(erosion::blendWeights(100.0).sine, 0.0);
    CHECK_EQ(erosion::blendWeights(100.0).noise, 1.0);
    const erosion::Weights even = erosion::blendWeights(50.0);
    CHECK_APPROX(even.sine * even.sine + even.noise * even.noise, 1.0);
    CHECK_APPROX(even.sine, even.noise);
}
