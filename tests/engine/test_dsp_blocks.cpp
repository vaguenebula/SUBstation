// The building blocks the built-in effects share (builtin/DspBlocks.h): each
// one measured against what it is meant to do.

#include <cmath>
#include <string>
#include <vector>

#include "builtin/DspBlocks.h"
#include "harness/Signal.h"
#include "harness/Test.h"

using namespace subtest;
namespace dsp = sub::dsp;

namespace {

constexpr double kRate = 48000.0;

// The level of a steady sine through `process`, in dB, after it has settled.
template <typename Process>
double sineGainDb(double freq, Process&& process, int frames = 48000) {
    double inEnergy = 0.0, outEnergy = 0.0;
    for (int i = 0; i < frames; ++i) {
        const auto x = static_cast<float>(std::sin(2 * kPi * freq * i / kRate));
        const float y = process(x);
        if (i >= frames / 2) {
            inEnergy += double(x) * x;
            outEnergy += double(y) * y;
        }
    }
    return 10 * std::log10(outEnergy / inEnergy);
}

}  // namespace

TEST_CASE("dsp blocks: a one-pole smooths and a DC blocker removes DC") {
    dsp::OnePole pole;
    const float coeff = dsp::onePoleCutoff(1000.0, kRate);
    for (int i = 0; i < 4800; ++i) pole.lowpass(1.f, coeff);
    CHECK_NEAR(pole.z, 1.0, 1e-6);
    // -3 dB at the cutoff, near enough far below Nyquist.
    pole.reset();
    CHECK_NEAR(sineGainDb(1000.0, [&](float x) { return pole.lowpass(x, coeff); }), -3.0, 0.3);

    dsp::DcBlocker dc;
    dc.prepare(kRate);
    float y = 1.f;
    for (int i = 0; i < 48000; ++i) y = dc.process(0.5f);
    CHECK(std::abs(y) < 1e-3);
    dc.reset();
    CHECK_NEAR(sineGainDb(1000.0, [&](float x) { return dc.process(x); }), 0.0, 0.01);
}

TEST_CASE("dsp blocks: a delay line reads what was pushed, between samples too") {
    dsp::DelayLine line;
    line.prepare(100);
    CHECK(line.capacity() >= 104);
    for (int i = 0; i < 200; ++i) line.push(float(i));
    CHECK_EQ(line.tap(0), 199.f);
    CHECK_EQ(line.tap(10), 189.f);
    CHECK_NEAR(line.linear(10.25f), 188.75, 1e-4);
    CHECK_NEAR(line.hermite(10.25f), 188.75, 1e-4);  // a ramp: exact either way
    // Delays past the ring clamp instead of reading garbage.
    CHECK(std::isfinite(line.linear(1e9f)));
    CHECK(std::isfinite(line.hermite(-5.f)));
    line.reset();
    CHECK_EQ(line.tap(3), 0.f);
}

TEST_CASE("dsp blocks: an envelope follower rises and falls with its times") {
    dsp::EnvelopeFollower env;
    env.setTimes(10.f, 100.f, kRate);
    // After one attack time a step has come 1 - 1/e of the way.
    for (int i = 0; i < 480; ++i) env.peak(1.f);
    CHECK_NEAR(env.value(), 1.0 - std::exp(-1.0), 0.01);
    for (int i = 0; i < 48000; ++i) env.peak(1.f);
    for (int i = 0; i < 4800; ++i) env.peak(0.f);
    CHECK_NEAR(env.value(), std::exp(-1.0), 0.01);
    // RMS of a full-scale sine: 1/sqrt(2).
    env.reset();
    env.setTimes(50.f, 50.f, kRate);
    float rms = 0.f;
    for (int i = 0; i < 48000; ++i) rms = env.rms(static_cast<float>(std::sin(2 * kPi * 1000.0 * i / kRate)));
    CHECK_NEAR(rms, std::sqrt(0.5), 0.01);
}

