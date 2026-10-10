// The built-in Disperser: a cascade of all-passes that delays what is around a
// frequency. Its magnitude stays flat and its group delay is the design's (the
// curve its editor draws), at the extremes and at any sample rate; however its
// controls move it puts out no more energy than it was given; Dry/Wet blends
// the input with its dispersed copy in time; each channel is
// its own; changes of every control are click-free; reset, silence and a new
// sample rate start it cleanly; its tail covers its ringing; and the engine
// keeps its dispersion (no latency to compensate).

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Engine.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/DisperserDesign.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"
#include "harness/Standalone.h"

using namespace subtest;
namespace disperser = sub::disperser;

namespace {

constexpr int kBlock = 1024;  // the renderer's largest block (Renderer::kMaxBlock)

using Values = ParamValues;
using Change = ParamChange;

// A Disperser on its own, outside an engine (harness/Standalone.h).
struct Disperser : Standalone {
    explicit Disperser(double rate = kSampleRate, const Values& values = {}) : Standalone("disperser", rate, values) {}
};

Samples impulse(size_t length) {
    Samples x(length, 0.f);
    x[0] = 1.f;
    return x;
}

Samples noise(size_t length, unsigned seed, float amplitude = 0.5f) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> uniform(-amplitude, amplitude);
    Samples x(length);
    for (float& v : x) v = uniform(random);
    return x;
}

// A sine that fades in over 100 ms: the stages never hear it start abruptly
// (which they would smear into a chirp, as they should: that is the effect).
Samples smoothSine(double freq, double seconds, double rate = kSampleRate, double amplitude = 0.5) {
    Samples x(static_cast<size_t>(seconds * rate));
    const double fadeIn = 0.1 * rate;
    for (size_t i = 0; i < x.size(); ++i) {
        const double t = std::min(1.0, static_cast<double>(i) / fadeIn);
        x[i] = static_cast<float>(t * t * (3.0 - 2.0 * t) * amplitude * std::sin(2.0 * kPi * freq * i / rate));
    }
    return x;
}

// The largest 6th difference over [from, to): a steep high-pass, about 64 times
// (36 dB) more sensitive at Nyquist than at a quarter of the sample rate and
// 10^5 times more than at 2 kHz (48 kHz). A step of d shows as up to 20 d; a
// smooth signal well below Nyquist hardly at all.
double clickiness(const Samples& x, int64_t from = 0, int64_t to = -1) {
    std::vector<double> d(x.begin(), x.end());
    for (int k = 0; k < 6; ++k)
        for (size_t i = d.size() - 1; i > 0; --i) d[i] -= d[i - 1];
    if (to < 0) to = static_cast<int64_t>(d.size());
    double worst = 0.0;
    for (int64_t i = std::max<int64_t>(from, 6); i < to; ++i) worst = std::max(worst, std::abs(d[static_cast<size_t>(i)]));
    return worst;
}

// The impulse response's transform at `freq` (Hz).
std::complex<double> transform(const Samples& h, double freq, double rate, bool timesN = false) {
    const double w = 2.0 * kPi * freq / rate;
    std::complex<double> sum = 0.0;
    for (size_t n = 0; n < h.size(); ++n) {
        const double weight = timesN ? static_cast<double>(n) : 1.0;
        sum += weight * static_cast<double>(h[n]) * std::polar(1.0, -w * static_cast<double>(n));
    }
    return sum;
}

// The group delay an impulse response has at `freq`, in samples: Re(DTFT(n h) / DTFT(h)).
double measuredDelay(const Samples& h, double freq, double rate) {
    return std::real(transform(h, freq, rate, true) / transform(h, freq, rate));
}

// The design's group delay, in samples.
double designDelay(int stages, double freq, double pinch, double rate, double at) {
    return disperser::groupDelayMs(stages, freq, pinch, rate, at) * rate / 1000.0;
}

double energy(const Samples& x) {
    double sum = 0.0;
    for (const float v : x) sum += static_cast<double>(v) * v;
    return sum;
}