TEST_CASE("dsp blocks: noise is uniform, repeatable, and fast tanh is tanh") {
    dsp::Noise a, b;
    a.seed(7);
    b.seed(7);
    double sum = 0.0, square = 0.0;
    for (int i = 0; i < 100000; ++i) {
        const float x = a.next();
        CHECK_EQ(x, b.next());
        sum += x;
        square += double(x) * x;
    }
    CHECK_NEAR(sum / 100000, 0.0, 0.01);
    CHECK_NEAR(square / 100000, 1.0 / 3.0, 0.01);  // uniform in -1..1

    double worst = 0.0;
    for (float x = -8.f; x <= 8.f; x += 0.001f) worst = std::max(worst, std::abs(double(dsp::fastTanh(x)) - std::tanh(x)));
    CHECK(worst < 2e-4);
    CHECK_EQ(dsp::fastTanh(100.f), 1.f);
    CHECK_EQ(dsp::fastTanh(-100.f), -1.f);
}

TEST_CASE("dsp blocks: the LFO's shapes and its cycles") {
    using dsp::Lfo;
    using dsp::LfoShape;
    CHECK_NEAR(Lfo::shape(LfoShape::Sine, 0.25, 0), 1.0, 1e-6);
    CHECK_NEAR(Lfo::shape(LfoShape::Triangle, 0.0, 0), 0.0, 1e-6);
    CHECK_NEAR(Lfo::shape(LfoShape::Triangle, 0.25, 0), 1.0, 1e-6);
    CHECK_NEAR(Lfo::shape(LfoShape::Triangle, 0.75, 0), -1.0, 1e-6);
    CHECK_NEAR(Lfo::shape(LfoShape::SawUp, 0.0, 0), -1.0, 1e-6);
    CHECK_NEAR(Lfo::shape(LfoShape::SawDown, 0.0, 0), 1.0, 1e-6);
    CHECK_EQ(Lfo::shape(LfoShape::Square, 0.49, 0), 1.f);
    CHECK_EQ(Lfo::shape(LfoShape::Square, 0.51, 0), -1.f);
    // Random holds a value per cycle; RandomSmooth glides from it to the next's.
    CHECK_EQ(Lfo::shape(LfoShape::Random, 0.1, 5), Lfo::shape(LfoShape::Random, 0.9, 5));
    CHECK(Lfo::shape(LfoShape::Random, 0.1, 5) != Lfo::shape(LfoShape::Random, 0.1, 6));
    CHECK_NEAR(Lfo::shape(LfoShape::RandomSmooth, 0.0, 5), Lfo::shape(LfoShape::Random, 0.0, 5), 1e-6);
    CHECK_NEAR(Lfo::shape(LfoShape::RandomSmooth, 1.0, 5), Lfo::shape(LfoShape::Random, 0.0, 6), 1e-6);

    Lfo lfo;
    lfo.advance(2.75);
    CHECK_NEAR(lfo.phase(), 0.75, 1e-12);
    CHECK_EQ(lfo.cycle(), 2u);
    CHECK_NEAR(lfo.value(LfoShape::Sine, 0.5), 1.0, 1e-6);  // 0.25 of the next cycle
    CHECK_EQ(lfo.value(LfoShape::Random, 0.5), Lfo::shape(LfoShape::Random, 0.25, 3));
    CHECK_EQ(lfo.value(LfoShape::Random, -1.0), Lfo::shape(LfoShape::Random, 0.75, 1));

    CHECK_EQ(dsp::syncedDivisionLabels().size(), size_t{19});
    CHECK_EQ(dsp::syncedCycleBeats(10), 1.0);   // 1/4
    CHECK_EQ(dsp::syncedCycleBeats(15), 4.0);   // 1 Bar
    CHECK_NEAR(dsp::syncedCycleBeats(9), 2.0 / 3.0, 1e-12);  // 1/4T
    CHECK_EQ(dsp::syncedCycleBeats(11), 1.5);   // 1/4D
}