// Whether, at every frame, what came out so far holds no more energy than what went in.
bool neverMoreThanGiven(const Samples& in, const Samples& out) {
    double given = 0.0, put = 0.0;
    for (size_t i = 0; i < in.size(); ++i) {
        given += static_cast<double>(in[i]) * in[i];
        put += static_cast<double>(out[i]) * out[i];
        if (put > given * (1.0 + 1e-6) + 1e-9) return false;
    }
    return true;
}

size_t powerOfTwoAtLeast(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

int tailOf(Disperser& d) { return d.processor().tailSamples(); }

struct Setting {
    int stages;
    float freq;
    float pinch;
    double rate = kSampleRate;
};

std::string describe(const Setting& s) {
    return std::to_string(s.stages) + " stages, " + std::to_string(s.freq) + " Hz, Q " + std::to_string(s.pinch) +
           " at " + std::to_string(s.rate);
}

Disperser deviceFor(const Setting& s) {
    return Disperser(s.rate, {{"amount", static_cast<float>(s.stages)}, {"freq", s.freq}, {"pinch", s.pinch}});
}

// Its impulse response, `length` long (at least twice its tail by default).
Samples impulseResponse(const Setting& s, size_t length = 0) {
    Disperser d = deviceFor(s);
    if (length == 0) length = powerOfTwoAtLeast(2 * static_cast<size_t>(tailOf(d)) + 1024);
    return d.play(impulse(length));
}

// A click at frame `at` of a track's left and/or right channel.
uint32_t clickTrack(sub::Engine& engine, float left, float right, int64_t at, double seconds = 2.0) {
    return stereoClickTrack(engine, left, right, at, seconds);
}

}  // namespace

TEST_CASE("the disperser is listed with its parameters") {
    const sub::BuiltinInfo info = builtinInfo("disperser");
    CHECK_EQ(info.name, std::string("Disperser"));
    CHECK(!info.isInstrument());
    CHECK(paramIds(info.params) == (std::vector<std::string>{"amount", "freq", "pinch", "mix", "bypass"}));
    const sub::ParamInfo& amount = info.params[0];
    CHECK_EQ(amount.minValue, 0.f);
    CHECK_EQ(amount.maxValue, 64.f);
    CHECK_EQ(amount.stepCount(), 64);  // whole stages, automation too
    CHECK_EQ(amount.fromNormalized(amount.toNormalized(33.f)), 33.f);
    const sub::ParamInfo& freq = info.params[1];
    CHECK(freq.isLog());
    CHECK_EQ(freq.minValue, 20.f);
    CHECK_EQ(freq.maxValue, 20000.f);
    CHECK_EQ(freq.unit, std::string("Hz"));
    const sub::ParamInfo& pinch = info.params[2];
    CHECK(pinch.isLog());
    CHECK_APPROX(pinch.minValue, 0.1);
    CHECK_EQ(pinch.maxValue, 10.f);
    const sub::ParamInfo& mix = info.params[3];
    CHECK_EQ(mix.name, std::string("Dry/Wet"));
    CHECK_EQ(mix.unit, std::string("%"));
    CHECK_EQ(mix.minValue, 0.f);
    CHECK_EQ(mix.maxValue, 100.f);
    CHECK_EQ(mix.defaultValue, 100.f);
    const sub::ParamInfo& bypass = info.params[4];
    CHECK(bypass.valueLabels == (std::vector<std::string>{"Off", "On"}));
    CHECK_EQ(bypass.defaultValue, 0.f);
    for (const sub::ParamInfo& p : info.params) CHECK(p.automatable);

    // The engine makes it, and it reports no latency: the dispersion is the effect.
    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "disperser", -1);
    setParam(engine, id, "amount", 64.f);
    const sub::ProcessorInfo processor = engine.processorInfo(id);
    CHECK_EQ(processor.name, std::string("Disperser"));
    CHECK_EQ(processor.latency, 0);
    CHECK(processor.tail > 0);
}