TEST_CASE("dsp blocks: biquads respond as their magnitude says") {
    using C = dsp::BiquadCoefficients;
    struct Case {
        const char* name;
        C coeffs;
        double freq;
        double expectDb;
    };
    const Case cases[] = {
        {"lowpass at its corner", C::lowpass(1000, 0.7071, kRate), 1000, -3.01},
        {"lowpass far below", C::lowpass(1000, 0.7071, kRate), 50, 0.0},
        {"highpass at its corner", C::highpass(1000, 0.7071, kRate), 1000, -3.01},
        {"bandpass at its centre", C::bandpass(2000, 2.0, kRate), 2000, 0.0},
        {"notch at its centre", C::notch(2000, 2.0, kRate), 2000, -120.0},
        {"allpass anywhere", C::allpass(2000, 2.0, kRate), 500, 0.0},
        {"peak at its centre", C::peak(3000, 1.0, 6.0, kRate), 3000, 6.0},
        {"low shelf far below", C::lowShelf(200, 0.7071, -9.0, kRate), 10, -9.0},
        {"high shelf far above", C::highShelf(3000, 0.7071, 4.5, kRate), 20000, 4.5},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        const double drawn = c.coeffs.magnitudeDb(c.freq, kRate);
        if (c.expectDb < -100)
            CHECK(drawn < -60.0);
        else
            CHECK_NEAR(drawn, c.expectDb, 0.15);
        if (c.expectDb > -100 && c.freq >= 50) {
            dsp::Biquad state;
            const double played = sineGainDb(c.freq, [&](float x) { return state.process(c.coeffs, x); });
            CHECK_NEAR(played, drawn, 0.05);
        }
    }
}

TEST_CASE("dsp blocks: the crossover's bands add up flat, as its all-pass does") {
    const auto coeffs = dsp::CrossoverCoefficients::at(500.0, kRate);
    for (double freq : {60.0, 400.0, 500.0, 700.0, 5000.0}) {
        INFO("at " + std::to_string(freq) + " Hz");
        dsp::Crossover split;
        dsp::CrossoverAllpass allpass;
        double sumEnergy = 0.0, allpassEnergy = 0.0, diff = 0.0, lowEnergy = 0.0, highEnergy = 0.0;
        for (int i = 0; i < 48000; ++i) {
            const auto x = static_cast<float>(std::sin(2 * kPi * freq * i / kRate));
            float low, high;
            split.process(coeffs, x, low, high);
            const float ap = allpass.process(coeffs, x);
            if (i >= 24000) {
                sumEnergy += double(low + high) * (low + high);
                allpassEnergy += double(ap) * ap;
                diff += double(low + high - ap) * (low + high - ap);
                lowEnergy += double(low) * low;
                highEnergy += double(high) * high;
            }
        }
        CHECK_NEAR(10 * std::log10(sumEnergy / 12000.0), 0.0, 0.01);  // a unit sine's energy is 1/2 a sample
        CHECK(diff < 1e-6 * allpassEnergy);
        if (freq == 500.0) CHECK_NEAR(10 * std::log10(lowEnergy / highEnergy), 0.0, 0.05);  // -6 dB each
    }
}

TEST_CASE("dsp blocks: a sliding maximum over a changing window") {
    dsp::SlidingMax max;
    max.prepare(64);
    const std::vector<float> values = {1, 5, 2, 3, 0, 0, 0, 4, 1, 1};
    std::vector<float> got;
    for (float v : values) got.push_back(max.push(v, 3));
    CHECK(got == (std::vector<float>{1, 5, 5, 5, 3, 3, 0, 4, 4, 4}));
    // Against brute force, with random values and windows.
    dsp::Noise noise;
    std::vector<float> history;
    max.reset();
    for (int i = 0; i < 5000; ++i) {
        const float v = noise.next();
        const int window = 1 + static_cast<int>((noise.next() * 0.5f + 0.5f) * 63.f);
        history.push_back(v);
        float want = -2.f;
        for (int k = 0; k < window && k < static_cast<int>(history.size()); ++k)
            want = std::max(want, history[history.size() - 1 - static_cast<size_t>(k)]);
        REQUIRE(max.push(v, window) == want);
    }
}