TEST_CASE("no stages, bypassed or fully dry, it passes the input through untouched") {
    const Samples left = noise(kSampleRate, 1), right = noise(kSampleRate, 2);
    for (const Values& values : {Values{{"amount", 0.f}}, Values{{"amount", 64.f}, {"bypass", 1.f}},
                                 Values{{"amount", 64.f}, {"mix", 0.f}}}) {
        INFO(values[0].first + " " + std::to_string(values.back().second));
        Disperser d(kSampleRate, values);
        Samples l = left, r = right;
        d.run({&l, &r});
        CHECK_ARRAY_EQUAL(l, left);
        CHECK_ARRAY_EQUAL(r, right);
    }
}

TEST_CASE("every frequency keeps its level") {
    for (const Setting& s : {Setting{1, 1000.f, 1.f}, Setting{16, 200.f, 3.f}, Setting{64, 2000.f, 10.f},
                             Setting{64, 100.f, 0.1f}, Setting{64, 20000.f, 1.f, 44100.0},
                             Setting{32, 50.f, 10.f, 96000.0}, Setting{64, 20.f, 0.5f, 192000.0}}) {
        INFO(describe(s));
        const Samples h = impulseResponse(s);
        CHECK_APPROX_TOL(energy(h), 1.0, 1e-4, 0.0);  // all its energy comes out (Parseval: |H| = 1 on average)
        const std::vector<double> magnitude = spectrum(h);  // every bin, 0 Hz to Nyquist
        double worst = 0.0;
        for (const double m : magnitude) worst = std::max(worst, std::abs(20.0 * std::log10(m)));
        INFO("off by " + std::to_string(worst) + " dB at most");
        CHECK(worst < 0.01);
    }
    // The longest it gets: 64 narrow stages at 20 Hz delay 20 Hz by 20 s.
    const Setting longest{64, 20.f, 10.f};
    const Samples h = impulseResponse(longest, static_cast<size_t>(30 * kSampleRate));
    CHECK_APPROX_TOL(energy(h), 1.0, 1e-3, 0.0);
    for (const double freq : {20.0, 21.0, 100.0, 5000.0}) {
        INFO(std::to_string(freq));
        CHECK(std::abs(20.0 * std::log10(std::abs(transform(h, freq, kSampleRate)))) < 0.01);
    }
}

TEST_CASE("its group delay is the curve its editor draws") {
    for (const Setting& s : {Setting{1, 1000.f, 1.f}, Setting{16, 200.f, 3.f}, Setting{64, 2000.f, 10.f},
                             Setting{48, 100.f, 0.1f}, Setting{8, 15000.f, 2.f, 44100.0},
                             Setting{32, 300.f, 5.f, 96000.0}}) {
        INFO(describe(s));
        const Samples h = impulseResponse(s);
        for (const double ratio : {0.25, 0.5, 0.9, 1.0, 1.1, 2.0, 4.0}) {
            const double at = s.freq * ratio;
            if (at >= 0.5 * s.rate) continue;
            INFO("at " + std::to_string(at) + " Hz");
            const double expected = designDelay(s.stages, s.freq, s.pinch, s.rate, at);
            CHECK_APPROX_TOL(measuredDelay(h, at, s.rate), expected, 1e-3, 0.05);
        }
    }
    // Its peak: about 4 Q / w0 per stage (2 Q / (pi f) seconds), where it is tuned.
    CHECK_APPROX_REL(disperser::groupDelayMs(64, 200.0, 5.0, 48000.0, 200.0), 64 * 2 * 5.0 / (kPi * 200.0) * 1000.0,
                     0.01);
    // More pinch: a taller, narrower peak; more stages: all of it in proportion.
    CHECK(disperser::groupDelayMs(16, 1000.0, 10.0, 48000.0, 1000.0) >
          disperser::groupDelayMs(16, 1000.0, 1.0, 48000.0, 1000.0));
    CHECK(disperser::groupDelayMs(16, 1000.0, 10.0, 48000.0, 2000.0) <
          disperser::groupDelayMs(16, 1000.0, 1.0, 48000.0, 2000.0));
    CHECK_APPROX(disperser::groupDelayMs(32, 700.0, 2.0, 48000.0, 500.0),
                 2.0 * disperser::groupDelayMs(16, 700.0, 2.0, 48000.0, 500.0));

    // Kept below Nyquist: at 22.05 kHz, 20 kHz is tuned to 0.45 of the rate, and plays as drawn there.
    CHECK_APPROX(disperser::stageFrequency(20000.0, 22050.0), 9922.5);
    CHECK_APPROX(disperser::stageFrequency(20000.0, 48000.0), 20000.0);
    const Setting limited{8, 20000.f, 2.f, 22050.0};
    const Samples h = impulseResponse(limited);
    for (const double at : {2000.0, 9000.0, 9922.5, 10500.0}) {
        INFO(std::to_string(at));
        CHECK_APPROX_TOL(measuredDelay(h, at, limited.rate), designDelay(8, 20000.0, 2.0, 22050.0, at), 1e-3, 0.05);
    }
    CHECK_APPROX(disperser::groupDelayMs(8, 20000.0, 2.0, 22050.0, 9922.5),
                 disperser::groupDelayMs(8, 9922.5, 2.0, 22050.0, 9922.5));
}

TEST_CASE("it stays stable at the extremes and at any sample rate") {
    for (const double rate : {8000.0, 22050.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
        for (const int stages : {1, 64}) {
            for (const float freq : {20.f, 20000.f}) {
                for (const float pinch : {0.1f, 10.f}) {
                    const Setting s{stages, freq, pinch, rate};
                    INFO(describe(s));
                    Disperser d = deviceFor(s);
                    Samples in = noise(static_cast<size_t>(rate), 7);
                    in.resize(in.size() + static_cast<size_t>(rate / 2), 0.f);  // then half a second of silence
                    Samples l = in, r = in;
                    d.run({&l, &r});
                    CHECK(allFinite(l) && allFinite(r));
                    CHECK(neverMoreThanGiven(in, l));
                    CHECK_ARRAY_EQUAL(r, l);  // the same in, the same out, on either side
                }
            }
        }
    }
}

TEST_CASE("however Frequency and Pinch move, it never puts out more energy than it was given") {
    // Jumps anywhere in their ranges, every few milliseconds to a fraction of a
    // second: the lattice's stages hold the energy they were given however
    // their coefficients move (a state-variable filter would throw out up to Q
    // times what it holds as its pinch fell).
    for (const double rate : {44100.0, 96000.0}) {
        INFO(std::to_string(rate));
        std::mt19937 random(11);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        std::vector<Change> changes;
        for (int64_t at = 0; at < static_cast<int64_t>(3 * rate);) {
            at += static_cast<int64_t>(rate * (0.002 + 0.3 * unit(random)));
            changes.push_back({at, "freq", static_cast<float>(20.0 * std::pow(1000.0, unit(random)))});
            changes.push_back({at, "pinch", static_cast<float>(0.1 * std::pow(100.0, unit(random)))});
        }
        for (const Samples& in : {noise(static_cast<size_t>(3 * rate), 3), smoothSine(330.0, 3.0, rate)}) {
            Disperser d(rate, {{"amount", 64.f}, {"pinch", 10.f}, {"freq", 100.f}});
            const Samples out = d.play(in, changes);
            CHECK(allFinite(out));
            CHECK(neverMoreThanGiven(in, out));
        }
    }
    // Pinch falling from 10 to 0.1 on a tone it is tuned to: what the stages hold
    // comes out as the delay shrinks, a swell no bigger than they had taken in.
    const Samples tone = smoothSine(2000.0, 2.0);
    Disperser d(kSampleRate, {{"amount", 64.f}, {"freq", 2000.f}, {"pinch", 10.f}});
    const Samples out = d.play(tone, {{kSampleRate, "pinch", 0.1f}});
    CHECK(neverMoreThanGiven(tone, out));
    CHECK(maxAbs(out) < 2.5);  // (a state-variable filter's peak here: about 8)
}

TEST_CASE("Dry/Wet blends the input with its dispersed copy, in time") {
    // The dry input isn't delayed (there is no latency), so the blend is exactly
    // the input's share plus the stages' output's share, sample for sample.
    const Setting s{24, 700.f, 3.f};
    const Samples wet = impulseResponse(s, 1 << 15);
    for (const float mix : {50.f, 30.f}) {
        INFO(std::to_string(mix) + " %");
        Disperser d(kSampleRate, {{"amount", 24.f}, {"freq", 700.f}, {"pinch", 3.f}, {"mix", mix}});
        const Samples blend = d.play(impulse(wet.size()));
        Samples want(wet.size());
        const double share = mix / 100.0;
        for (size_t i = 0; i < want.size(); ++i) want[i] = static_cast<float>(share * wet[i] + (i == 0 ? 1.0 - share : 0.0));
        CHECK_ALLCLOSE(blend, want, 0.0, 1e-7);
        // So it sounds as a phaser does: |(1 - mix) + mix H| at each frequency, notched where the
        // stages turn the phase half a circle (and, at an even blend, only there).
        const disperser::Stage stage = disperser::design(700.0, 3.0, kSampleRate);
        for (const double freq : {100.0, 500.0, 680.0, 700.0, 720.0, 1000.0, 5000.0}) {
            INFO(std::to_string(freq) + " Hz");
            const std::complex<double> h = std::pow(disperser::response(stage, 2.0 * kPi * freq / kSampleRate), 24);
            CHECK_APPROX_TOL(std::abs(transform(blend, freq, kSampleRate)), std::abs((1.0 - share) + share * h), 1e-4,
                             1e-5);
        }
    }
    // At an even blend the notches go all the way down; fully wet the level is flat.
    Disperser even(kSampleRate, {{"amount", 1.f}, {"freq", 1000.f}, {"pinch", 1.f}, {"mix", 50.f}});
    const Samples h = even.play(impulse(1 << 14));
    CHECK(std::abs(transform(h, 1000.0, kSampleRate)) < 1e-4);  // one stage turns 1 kHz half a circle
    CHECK_APPROX_TOL(std::abs(transform(h, 50.0, kSampleRate)), 1.0, 0.01, 0.0);
}

TEST_CASE("each channel has its own state") {
    const std::vector<Change> changes = {{9000, "amount", 40.f}, {20000, "freq", 300.f}, {30000, "pinch", 4.f},
                                         {40000, "bypass", 1.f}, {41000, "bypass", 0.f}};
    // Silence on one side stays silence, whatever the other side does.
    {
        Disperser d(kSampleRate, {{"amount", 16.f}});
        Samples l = noise(kSampleRate, 4), r(kSampleRate, 0.f);
        d.run({&l, &r}, changes);
        CHECK(allEqual(r, 0.0));
        CHECK(anyNonzero(l));
        Samples l2(kSampleRate, 0.f), r2 = noise(kSampleRate, 4);
        Disperser d2(kSampleRate, {{"amount", 16.f}});
        d2.run({&l2, &r2}, changes);
        CHECK(allEqual(l2, 0.0));
        CHECK_ARRAY_EQUAL(r2, l);  // and either side plays as the other would
    }
    // Two different signals: each comes out as it would on its own.
    const Samples a = noise(kSampleRate, 5), b = smoothSine(440.0, 1.0);
    Disperser stereo(kSampleRate, {{"amount", 16.f}});
    Samples l = a, r = b;
    stereo.run({&l, &r}, changes);
    Disperser alone(kSampleRate, {{"amount", 16.f}}), alone2(kSampleRate, {{"amount", 16.f}});
    CHECK_ALLCLOSE(l, alone.play(a, changes), 0.0, 1e-7);
    CHECK_ALLCLOSE(r, alone2.play(b, changes), 0.0, 1e-7);
}

TEST_CASE("a change of Amount lands on the new number of stages") {
    const Samples in = noise(kSampleRate, 6);
    const int64_t at = kSampleRate / 2, fade = kSampleRate / 50;  // the change, and its 20 ms fade
    // Fewer: once faded, exactly the output of the stages kept (they never stopped).
    {
        Disperser d(kSampleRate, {{"amount", 64.f}});
        const Samples out = d.play(in, {{at, "amount", 16.f}});
        Disperser sixteen(kSampleRate, {{"amount", 16.f}});
        const Samples want = sixteen.play(in);
        CHECK_ARRAY_EQUAL(slice(out, at + fade), slice(want, at + fade));
        Disperser sixtyFour(kSampleRate, {{"amount", 64.f}});
        CHECK_ARRAY_EQUAL(slice(out, 0, at), slice(sixtyFour.play(in), 0, at));  // and before it, the 64
    }
    // More: the stages added start from silence, so once they have rung in it is
    // the output of the new number all along.
    {
        Disperser d(kSampleRate, {{"amount", 16.f}});
        const Samples out = d.play(in, {{at, "amount", 64.f}});
        Disperser sixtyFour(kSampleRate, {{"amount", 64.f}});
        const Samples want = sixtyFour.play(in);
        CHECK_ALLCLOSE(slice(out, at + kSampleRate / 10), slice(want, at + kSampleRate / 10), 0.0, 1e-5);
    }
    // A change during a fade waits for it: two changes 5 ms apart end on the second.
    {
        Disperser d(kSampleRate, {{"amount", 64.f}});
        const Samples out = d.play(in, {{at, "amount", 32.f}, {at + 240, "amount", 8.f}});
        Disperser eight(kSampleRate, {{"amount", 8.f}});
        CHECK_ARRAY_EQUAL(slice(out, at + 2 * fade), slice(eight.play(in), at + 2 * fade));
    }
}

TEST_CASE("changing any control is click-free") {
    // A 220 Hz tone through 16 stages tuned to 1 kHz, every control jumping in
    // turn, as automation's steps make them (and some at once, and one during
    // another's fade).
    const Samples tone = smoothSine(220.0, 3.0);
    const auto s = [](double seconds) { return static_cast<int64_t>(seconds * kSampleRate); };
    const std::vector<Change> changes = {
        {s(0.4), "amount", 64.f},  {s(0.6), "amount", 3.f},   {s(0.8), "amount", 40.f},
        {s(1.0), "amount", 0.f},   {s(1.2), "amount", 24.f},  {s(1.4), "freq", 300.f},
        {s(1.6), "freq", 3000.f},  {s(1.8), "pinch", 4.f},    {s(2.0), "pinch", 0.3f},
        {s(2.2), "bypass", 1.f},   {s(2.4), "bypass", 0.f},   {s(2.41), "amount", 50.f},
        {s(2.6), "freq", 600.f},   {s(2.6), "amount", 8.f},   {s(2.6), "pinch", 2.f},
        {s(2.8), "bypass", 1.f},   {s(2.81), "bypass", 0.f},  {s(2.85), "mix", 30.f},
        {s(2.9), "mix", 0.f},      {s(2.91), "mix", 100.f},   {s(2.95), "mix", 60.f},
    };
    Disperser d(kSampleRate, {{"amount", 16.f}});
    const Samples out = d.play(tone, changes);
    CHECK(allFinite(out));
    const double input = clickiness(tone, s(0.2));
    const double output = clickiness(out, s(0.2));
    INFO("tone " + std::to_string(input) + ", through the changes " + std::to_string(output));
    CHECK(output < 1e-4);

    // What the measure makes of a click: the tone switched from 16 stages' output
    // to the dry tone at once, unfaded.
    Disperser sixteen(kSampleRate, {{"amount", 16.f}});
    Samples spliced = sixteen.play(tone);
    std::copy(tone.begin() + s(1.5), tone.end(), spliced.begin() + s(1.5));
    CHECK(clickiness(spliced, s(0.2)) > 1000 * output);
}

TEST_CASE("its automation plays through the engine sample-accurately") {
    sub::Engine engine;
    engine.setClipFadeMs(0);
    // (221.25 Hz: at its peak where the automation steps, so the first sample changed shows.)
    const Samples tone = smoothSine(221.25, 3.0);
    const std::string path = makeWav(stereo(tone), 2);
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, 0.0, 3.0, 0.0, 1.f)});
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "disperser", -1);
    setParam(engine, id, "amount", 0.f);
    const Samples untouched = engine.renderOffline(0.0, 3 * kSampleRate);  // no stages: the track as it is

    // Amount: none until beat 2 (1 s), then all 64; Bypass on from beat 4 (2 s).
    using Points = std::vector<sub::AutomationPoint>;
    CHECK_EQ(paramInfo(engine, id, "amount").fromNormalized(1.f), 64.f);
    engine.setTrackAutomation(track, {{id, "amount", Points{{0.0, 0.f, 0.f}, {2.0, 0.f, 0.f}, {2.0, 1.f, 0.f}}},
                                      {id, "bypass", Points{{0.0, 0.f, 0.f}, {4.0, 0.f, 0.f}, {4.0, 1.f, 0.f}}}});
    const Samples out = engine.renderOffline(0.0, 3 * kSampleRate);
    CHECK(allFinite(out));
    // Untouched up to the step, to the sample; then the stages fade in.
    int64_t first = -1;
    for (size_t i = 0; i < out.size() && first < 0; ++i)
        if (out[i] != untouched[i]) first = static_cast<int64_t>(i) / 2;
    CHECK_EQ(first, int64_t{kSampleRate});
    CHECK(!allclose(frames(out, kSampleRate + 2000, 2 * kSampleRate),
                    frames(untouched, kSampleRate + 2000, 2 * kSampleRate), 0.0, 0.01));
    // Bypassed: gliding to the input, then (once there, within 0.15 s) untouched again.
    const int64_t glide = kSampleRate / 100;
    CHECK(!allclose(frames(out, 2 * kSampleRate, 2 * kSampleRate + glide),
                    frames(untouched, 2 * kSampleRate, 2 * kSampleRate + glide), 0.0, 1e-4));
    const int64_t there = 2 * kSampleRate + kSampleRate * 3 / 20;
    CHECK_ARRAY_EQUAL(frames(out, there), frames(untouched, there));
    // And no click where they change: no more than the WAV's own 16-bit steps make.
    for (int c = 0; c < 2; ++c) {
        INFO(std::to_string(c));
        CHECK(clickiness(channel(out, c), kSampleRate / 5) < 2.0 * clickiness(channel(untouched, c), kSampleRate / 5));
    }
}