TEST_CASE("dsp blocks: oversampling is transparent in band, delayed by its latency") {
    for (int factorLog2 = 0; factorLog2 <= 3; ++factorLog2) {
        INFO("factor log2 " + std::to_string(factorLog2));
        dsp::Oversampler os;
        os.prepare(256);
        os.setFactorLog2(factorLog2);
        CHECK_EQ(os.factor(), 1 << factorLog2);
        const int latencies[] = {0, 31, 36, 38};
        CHECK_EQ(os.latencySamples(), latencies[factorLog2]);
        const int latency = os.latencySamples();
        // A sine through up and down (in odd-sized blocks) comes out as it went in, `latency` later.
        for (double freq : {100.0, 5000.0, 18000.0}) {
            INFO("at " + std::to_string(freq) + " Hz");
            os.reset();
            std::vector<float> in(48000), out(48000);
            for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(0.5 * std::sin(2 * kPi * freq * double(i) / kRate));
            for (size_t at = 0; at < in.size();) {
                const int n = std::min<int>(197, static_cast<int>(in.size() - at));
                float* up = os.up(in.data() + at, n);
                os.down(up, n, out.data() + at);
                at += static_cast<size_t>(n);
            }
            double err = 0.0, energy = 0.0;
            for (size_t i = 1000; i < in.size(); ++i) {
                const double d = out[i] - in[i - static_cast<size_t>(latency)];
                err += d * d;
                energy += double(in[i]) * in[i];
            }
            CHECK(10 * std::log10(err / energy + 1e-30) < (freq <= 5000.0 ? -70.0 : -40.0));
        }
    }
}

TEST_CASE("dsp blocks: oversampled saturation folds back far less") {
    // A hard-clipped 7 kHz sine: at 1x its harmonics fold back everywhere; at 8x what folds back is tiny.
    auto aliasDb = [](int factorLog2) {
        dsp::Oversampler os;
        os.prepare(512);
        os.setFactorLog2(factorLog2);
        const int n = 512 * 93;
        std::vector<double> out(static_cast<size_t>(n));
        std::vector<float> block(512), result(512);
        for (int at = 0; at < n; at += 512) {
            for (int i = 0; i < 512; ++i) block[size_t(i)] = static_cast<float>(std::sin(2 * kPi * 7000.0 * (at + i) / kRate));
            float* up = os.up(block.data(), 512);
            for (int i = 0; i < 512 * os.factor(); ++i) up[i] = std::clamp(3.f * up[i], -1.f, 1.f);
            os.down(up, 512, result.data());
            for (int i = 0; i < 512; ++i) out[size_t(at + i)] = result[size_t(i)];
        }
        // Energy away from the true harmonics (7k, 21k) below 20 kHz, relative to the fundamental's.
        std::vector<double> segment(out.begin() + 16384, out.begin() + 16384 + 16384);
        const std::vector<double> window = hanning(segment.size());
        for (size_t i = 0; i < segment.size(); ++i) segment[i] *= window[i];
        const auto spectrum = dft(segment);
        const double binHz = kRate / double(segment.size());
        double fundamental = 0.0, alias = 0.0;
        for (size_t b = 1; b < segment.size() / 2; ++b) {
            const double hz = double(b) * binHz;
            const double power = std::norm(spectrum[b]);
            if (std::abs(hz - 7000.0) < 60.0)
                fundamental += power;
            else if (hz < 20000.0 && std::abs(hz - 21000.0) > 60.0)
                alias += power;
        }
        return 10 * std::log10(alias / fundamental);
    };
    const double plain = aliasDb(0);
    const double oversampled = aliasDb(3);
    CHECK(plain > -40.0);
    CHECK(oversampled < plain - 25.0);
}

TEST_CASE("dsp blocks: a low, narrow biquad rings out to exact zeros") {
    // Poles close to z = 1: flushing one state at a time would leave the pair cycling around 1e-19.
    for (const auto& coeffs : {dsp::BiquadCoefficients::peak(30.0, 12.0, 15.0, kRate),
                               dsp::BiquadCoefficients::peak(30.0, 0.1, -15.0, kRate),
                               dsp::BiquadCoefficients::lowShelf(20.0, 0.7071, 12.0, kRate)}) {
        dsp::Biquad state;
        for (int i = 0; i < 4800; ++i) state.process(coeffs, i % 100 == 0 ? 1.f : 0.f);
        int samples = 0;
        while ((state.s1 != 0.0 || state.s2 != 0.0) && samples < 60 * 48000) {
            state.process(coeffs, 0.f);
            ++samples;
        }
        CHECK(samples < 30 * 48000);
        CHECK_EQ(state.process(coeffs, 0.f), 0.f);
    }
}