TEST_CASE("reset and a new sample rate start it from silence") {
    const Setting s{32, 500.f, 4.f, 44100.0};
    Disperser fresh = deviceFor(s);
    const Samples want = fresh.play(impulse(8192));
    Disperser d = deviceFor(s);
    d.play(noise(44100, 8));
    d.processor().reset();
    CHECK_ARRAY_EQUAL(d.play(impulse(8192)), want);

    // A new rate: retuned to it, from silence.
    d.play(noise(44100, 9));
    d.processor().prepare(96000.0, kBlock);
    Disperser at96 = deviceFor({32, 500.f, 4.f, 96000.0});
    const Samples h = d.play(impulse(32768));
    CHECK_ARRAY_EQUAL(h, at96.play(impulse(32768)));
    CHECK_APPROX_TOL(measuredDelay(h, 500.0, 96000.0), designDelay(32, 500.0, 4.0, 96000.0, 500.0), 1e-3, 0.05);
    CHECK(designDelay(32, 500.0, 4.0, 96000.0, 500.0) > 2.0 * designDelay(32, 500.0, 4.0, 44100.0, 500.0));
}

TEST_CASE("silence rings out to exact zeros") {
    // After a sound, its states are flushed once they die away: the output is
    // exact zeros, never denormals.
    Disperser d(kSampleRate, {{"amount", 64.f}, {"freq", 100.f}, {"pinch", 3.f}});
    Samples x = noise(kSampleRate / 2, 10);
    x.resize(static_cast<size_t>(6 * kSampleRate), 0.f);
    const Samples out = d.play(x);
    const std::vector<int64_t> sounding = nonzero(out);
    REQUIRE(!sounding.empty());
    CHECK(sounding.back() < 5 * kSampleRate);
    for (const float v : out) CHECK(v == 0.f || std::abs(v) >= std::numeric_limits<float>::min());

    // Denormal input passes as finite numbers, and dies away to zeros too.
    Samples tiny(static_cast<size_t>(2 * kSampleRate), 0.f);
    for (size_t i = 0; i < 1000; ++i) tiny[i] = (i % 2 ? 1e-40f : -1e-41f);
    const Samples quiet = d.play(tiny);
    CHECK(allFinite(quiet));
    CHECK(allEqual(slice(quiet, kSampleRate), 0.0));
}

TEST_CASE("its tail covers its ringing") {
    for (const Setting& s : {Setting{1, 1000.f, 1.f}, Setting{16, 200.f, 3.f}, Setting{64, 2000.f, 10.f},
                             Setting{64, 100.f, 0.1f}, Setting{64, 20000.f, 10.f, 44100.0},
                             Setting{8, 50.f, 0.5f, 192000.0}}) {
        INFO(describe(s));
        Disperser d = deviceFor(s);
        const auto tail = static_cast<size_t>(tailOf(d));
        const Samples h = d.play(impulse(powerOfTwoAtLeast(4 * tail + 4096)));
        CHECK(energy(slice(h, static_cast<int64_t>(tail))) < 1e-6 * energy(h));  // 60 dB down
        CHECK(tail < h.size() / 2);
    }
    Disperser none(kSampleRate, {{"amount", 0.f}}), bypassed(kSampleRate, {{"bypass", 1.f}});
    CHECK_EQ(tailOf(none), 0);
    CHECK_EQ(tailOf(bypassed), 0);
    Disperser dry(kSampleRate, {{"mix", 0.f}});
    CHECK_EQ(tailOf(dry), 0);
    // At most a minute, however long its delay.
    Disperser longest(kSampleRate, {{"amount", 64.f}, {"freq", 20.f}, {"pinch", 10.f}});
    CHECK(tailOf(longest) > 20 * kSampleRate);
    CHECK(tailOf(longest) <= 60 * kSampleRate);
}

TEST_CASE("the engine keeps its dispersion: nothing is delayed to line up with it") {
    sub::Engine engine;
    engine.setClipFadeMs(0);
    // A click on the left of a dry track, and on the right of one through the Disperser.
    constexpr int64_t kClick = 1000;
    clickTrack(engine, 1.f, 0.f, kClick);
    const uint32_t wet = clickTrack(engine, 0.f, 1.f, kClick);
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(wet), "disperser", -1);
    setParam(engine, id, "amount", 32.f);
    setParam(engine, id, "freq", 200.f);
    setParam(engine, id, "pinch", 2.f);
    CHECK_EQ(engine.processorInfo(id).latency, 0);
    const Samples out = engine.renderOffline(0.0, kSampleRate);
    const Samples left = channel(out, 0), right = channel(out, 1);

    // The dry click where it was: nothing delays it to line up with the other track.
    CHECK_EQ(nonzero(left), (std::vector<int64_t>{kClick}));
    // The dispersed one starts at the click too, as the device's impulse response.
    CHECK(allEqual(slice(right, 0, kClick), 0.0));
    const Samples response = impulseResponse({32, 200.f, 2.f}, static_cast<size_t>(kSampleRate - kClick));
    Samples scaled = response;
    for (float& v : scaled) v *= left[static_cast<size_t>(kClick)];  // (the WAV's 16-bit full scale)
    CHECK_ALLCLOSE(slice(right, kClick), scaled, 1e-5, 1e-7);
    // Its 200 Hz comes the stages' group delay late (about 0.2 s).
    const double delay = measuredDelay(slice(right, kClick), 200.0, kSampleRate);
    CHECK_APPROX_TOL(delay, designDelay(32, 200.0, 2.0, kSampleRate, 200.0), 1e-3, 0.5);
    CHECK(delay > 0.15 * kSampleRate);
}
