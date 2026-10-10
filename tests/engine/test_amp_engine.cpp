// The built-in Amp, after Ableton's: seven amp models through one structure
// (builtin/AmpDesign.h).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "Engine.h"
#include "builtin/AmpDesign.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/DspBlocks.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;
namespace amp = sub::amp;

namespace {

constexpr int kBlock = 1024;  // the renderer's largest block
constexpr int kLatency = 37;
constexpr int kModels = amp::kModels;

using Values = std::vector<std::pair<std::string, float>>;

struct Change {
    int64_t frame;
    std::string id;
    float value;
};

// An Amp on its own, outside an engine, at any sample rate: processed in
// blocks, its changes handed over as automation as the renderer does.
class Amp {
public:
    explicit Amp(double rate = kSampleRate, const Values& values = {}, int maxBlock = kBlock)
        : processor_(sub::BuiltinRegistry::instance().create("amp")), rate_(rate) {
        for (const auto& [id, value] : values) set(id, value);
        processor_->prepare(rate, maxBlock);
    }

    sub::Processor& processor() { return *processor_; }

    int index(const std::string& id) const {
        const auto& params = processor_->params();
        for (size_t i = 0; i < params.size(); ++i)
            if (params[i].id == id) return static_cast<int>(i);
        INFO(id);
        REQUIRE(false);
        return -1;
    }
    void set(const std::string& id, float value) { processor_->setParam(index(id), value); }

    // Processes one or two channels of equal length in place, `block` frames at a time.
    void run(const std::vector<Samples*>& channels, const std::vector<Change>& changes = {}, int block = 256) {
        const auto frames = static_cast<int64_t>(channels[0]->size());
        sub::ProcessContext ctx;
        ctx.sampleRate = rate_;
        ctx.offline = true;
        float* pointers[2] = {};
        size_t next = 0;
        for (int64_t start = 0; start < frames; start += block) {
            const int n = static_cast<int>(std::min<int64_t>(block, frames - start));
            while (next < changes.size() && changes[next].frame < start + n) {
                const Change& change = changes[next++];
                const int i = index(change.id);
                processor_->automate(i, processor_->params()[static_cast<size_t>(i)].toNormalized(change.value),
                                     static_cast<int32_t>(std::max<int64_t>(0, change.frame - start)));
            }
            for (size_t c = 0; c < channels.size(); ++c) pointers[c] = channels[c]->data() + start;
            ctx.samplePos = start;
            processor_->process(ctx, pointers, static_cast<int>(channels.size()), n);
            processor_->clearAutomation();
        }
    }
    // One channel: what comes out.
    Samples play(Samples mono, const std::vector<Change>& changes = {}, int block = 256) {
        run({&mono}, changes, block);
        return mono;
    }
    // Two channels, the same signal on both: what comes out of each.
    std::pair<Samples, Samples> playStereo(const Samples& left, const Samples& right,
                                           const std::vector<Change>& changes = {}, int block = 256) {
        Samples l = left, r = right;
        run({&l, &r}, changes, block);
        return {l, r};
    }

    // Display `index`'s values since the last read.
    std::vector<float> display(int index) {
        std::vector<float> out;
        positions_[index] = processor_->readDisplay(index, positions_[index], out);
        return out;
    }

private:
    std::shared_ptr<sub::Processor> processor_;
    double rate_;
    uint64_t positions_[7] = {};
};

enum Display { Input = 0, Drive1, Drive2, Drive3, Power, Sag, Output };

Values model(int m, const Values& more = {}) {
    Values v = {{"type", static_cast<float>(m)}};
    v.insert(v.end(), more.begin(), more.end());
    return v;
}

std::string modelName(int m) { return amp::modelLabels()[static_cast<size_t>(m)]; }

// A sine, `amplitude` peak.
Samples tone(double freq, double amplitude, double seconds, double rate = kSampleRate) {
    Samples x(static_cast<size_t>(std::lround(seconds * rate)));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate));
    return x;
}

Samples noise(size_t length, unsigned seed, float amplitude = 0.5f) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> uniform(-amplitude, amplitude);
    Samples x(length);
    for (float& v : x) v = uniform(random);
    return x;
}

double rmsDb(const Samples& x, int64_t from = 0, int64_t to = -1) {
    if (to < 0) to = static_cast<int64_t>(x.size());
    double sum = 0.0;
    for (int64_t i = from; i < to; ++i)
        sum += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
    return 10.0 * std::log10(sum / static_cast<double>(std::max<int64_t>(1, to - from)) + 1e-30);
}

// The amplitude of `freq` in the last half, by a Hann-windowed DFT (exact frequency).
double amplitudeAt(const Samples& x, double freq, double rate = kSampleRate) {
    const size_t from = x.size() / 2, n = x.size() - from;
    std::complex<double> sum = 0.0;
    double weights = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
        sum +=
            w * static_cast<double>(x[from + i]) * std::polar(1.0, -2.0 * kPi * freq * static_cast<double>(i) / rate);
        weights += w;
    }
    return 2.0 * std::abs(sum) / weights;
}

// The amplitudes of the first `n` harmonics of `freq`.
std::vector<double> harmonics(const Samples& x, double freq, int n = 8, double rate = kSampleRate) {
    std::vector<double> h;
    for (int k = 1; k <= n; ++k) h.push_back(k * freq < 0.5 * rate ? amplitudeAt(x, k * freq, rate) : 0.0);
    return h;
}

double thd(const Samples& x, double freq, double rate = kSampleRate) {
    const std::vector<double> h = harmonics(x, freq, 8, rate);
    double sum = 0.0;
    for (size_t k = 1; k < h.size(); ++k) sum += h[k] * h[k];
    return std::sqrt(sum) / h[0];
}

double db(double v) { return 20.0 * std::log10(std::max(v, 1e-30)); }

// The amplitude of `freq` over whole periods of the last half (a plain DFT), in dB.
double toneAt(const Samples& x, double freq, double rate = kSampleRate) {
    const double period = rate / freq;
    const auto periods = static_cast<int64_t>(static_cast<double>(x.size() / 2) / period);
    const auto n = static_cast<int64_t>(std::lround(static_cast<double>(periods) * period));
    const int64_t from = static_cast<int64_t>(x.size()) - n;
    std::complex<double> sum = 0.0;
    for (int64_t i = 0; i < n; ++i)
        sum += static_cast<double>(x[static_cast<size_t>(from + i)]) *
               std::polar(1.0, -2.0 * kPi * freq * static_cast<double>(i) / rate);
    return db(2.0 * std::abs(sum) / static_cast<double>(n));
}

// What lies between the harmonics (and DC) of `freq`, against everything, below
// 0.42 of the rate (the oversampler's passband), in dB: a Blackman window over
// the last three quarters.
double aliasDb(const Samples& x, double freq, double rate = kSampleRate) {
    const Samples seg = slice(x, static_cast<int64_t>(x.size() / 4));
    const size_t n = seg.size();
    std::vector<double> window(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1);
        window[i] = 0.42 - 0.5 * std::cos(t) + 0.08 * std::cos(2.0 * t);
    }
    const std::vector<double> s = spectrum(seg, window);
    double between = 0.0, total = 0.0;
    for (size_t k = 0; k < s.size(); ++k) {
        const double f = static_cast<double>(k) * rate / static_cast<double>(n);
        if (f > 0.42 * rate) break;
        const double power = s[k] * s[k];
        total += power;
        const double nearest = std::round(f / freq) * freq;
        if (std::abs(f - nearest) > 30.0) between += power;
    }
    return 10.0 * std::log10(between / total + 1e-30);
}

// The largest 6th difference over [from, to): a steep high-pass, (2 sin(ω / 2))^6:
// 64 times as sensitive at Nyquist as at a sixth of the rate (8 times, 18 dB,
// more than at a quarter). A step of d shows as up to 20 d; a smooth signal well
// below Nyquist hardly at all.
double clickiness(const Samples& x, int64_t from = 0, int64_t to = -1) {
    std::vector<double> d(x.begin(), x.end());
    for (int k = 0; k < 6; ++k)
        for (size_t i = d.size() - 1; i > 0; --i) d[i] -= d[i - 1];
    if (to < 0) to = static_cast<int64_t>(d.size());
    double worst = 0.0;
    for (int64_t i = std::max<int64_t>(from, 6); i < std::min<int64_t>(to, static_cast<int64_t>(d.size())); ++i)
        worst = std::max(worst, std::abs(d[static_cast<size_t>(i)]));
    return worst;
}

int64_t frameAt(double seconds, double rate = kSampleRate) { return static_cast<int64_t>(std::lround(seconds * rate)); }

// The seconds an amp with `values` takes over `left` and `right` (after playing `before`,
// untimed): the best of three runs, as other work on the machine only ever makes one slower.
double bestOfThree(const Values& values, const Samples& left, const Samples& right, const Samples& before) {
    using Clock = std::chrono::steady_clock;
    double best = 1e9;
    for (int run = 0; run < 3; ++run) {
        Amp a(kSampleRate, values);
        if (!before.empty()) a.playStereo(before, before);
        Samples l = left, r = right;
        const auto start = Clock::now();
        a.run({&l, &r});
        best = std::min(best, std::chrono::duration<double>(Clock::now() - start).count());
    }
    return best;
}

}  // namespace

namespace {

const Values kAllAtTen = {{"gain", 10.f},   {"bass", 10.f},     {"middle", 10.f},
                          {"treble", 10.f}, {"presence", 10.f}, {"volume", 10.f}};
const Values kAllAtZero = {{"gain", 0.f},   {"bass", 0.f},     {"middle", 0.f},
                           {"treble", 0.f}, {"presence", 0.f}, {"volume", 0.f}};
const char* const kDials[] = {"gain", "bass", "middle", "treble", "presence", "volume"};

// The -12 dBFS (peak) 220 Hz sine most tests play, `seconds` long.
Samples sine220(double seconds = 1.0) { return tone(220.0, 0.25, seconds); }

// Whether two voicings are the same, field by field.
bool sameVoicing(const amp::Voicing& a, const amp::Voicing& b) {
    bool same = a.inputHighpassHz == b.inputHighpassHz && a.brightHz == b.brightHz && a.brightDb == b.brightDb &&
                a.gainMinDb == b.gainMinDb && a.gainMaxDb == b.gainMaxDb && a.gainSplit == b.gainSplit &&
                a.tone.r1 == b.tone.r1 && a.tone.r2 == b.tone.r2 && a.tone.r3 == b.tone.r3 && a.tone.r4 == b.tone.r4 &&
                a.tone.c1 == b.tone.c1 && a.tone.c2 == b.tone.c2 && a.tone.c3 == b.tone.c3 &&
                a.toneMakeupDb == b.toneMakeupDb && a.presenceHz == b.presenceHz &&
                a.presenceRangeDb == b.presenceRangeDb && a.volumeDb == b.volumeDb && a.powerBias == b.powerBias &&
                a.sag == b.sag && a.sagAttackMs == b.sagAttackMs && a.sagReleaseMs == b.sagReleaseMs &&
                a.transformerHz == b.transformerHz && a.trimDb == b.trimDb;
    for (int k = 0; k < 3; ++k) {
        const amp::Stage& x = a.stages[k];
        const amp::Stage& y = b.stages[k];
        same = same && x.levelDb == y.levelDb && x.headDb == y.headDb && x.bias == y.bias &&
               x.lowpassHz == y.lowpassHz && x.highpassHz == y.highpassHz;
    }
    return same;
}

double meanOf(const Samples& x, int64_t from) {
    double sum = 0.0;
    for (size_t i = static_cast<size_t>(from); i < x.size(); ++i) sum += x[i];
    return sum / static_cast<double>(x.size() - static_cast<size_t>(from));
}

double meanDb(const std::vector<float>& values, size_t from) {
    double sum = 0.0;
    for (size_t i = from; i < values.size(); ++i) sum += values[i];
    return sum / static_cast<double>(values.size() - from);
}

}  // namespace

TEST_CASE("the amp is listed with its parameters and displays") {
    const sub::BuiltinInfo info = builtinInfo("amp");
    CHECK_EQ(info.name, std::string("Amp"));
    CHECK(!info.isInstrument());
    CHECK(paramIds(info.params) ==
          (std::vector<std::string>{"type", "gain", "bass", "middle", "treble", "presence", "volume", "dual", "mix"}));
    const std::vector<std::string> names = {"Amp Type", "Gain",   "Bass",      "Middle", "Treble",
                                            "Presence", "Volume", "Dual Mono", "Dry/Wet"};
    for (size_t i = 0; i < info.params.size(); ++i) {
        const sub::ParamInfo& p = info.params[i];
        INFO(p.id);
        CHECK_EQ(p.name, names[i]);
        CHECK(p.automatable);
        CHECK(!p.isLog());
        if (i >= 1 && i <= 6) {  // the dials: an amp's 0..10, reading "5.0"
            CHECK_EQ(p.unit, std::string("dial"));
            CHECK_EQ(p.minValue, 0.f);
            CHECK_EQ(p.maxValue, 10.f);
            CHECK_EQ(p.defaultValue, 5.f);
            CHECK_EQ(p.stepCount(), 0);
        }
    }
    const sub::ParamInfo& type = info.params[0];
    CHECK(type.valueLabels == amp::modelLabels());
    CHECK(type.valueLabels == (std::vector<std::string>{"Clean", "Boost", "Blues", "Rock", "Lead", "Heavy", "Bass"}));
    CHECK_EQ(type.unit, std::string(""));
    CHECK_EQ(type.minValue, 0.f);
    CHECK_EQ(type.maxValue, 6.f);
    CHECK_EQ(type.defaultValue, 0.f);
    CHECK_EQ(type.stepCount(), 6);
    const sub::ParamInfo& dual = info.params[7];
    CHECK(dual.valueLabels == (std::vector<std::string>{"Mono", "Dual"}));
    CHECK_EQ(dual.defaultValue, 0.f);
    const sub::ParamInfo& mix = info.params[8];
    CHECK_EQ(mix.unit, std::string("%"));
    CHECK_EQ(mix.minValue, 0.f);
    CHECK_EQ(mix.maxValue, 100.f);
    CHECK_EQ(mix.defaultValue, 100.f);

    // Seven displays, a value per 256 samples.
    const std::shared_ptr<sub::Processor> device = sub::BuiltinRegistry::instance().create("amp");
    const std::vector<sub::DisplayInfo> displays = device->displays();
    REQUIRE(displays.size() == 7);
    const std::vector<std::string> ids = {"input", "drive1", "drive2", "drive3", "power", "sag", "output"};
    for (size_t i = 0; i < displays.size(); ++i) {
        CHECK_EQ(displays[i].id, ids[i]);
        CHECK_EQ(displays[i].samplesPerValue, 256);
    }
    // Its latency: the oversampler's and one sample for the stages' anti-aliasing, at any rate, from the start.
    CHECK_EQ(kLatency, sub::dsp::Oversampler::latencyFor(amp::kOversamplingLog2) + 1);
    CHECK_EQ(device->latencySamples(), kLatency);
    for (const double rate : {44100.0, 96000.0}) {
        device->prepare(rate, kBlock);
        CHECK_EQ(device->latencySamples(), kLatency);
        CHECK_EQ(device->tailSamples(), kLatency + static_cast<int>(std::lround(0.5 * rate)));
        CHECK(!device->idle());
    }

    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "amp", -1);
    const sub::ProcessorInfo processor = engine.processorInfo(id);
    CHECK_EQ(processor.name, std::string("Amp"));
    CHECK_EQ(processor.latency, kLatency);
    CHECK(processor.tail > kSampleRate / 2);
}

TEST_CASE("the amp's design: tone stacks, the curve and its anti-aliasing") {
    for (int m = 0; m < kModels; ++m) {
        INFO(modelName(m));
        const amp::Voicing& v = amp::voicing(m);
        CHECK_NEAR(amp::toneMakeupDb(v.tone), v.toneMakeupDb, 0.01);
        // At noon each stack peaks at 0 dB (the make-up).
        double most = -100.0;
        for (int i = 0; i < 64; ++i)
            most = std::max(most, amp::toneResponseDb(v, 5, 5, 5, 5, kSampleRate, 40.0 * std::pow(250.0, i / 63.0)));
        CHECK_NEAR(most, 0.0, 0.05);
        // The digital stack (bilinear, 4x) plays as the analog one.
        for (const auto& [bass, middle, treble] : {std::tuple{5.0, 5.0, 5.0}, std::tuple{10.0, 0.0, 10.0},
                                                   std::tuple{0.0, 10.0, 0.0}, std::tuple{10.0, 10.0, 0.0}}) {
            for (int i = 0; i < 30; ++i) {
                const double f = 30.0 * std::pow(16000.0 / 30.0, i / 29.0);
                const double analog =
                    db(std::abs(amp::toneStackAnalog(v.tone, bass, middle, treble, f))) + v.toneMakeupDb;
                CHECK_NEAR(amp::toneResponseDb(v, bass, middle, treble, 5, kSampleRate, f), analog, 0.02);
            }
        }
    }
    // The Fender (Bassman) stack's deep scoop at noon: the regression anchors.
    const amp::Voicing& blues = amp::voicing(amp::Blues);
    CHECK_NEAR(amp::toneResponseDb(blues, 5, 5, 5, 5, kSampleRate, 100.0), -0.70, 0.05);
    CHECK_NEAR(amp::toneResponseDb(blues, 5, 5, 5, 5, kSampleRate, 700.0), -7.09, 0.05);
    CHECK_NEAR(amp::toneResponseDb(blues, 5, 5, 5, 5, kSampleRate, 6000.0), -0.42, 0.05);
    // The controls' ranges there: modest and interacting, as a real stack's.
    CHECK(amp::toneResponseDb(blues, 10, 5, 5, 5, kSampleRate, 100.0) > -0.70 + 2.0);
    CHECK(amp::toneResponseDb(blues, 5, 0, 5, 5, kSampleRate, 700.0) < -7.09 - 4.0);
    CHECK(amp::toneResponseDb(blues, 5, 5, 10, 5, kSampleRate, 3000.0) >
          amp::toneResponseDb(blues, 5, 5, 5, 5, kSampleRate, 3000.0) + 3.0);
    // Presence: flat at 5, a shelf at the ends.
    CHECK_NEAR(amp::toneResponseDb(blues, 5, 5, 5, 10, kSampleRate, 10000.0) -
                   amp::toneResponseDb(blues, 5, 5, 5, 5, kSampleRate, 10000.0),
               9.0, 1.5);
    CHECK_NEAR(amp::toneResponseDb(blues, 5, 5, 5, 0, kSampleRate, 50.0),
               amp::toneResponseDb(blues, 5, 5, 5, 5, kSampleRate, 50.0), 0.1);

    // The curve: ±1 where it flattens, its integral's derivative is itself, continuous at ±1.
    CHECK_EQ(amp::shape(1.0), 1.0);
    CHECK_EQ(amp::shape(-1.0), -1.0);
    CHECK_EQ(amp::shape(7.0), 1.0);
    CHECK_EQ(amp::shapeSlope(1.0), 0.0);
    CHECK_EQ(amp::shapeSlope(-1.0), 0.0);
    CHECK_EQ(amp::shapeSlope(0.0), 1.5);
    for (const double x : {-1.7, -1.0, -0.6, -0.1, 0.0, 0.3, 0.999, 1.0, 2.5}) {
        INFO(std::to_string(x));
        const double h = 1e-6;
        CHECK_NEAR((amp::shapeIntegral(x + h) - amp::shapeIntegral(x - h)) / (2 * h), amp::shape(x), 1e-6);
    }
    CHECK_NEAR(amp::shapeIntegral(1.0 - 1e-12), amp::shapeIntegral(1.0 + 1e-12), 1e-11);
    CHECK_NEAR(amp::shapeIntegral(-1.0 - 1e-12), amp::shapeIntegral(-1.0 + 1e-12), 1e-11);

    // The anti-aliasing: half a sample late for small signals, exact when clipped and when constant.
    amp::Adaa adaa;
    double last = 0.0;
    for (int k = 1; k <= 50; ++k) {
        const double x = k * 2e-6;
        CHECK_NEAR(adaa.process(x), 0.75 * (last + x), 1e-12);
        last = x;
    }
    adaa.prime(1.2);
    CHECK_EQ(adaa.process(1.7), 1.0);
    CHECK_EQ(adaa.process(1.0000001), 1.0);
    adaa.prime(-1.5);
    CHECK_EQ(adaa.process(-30.0), -1.0);
    for (const double x : {0.3, -0.7, 0.0}) {
        adaa.prime(x);
        CHECK_EQ(adaa.process(x), amp::shape(x));
        CHECK_EQ(adaa.process(x), amp::shape(x));
    }
    // Primed at its bias, a stage puts out exactly what it subtracts: silence stays exact zeros.
    for (const double bias : {0.0, 0.1, 0.25, 0.45}) {
        adaa.prime(bias);
        CHECK_EQ(adaa.process(bias), amp::shape(bias));
        CHECK_EQ(adaa.process(bias), amp::restValue(bias));
    }

    // The morph's ends are the voicings themselves; in between, every number moves.
    for (int a = 0; a < kModels; ++a) {
        for (int b = 0; b < kModels; ++b) {
            CHECK(sameVoicing(amp::blend(amp::voicing(a), amp::voicing(b), 0.0), amp::voicing(a)));
            CHECK(sameVoicing(amp::blend(amp::voicing(a), amp::voicing(b), 1.0), amp::voicing(b)));
        }
    }
    const amp::Voicing half = amp::blend(amp::voicing(amp::Clean), amp::voicing(amp::Lead), 0.5);
    CHECK_NEAR(half.inputHighpassHz, std::sqrt(30.0 * 80.0), 1e-9);
    CHECK_NEAR(half.gainSplit, 0.75, 1e-12);
    CHECK_NEAR(half.stages[1].bias, 0.175, 1e-12);

    // The transfer, a point at a time or a curve's worth; the preamp's part kept for any sag.
    const amp::Transfer t(amp::voicing(amp::Rock), 7, 3, 6, 8, 4, 9, kSampleRate);
    for (const double x : {-1.0, -0.2, 0.0, 1e-3, 0.5})
        CHECK_EQ(t(x, 1.5), amp::transfer(amp::voicing(amp::Rock), 7, 3, 6, 8, 4, 9, 1.5, kSampleRate, x));
    CHECK_EQ(t(0.0, 1.5), 0.0);  // the stages' biases cancel
    const amp::Transfer::Wave w = t.preamp(0.3);
    CHECK_EQ(t.peaksOf(t.power(w, 2.0)).high, t.peaks(0.3, 2.0).high);
    CHECK_EQ(t.peaksOf(t.power(w, 2.0)).low, t(-0.3, 2.0));
    CHECK(t(0.3, 2.0) < t(0.3, 0.0));  // the sag: less drive
    // Its slope through 0, and its peaks there (the biased stages' even term aside).
    CHECK_NEAR(t(1e-6, 0.0) / 1e-6, t.smallSignalGain(0.0), 1e-3 * t.smallSignalGain(0.0));
    CHECK_NEAR(db(t.smallSignalGain(1.5) / t.smallSignalGain(0.0)), -1.5, 1e-9);
    // Fewer points a period (the device's morph levels): the same level within 0.02 dB.
    for (int m = 0; m < kModels; ++m) {
        const amp::Transfer fine(amp::voicing(m), 5, 5, 5, 5, 5, 5, kSampleRate),
            coarse(amp::voicing(m), 5, 5, 5, 5, 5, 5, kSampleRate, 64);
        for (const double a : {1e-3, 0.03, 0.25, 0.9}) CHECK_NEAR(db(coarse.rms(a, 0.0) / fine.rms(a, 0.0)), 0.0, 0.02);
    }

    // A one-pole's gain at its corner, far below the rate: -3 dB.
    CHECK_NEAR(db(amp::onePoleLowpassGain(sub::dsp::onePoleCutoff(100.0, 192000.0), 100.0, 192000.0)), -3.01, 0.05);
    CHECK_NEAR(db(amp::onePoleHighpassGain(sub::dsp::onePoleCutoff(100.0, 192000.0), 100.0, 192000.0)), -3.01, 0.05);
    // The dials: Gain over the model's range, Volume ±12 dB, an audio-taper bass pot.
    CHECK_EQ(amp::gainDb(amp::voicing(amp::Lead), 0.0), 0.0);
    CHECK_EQ(amp::gainDb(amp::voicing(amp::Lead), 10.0), 30.0);
    CHECK_NEAR(amp::volumeDb(amp::voicing(amp::Clean), 10.0) - amp::volumeDb(amp::voicing(amp::Clean), 5.0), 12.0,
               1e-12);
    CHECK_NEAR(amp::bassTaper(5.0), 9.0 / 99.0, 1e-12);
    CHECK_EQ(amp::bassTaper(0.0), 0.0);
    CHECK_NEAR(amp::bassTaper(10.0), 1.0, 1e-12);
}

TEST_CASE("every amp model is level-matched at the defaults") {
    // A 220 Hz sine at -12 dBFS (peak) comes out at its own RMS (-15.05 dBFS), Mono and Dual.
    const Samples x = sine220();
    for (int m = 0; m < kModels; ++m) {
        for (const float dual : {0.f, 1.f}) {
            INFO(modelName(m) + (dual > 0.f ? " Dual" : " Mono"));
            Amp a(kSampleRate, model(m, {{"dual", dual}}));
            const auto [l, r] = a.playStereo(x, x);
            CHECK_NEAR(rmsDb(l, frameAt(0.5)), -15.05, 1.0);
            CHECK_NEAR(rmsDb(r, frameAt(0.5)), -15.05, 1.0);
        }
    }
}

TEST_CASE("each amp model has its character") {
    // THD at the defaults, and the balance of even and odd harmonics.
    struct Band {
        double low, high;
    };
    const Band bands[kModels] = {{0.02, 0.10}, {0.25, 0.45}, {0.05, 0.16}, {0.22, 0.40},
                                 {0.40, 1.00}, {0.28, 0.42}, {0.22, 0.40}};
    const Samples x = sine220();
    std::vector<std::vector<double>> h(kModels);
    for (int m = 0; m < kModels; ++m) {
        INFO(modelName(m));
        Amp a(kSampleRate, model(m));
        const Samples out = a.play(x);
        const double distortion = thd(out, 220.0);
        INFO("THD " + std::to_string(100 * distortion) + " %");
        CHECK(distortion >= bands[m].low);
        CHECK(distortion <= bands[m].high);
        h[static_cast<size_t>(m)] = harmonics(out, 220.0);
    }
    const auto level = [&](int m, int k) { return db(h[static_cast<size_t>(m)][static_cast<size_t>(k - 1)]); };
    CHECK(level(amp::Clean, 2) >= level(amp::Clean, 3) + 8.0);  // a triode's even harmonics
    CHECK(level(amp::Bass, 2) >= level(amp::Bass, 3) + 12.0);   // the fuzz's asymmetry
    CHECK(level(amp::Lead, 3) >= level(amp::Lead, 2) + 2.0);    // driven hard: squarer
}

TEST_CASE("Gain is the amp's distortion control") {
    const Samples x = sine220();
    for (int m = 0; m < kModels; ++m) {
        INFO(modelName(m));
        Amp low(kSampleRate, model(m, {{"gain", 0.f}})), high(kSampleRate, model(m, {{"gain", 10.f}}));
        const Samples quiet = low.play(x), driven = high.play(x);
        CHECK(thd(driven, 220.0) > thd(quiet, 220.0));
        // The stages' drive rises with it.
        double lowDrive = -100.0, highDrive = -100.0;
        for (const int d : {Drive1, Drive2, Drive3}) {
            const std::vector<float> a = low.display(d), b = high.display(d);
            lowDrive = std::max(lowDrive, meanDb(a, a.size() / 2));
            highDrive = std::max(highDrive, meanDb(b, b.size() / 2));
        }
        INFO(std::to_string(lowDrive) + " to " + std::to_string(highDrive) + " dB");
        CHECK(highDrive >= lowDrive + 14.0);
        if (m == amp::Clean) {
            CHECK(thd(quiet, 220.0) < 0.02);
            CHECK(thd(driven, 220.0) > 0.20);
        }
    }
}

TEST_CASE("Volume sets the level, and on Blues, Heavy and Bass drives the power amp") {
    const Samples x = sine220();
    for (int m = 0; m < kModels; ++m) {
        INFO(modelName(m));
        double level[3] = {};
        float power = 0.f;
        for (int v = 0; v < 3; ++v) {
            Amp a(kSampleRate, model(m, {{"volume", 5.f * v}}));
            level[v] = rmsDb(a.play(x), frameAt(0.5));
            if (v == 2) {
                const std::vector<float> p = a.display(Power);
                power = static_cast<float>(maxOf(slice(p, static_cast<int64_t>(p.size() / 2))));
            }
        }
        CHECK(level[1] >= level[0] + 3.0);
        CHECK(level[2] >= level[1] + 3.0);
        // The power stage's peak against its clipping point at Volume 10, steady (after the sag).
        INFO("power at 10: " + std::to_string(power) + " dB");
        if (m == amp::Blues || m == amp::Heavy || m == amp::Bass) {
            CHECK(power >= 3.f);
        } else {
            CHECK(power <= 1.f);
        }
    }
    // On Blues, turned up, it distorts much more; on Heavy, whose preamp is already
    // crunching at noon, clearly more too.
    Amp noon(kSampleRate, model(amp::Blues)), full(kSampleRate, model(amp::Blues, {{"volume", 10.f}}));
    CHECK(thd(full.play(x), 220.0) > 2.0 * thd(noon.play(x), 220.0));
    for (const double level : {0.25, 0.1}) {
        const Samples y = tone(220.0, level, 1.0);
        Amp heavy(kSampleRate, model(amp::Heavy)), cranked(kSampleRate, model(amp::Heavy, {{"volume", 10.f}}));
        const double atNoon = thd(heavy.play(y), 220.0), atTen = thd(cranked.play(y), 220.0);
        INFO("Heavy at " + std::to_string(db(level)) + " dBFS: THD " + std::to_string(100 * atNoon) + " % to " +
             std::to_string(100 * atTen) + " %");
        CHECK(atTen > atNoon + 0.07);
        CHECK(atTen > 1.25 * atNoon);
    }
}

TEST_CASE("the amp's supply sags under load and recovers") {
    const Samples x = sine220();
    Amp blues(kSampleRate, model(amp::Blues, {{"volume", 10.f}})),
        lead(kSampleRate, model(amp::Lead, {{"volume", 10.f}}));
    blues.play(x);
    lead.play(x);
    const std::vector<float> b = blues.display(Sag), l = lead.display(Sag);
    CHECK(b.back() >= 2.f);
    CHECK(l.back() <= 1.5f);
    CHECK(*std::min_element(b.begin(), b.end()) >= 0.f);

    // A loud burst (-6 dBFS), then quiet (-30): the sag lets go over its release,
    // back to what the quiet signal alone pulls the supply down by.
    Samples burst = tone(220.0, 0.5, 0.3);
    const Samples soft = tone(220.0, 0.0316, 2.0);
    burst.insert(burst.end(), soft.begin(), soft.end());
    Amp a(kSampleRate, model(amp::Blues)), quiet(kSampleRate, model(amp::Blues));
    a.play(burst);
    quiet.play(soft);
    const std::vector<float> sag = a.display(Sag);
    const float rest = quiet.display(Sag).back();
    const size_t burstEnd = static_cast<size_t>(frameAt(0.3) / 256);
    CHECK(maxOf(slice(sag, 0, static_cast<int64_t>(burstEnd))) > 3.f);
    CHECK(rest < 1.f);
    const size_t soon = static_cast<size_t>(frameAt(0.35) / 256), later = static_cast<size_t>(frameAt(1.8) / 256);
    REQUIRE(later < sag.size());
    CHECK(sag[soon] > rest + 1.5f);  // still sagging just after the burst
    CHECK_NEAR(sag[later], rest, 0.1);
}

TEST_CASE("the amp's tone curve is the sound") {
    // At Gain 0 and -60 dBFS everything is linear: what each tone control does to
    // the output is what toneResponseDb says it does.
    struct Turn {
        const char* id;
        float value;
        double freq;
    };
    const Turn turns[] = {{"bass", 10.f, 100.0},
                          {"middle", 0.f, 700.0},
                          {"treble", 10.f, 3000.0},
                          {"presence", 10.f, 6000.0},
                          {"presence", 0.f, 6000.0}};
    for (int m = 0; m < kModels; ++m) {
        const amp::Voicing& v = amp::voicing(m);
        for (const Turn& turn : turns) {
            INFO(modelName(m) + " " + turn.id + " " + std::to_string(turn.value));
            const Samples x = tone(turn.freq, 1e-3, 1.0);
            Amp noon(kSampleRate, model(m, {{"gain", 0.f}})),
                turned(kSampleRate, model(m, {{"gain", 0.f}, {turn.id, turn.value}}));
            const double played = toneAt(turned.play(x), turn.freq) - toneAt(noon.play(x), turn.freq);
            double dials[4] = {5, 5, 5, 5};
            const std::string id = turn.id;
            dials[id == "bass" ? 0 : id == "middle" ? 1 : id == "treble" ? 2 : 3] = turn.value;
            const double drawn =
                amp::toneResponseDb(v, dials[0], dials[1], dials[2], dials[3], kSampleRate, turn.freq) -
                amp::toneResponseDb(v, 5, 5, 5, 5, kSampleRate, turn.freq);
            INFO("played " + std::to_string(played) + " dB, drawn " + std::to_string(drawn) + " dB");
            CHECK_NEAR(played, drawn, 0.15);
        }
    }
}

TEST_CASE("the amp's transfer curve is the sound") {
    // In the linear region its slope is the device's gain at 1 kHz, every filter
    // included. (-100 dBFS: at -60 the hottest settings already bend V3.)
    constexpr double kLevel = 1e-5;
    const Samples x = tone(1000.0, kLevel, 0.5);
    for (int m = 0; m < kModels; ++m) {
        for (const Values& settings : {Values{}, Values{{"gain", 10.f}, {"bass", 0.f}, {"treble", 10.f}}}) {
            INFO(modelName(m) + (settings.empty() ? " defaults" : " gain 10, bass 0, treble 10"));
            Values values = model(m, settings);
            Amp a(kSampleRate, values);
            const double played = toneAt(a.play(x), 1000.0) - db(kLevel);
            const bool hot = !settings.empty();
            const amp::Transfer t(amp::voicing(m), hot ? 10 : 5, hot ? 0 : 5, 5, hot ? 10 : 5, 5, 5, kSampleRate);
            const double drawn = db(t.smallSignalGain(0.0));
            INFO("played " + std::to_string(played) + " dB, drawn " + std::to_string(drawn) + " dB");
            CHECK_NEAR(played, drawn, 0.15);
            CHECK_NEAR(db((t(kLevel, 0.0) - t(-kLevel, 0.0)) / (2.0 * kLevel)), drawn, 0.01);
        }
    }

    // Driven, the curve at ±the peak of a 1 kHz tone going in is where the tone's
    // highest and lowest values come out: the dots ride the curve where the sound
    // is. (With the sag as the device showed it.)
    struct Setting {
        const char* name;
        Values values;
        double within;  // dB
    };
    const std::vector<Setting> settings = {
        {"defaults", {}, 0.25},
        {"gain, presence and volume at 10", {{"gain", 10.f}, {"presence", 10.f}, {"volume", 10.f}}, 1.0}};
    for (const Setting& setting : settings) {
        for (const double a : {0.05, 0.25}) {
            for (int m = 0; m < kModels; ++m) {
                INFO(modelName(m) + ", " + setting.name + ", a tone of peak " + std::to_string(a));
                Amp amp(kSampleRate, model(m, setting.values));
                const Samples out = slice(amp.play(tone(1000.0, a, 1.0)), frameAt(0.5));
                const double high = *std::max_element(out.begin(), out.end()),
                             low = *std::min_element(out.begin(), out.end());
                const std::vector<float> sag = amp.display(Sag);
                double dials[6] = {5, 5, 5, 5, 5, 5};
                for (const auto& [id, value] : setting.values)
                    for (int d = 0; d < 6; ++d)
                        if (id == kDials[d]) dials[d] = value;
                const amp::Transfer t(amp::voicing(m), dials[0], dials[1], dials[2], dials[3], dials[4], dials[5],
                                      kSampleRate);
                const amp::Transfer::Peaks drawn = t.peaks(a, meanDb(sag, sag.size() / 2));
                INFO("played " + std::to_string(high) + " / " + std::to_string(low) + ", drawn " +
                     std::to_string(drawn.high) + " / " + std::to_string(drawn.low));
                CHECK_NEAR(db(drawn.high / high), 0.0, setting.within);
                CHECK_NEAR(db(drawn.low / low), 0.0, setting.within);
                CHECK_EQ(t(a, meanDb(sag, sag.size() / 2)), drawn.high);
                CHECK_EQ(t(-a, meanDb(sag, sag.size() / 2)), drawn.low);
            }
        }
    }
}

TEST_CASE("the amp's tone controls drive the stage after them") {
    // The stack sits before V3, so more bass drives it harder.
    const Samples x = tone(110.0, 0.25, 1.0);
    for (const int m : {static_cast<int>(amp::Lead), static_cast<int>(amp::Blues)}) {
        INFO(modelName(m));
        Amp noon(kSampleRate, model(m)), boosted(kSampleRate, model(m, {{"bass", 10.f}}));
        CHECK(thd(boosted.play(x), 110.0) > thd(noon.play(x), 110.0));
    }
}

TEST_CASE("Mono runs one amp on both channels, Dual one on each") {
    const Samples a = tone(220.0, 0.25, 0.5), b = tone(330.0, 0.25, 0.5);
    const Samples silence(a.size(), 0.f);
    // Mono: the sum, the same out of both sides.
    {
        Amp mono(kSampleRate, model(amp::Rock));
        const auto [l, r] = mono.playStereo(a, silence);
        CHECK_ARRAY_EQUAL(l, r);
        Samples half = a;
        for (float& v : half) v *= 0.5f;
        Amp one(kSampleRate, model(amp::Rock));
        CHECK_ALLCLOSE(l, one.play(half), 0.0, 1e-6);
    }
    // Dual: each side its own amp.
    {
        Amp dual(kSampleRate, model(amp::Rock, {{"dual", 1.f}}));
        const auto [l, r] = dual.playStereo(a, b);
        CHECK(db(amplitudeAt(l, 330.0)) < -100.0);
        CHECK(db(amplitudeAt(r, 220.0)) < -100.0);
        Amp left(kSampleRate, model(amp::Rock)), right(kSampleRate, model(amp::Rock));
        CHECK_ALLCLOSE(l, left.play(a), 0.0, 1e-6);
        CHECK_ALLCLOSE(r, right.play(b), 0.0, 1e-6);
    }
    // One channel: Dual Mono makes no difference.
    Amp monoOne(kSampleRate, model(amp::Lead)), dualOne(kSampleRate, model(amp::Lead, {{"dual", 1.f}}));
    CHECK_ARRAY_EQUAL(monoOne.play(a), dualOne.play(a));
}

TEST_CASE("switching the amp between Mono and Dual is click-free") {
    const Samples a = tone(220.0, 0.25, 1.5), b = tone(330.0, 0.25, 1.5);
    const int64_t on = frameAt(0.5), off = frameAt(1.0), lat = kLatency;
    const auto ms = [](double v) { return frameAt(v / 1000.0); };
    for (const int m : {static_cast<int>(amp::Clean), static_cast<int>(amp::Lead)}) {
        INFO(modelName(m));
        Amp switched(kSampleRate, model(m));
        const auto [l, r] = switched.playStereo(a, b, {{on, "dual", 1.f}, {off, "dual", 0.f}});
        CHECK(allFinite(l) && allFinite(r));
        for (const Samples* out : {&l, &r}) {
            for (const int64_t at : {on, off}) {
                const double steady =
                    std::max(clickiness(*out, at - ms(100), at), clickiness(*out, at + ms(40), at + ms(140)));
                const double change = clickiness(*out, at, at + ms(30) + lat);
                INFO(std::string(out == &l ? "left" : "right") + " at " + std::to_string(at) + ": " +
                     std::to_string(change) + " against " + std::to_string(steady));
                CHECK(change <= 1.5 * steady);
            }
        }
        // Once faded, the right side is its own amp's.
        Amp dual(kSampleRate, model(m, {{"dual", 1.f}}));
        const auto [dl, dr] = dual.playStereo(a, b);
        CHECK_NEAR(rmsDb(r, on + ms(40), on + ms(140)), rmsDb(dr, on + ms(40), on + ms(140)), 0.5);
        CHECK_NEAR(rmsDb(l, on + ms(40), on + ms(140)), rmsDb(dl, on + ms(40), on + ms(140)), 0.5);
    }
}

TEST_CASE("changing the amp model morphs without a click") {
    const Samples x = sine220(0.8);
    const int64_t at = frameAt(0.3);
    std::vector<Samples> steady;
    std::vector<double> steadyClicks;
    for (int m = 0; m < kModels; ++m) {
        Amp a(kSampleRate, model(m));
        steady.push_back(a.play(x));
        steadyClicks.push_back(clickiness(steady.back(), frameAt(0.1)));
    }
    double spliceRatio = 0.0;
    for (int from = 0; from < kModels; ++from) {
        for (int to = 0; to < kModels; ++to) {
            if (from == to) continue;
            INFO(modelName(from) + " to " + modelName(to));
            Amp a(kSampleRate, model(from));
            const Samples out = a.play(x, {{at, "type", static_cast<float>(to)}});
            CHECK(allFinite(out));
            const double reference =
                std::max(steadyClicks[static_cast<size_t>(from)], steadyClicks[static_cast<size_t>(to)]);
            const double change = clickiness(out, at, at + frameAt(0.06) + kLatency);
            INFO(std::to_string(change) + " against " + std::to_string(reference));
            // (Clean and Blues play the sine almost clean, so their own measure is
            // tiny: a floor of 1e-4, a step of 5e-6 (-106 dBFS), the Disperser's bound.)
            CHECK(change <= std::max(1.5 * reference, 1e-4));
            CHECK_NEAR(rmsDb(out, at + frameAt(0.08)), rmsDb(steady[static_cast<size_t>(to)], at + frameAt(0.08)), 0.5);
            // On the way, level-matched: no swell from a blend that clips where neither model
            // does (in 5 ms windows, against both models played steadily).
            for (int64_t w = at; w < at + frameAt(0.1); w += frameAt(0.005)) {
                const int64_t e = w + frameAt(0.005);
                const double level = rmsDb(out, w, e), a = rmsDb(steady[static_cast<size_t>(from)], w, e),
                             b = rmsDb(steady[static_cast<size_t>(to)], w, e);
                INFO("at " + std::to_string(w - at) + ": " + std::to_string(level) + " dB, the models " +
                     std::to_string(a) + " and " + std::to_string(b) + " dB");
                CHECK(level <= std::max(a, b) + 1.5);
                CHECK(level >= std::min(a, b) - 2.0);
            }
            // What the measure makes of a click: the two models spliced at once.
            Samples spliced = steady[static_cast<size_t>(from)];
            std::copy(steady[static_cast<size_t>(to)].begin() + at, steady[static_cast<size_t>(to)].end(),
                      spliced.begin() + at);
            spliceRatio = std::max(spliceRatio, clickiness(spliced, at - 10, at + 10) / reference);
        }
    }
    CHECK(spliceRatio >= 3.0);

    // In the linear region (-80 dBFS: nothing clips, the tone plays pure, so the
    // measure is sharp), every morph is smooth to within 1 % of the output's peak:
    // a step 40 dB under the signal would show.
    const Samples quiet = tone(220.0, 1e-4, 0.8);
    double worst = 0.0;
    for (int from = 0; from < kModels; ++from) {
        for (int to = 0; to < kModels; ++to) {
            if (from == to) continue;
            Amp a(kSampleRate, model(from));
            const Samples out = a.play(quiet, {{at, "type", static_cast<float>(to)}});
            worst =
                std::max(worst, clickiness(out, at, at + frameAt(0.06) + kLatency) / maxAbs(slice(out, frameAt(0.2))));
        }
    }
    INFO("the worst morph: " + std::to_string(worst) + " of the output's peak");
    CHECK(worst <= 0.01);
}

TEST_CASE("a model change works its levels out a cell at a time, the same in any blocks") {
    // The morph's levels (an amp::Transfer each) come one per 16-sample cell of the grid at
    // most: it sets off once the first four are known, three cells after the change, and until
    // then the amp plays on as the old model, bit for bit. A change inside a cell waits for the
    // next cell's start; it all goes by the grid, so it is the same whatever the blocks.
    const Samples x = sine220(0.5);
    const int64_t at = 16 * 900;  // a cell's start (the grid counts from the first sample)
    const auto firstDifference = [](const Samples& a, const Samples& b) {
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i] != b[i]) return static_cast<int64_t>(i);
        return static_cast<int64_t>(a.size());
    };
    for (const int block : {1, 37, 256, 1024}) {
        INFO("blocks of " + std::to_string(block));
        Amp steady(kSampleRate, model(amp::Rock));
        const Samples old = steady.play(x, {}, block);
        for (const int64_t offset : {0, 7}) {
            INFO("the change " + std::to_string(offset) + " samples into a cell");
            Amp a(kSampleRate, model(amp::Rock));
            const Samples out = a.play(x, {{at + offset, "type", static_cast<float>(amp::Lead)}}, block);
            const int64_t setsOff = at + (offset == 0 ? 0 : 16) + 3 * 16;
            CHECK_EQ(firstDifference(out, old), setsOff);
        }
    }
}

TEST_CASE("turning any of the amp's dials is click-free") {
    const Samples x = sine220(0.6);
    const int64_t at = frameAt(0.3);
    for (const int m : {static_cast<int>(amp::Rock), static_cast<int>(amp::Lead)}) {
        for (const char* dial : kDials) {
            for (const float from : {0.f, 10.f}) {
                const float to = 10.f - from;
                INFO(modelName(m) + " " + dial + " " + std::to_string(from) + " to " + std::to_string(to));
                Amp start(kSampleRate, model(m, {{dial, from}})), end(kSampleRate, model(m, {{dial, to}}));
                const double reference =
                    std::max(clickiness(start.play(x), frameAt(0.1)), clickiness(end.play(x), frameAt(0.1)));
                Amp a(kSampleRate, model(m, {{dial, from}}));
                const Samples out = a.play(x, {{at, dial, to}});
                const double change = clickiness(out, at, at + frameAt(0.05) + kLatency);
                INFO(std::to_string(change) + " against " + std::to_string(reference));
                CHECK(change <= 1.5 * reference);
            }
        }
        INFO(modelName(m) + " Dry/Wet");
        Amp dry(kSampleRate, model(m, {{"mix", 0.f}})), wet(kSampleRate, model(m));
        const double reference = std::max(clickiness(dry.play(x), frameAt(0.1)), clickiness(wet.play(x), frameAt(0.1)));
        Amp a(kSampleRate, model(m, {{"mix", 0.f}}));
        const Samples out = a.play(x, {{at, "mix", 100.f}});
        CHECK(clickiness(out, at, at + frameAt(0.05) + kLatency) <= 1.5 * reference);
    }
    // In the linear region (-80 dBFS), where the tone plays pure and the measure is
    // sharp, every model's every dial glides within 1 % of the output's peak: a
    // step 40 dB under the signal would show. (At -12 dBFS above, the crunch and
    // lead models' own clipping edges set the bound.)
    const Samples quiet = tone(220.0, 1e-4, 0.6);
    for (int m = 0; m < kModels; ++m) {
        for (const char* dial : kDials) {
            for (const float from : {0.f, 10.f}) {
                const float to = 10.f - from;
                INFO(modelName(m) + " " + dial + " " + std::to_string(from) + " to " + std::to_string(to));
                Amp start(kSampleRate, model(m, {{dial, from}})), end(kSampleRate, model(m, {{dial, to}}));
                const double level = std::max(maxAbs(slice(start.play(quiet), frameAt(0.2))),
                                              maxAbs(slice(end.play(quiet), frameAt(0.2))));
                Amp a(kSampleRate, model(m, {{dial, from}}));
                const Samples out = a.play(quiet, {{at, dial, to}});
                const double change = clickiness(out, at, at + frameAt(0.05) + kLatency) / level;
                INFO(std::to_string(change) + " of the output's peak");
                CHECK(change <= 0.01);
            }
        }
    }

    // What the measure makes of a click: Gain 0 and 10 spliced.
    Amp low(kSampleRate, model(amp::Rock, {{"gain", 0.f}})), high(kSampleRate, model(amp::Rock, {{"gain", 10.f}}));
    Samples spliced = low.play(x);
    const Samples loud = high.play(x);
    const double reference = std::max(clickiness(spliced, frameAt(0.1)), clickiness(loud, frameAt(0.1)));
    std::copy(loud.begin() + at, loud.end(), spliced.begin() + at);
    CHECK(clickiness(spliced, at - 10, at + 10) >= 3.0 * reference);
}

TEST_CASE("the amp's automation plays through the engine sample-accurately") {
    sub::Engine engine;
    engine.setClipFadeMs(0);
    const std::string path = makeWav(stereo(tone(221.25, 0.25, 3.0)), 2);
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, 0.0, 3.0, 0.0, 1.f)});
    const Samples dry = engine.renderOffline(0.0, 3 * kSampleRate);  // what the amp is fed
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "amp", -1);
    setParam(engine, id, "type", 3.f);

    // Gain from 5 to 10 at beat 2 (1 s).
    using Points = std::vector<sub::AutomationPoint>;
    engine.setTrackAutomation(track, {{id, "gain", Points{{0.0, 0.5f, 0.f}, {2.0, 0.5f, 0.f}, {2.0, 1.f, 0.f}}}});
    const Samples out = engine.renderOffline(0.0, 3 * kSampleRate);
    CHECK(allFinite(out));
    // A standalone amp fed the same, Gain handed to it at frame 48000: the engine's
    // output is what it puts out, its latency compensated. (The engine's blocks cut
    // the glide's chunks elsewhere: within 1e-4. A sample late is 4e-4 off.)
    const auto standalone = [&](int64_t at) {
        Samples l = channel(dry, 0), r = channel(dry, 1);
        l.resize(l.size() + kLatency, 0.f);
        r.resize(r.size() + kLatency, 0.f);
        Amp a(kSampleRate, model(amp::Rock));
        a.run({&l, &r}, {{at, "gain", 10.f}});
        return std::pair{slice(l, kLatency), slice(r, kLatency)};
    };
    const auto [l, r] = standalone(kSampleRate);
    CHECK_ALLCLOSE(channel(out, 0), l, 0.0, 1e-4);
    CHECK_ALLCLOSE(channel(out, 1), r, 0.0, 1e-4);
    for (const int64_t late : {-1, 1}) {
        INFO("the change " + std::to_string(late) + " samples late");
        CHECK(!allclose(channel(out, 0), standalone(kSampleRate + late).first, 0.0, 1e-4));
    }
}

TEST_CASE("the amp's Dry/Wet blends in the input delayed by its latency") {
    const Samples x = noise(kSampleRate / 2, 21, 0.4f);
    Samples delayed(x.size(), 0.f);
    std::copy(x.begin(), x.end() - kLatency, delayed.begin() + kLatency);
    for (const int m : {static_cast<int>(amp::Clean), static_cast<int>(amp::Lead)}) {
        INFO(modelName(m));
        Amp dry(kSampleRate, model(m, {{"mix", 0.f}, {"gain", 8.f}, {"treble", 2.f}}));
        CHECK_ARRAY_EQUAL(dry.play(x), delayed);
        Amp wet(kSampleRate, model(m)), half(kSampleRate, model(m, {{"mix", 50.f}}));
        const Samples w = wet.play(x), h = half.play(x);
        Samples want(x.size());
        for (size_t i = 0; i < x.size(); ++i) want[i] = 0.5f * delayed[i] + 0.5f * w[i];
        CHECK_ALLCLOSE(h, want, 0.0, 1e-6);
    }
}

TEST_CASE("the amp's latency is reported and compensated") {
    // The wet comes out 37 samples late, and its filters' own phase delay (the design's):
    // a small 1 kHz tone's phase, to within a twentieth of a sample, every model.
    const Samples x = tone(1000.0, 1e-5, 0.5);
    const double period = kSampleRate / 1000.0;
    for (int m = 0; m < kModels; ++m) {
        INFO(modelName(m));
        Amp a(kSampleRate, model(m, {{"gain", 0.f}}));
        const Samples y = a.play(x);
        const int64_t from = static_cast<int64_t>(x.size()) - 12 * static_cast<int64_t>(period);
        std::complex<double> in = 0.0, out = 0.0;
        for (int64_t i = from; i < static_cast<int64_t>(x.size()); ++i) {
            const std::complex<double> w = std::polar(1.0, -2.0 * kPi * static_cast<double>(i) / period);
            in += static_cast<double>(x[static_cast<size_t>(i)]) * w;
            out += static_cast<double>(y[static_cast<size_t>(i)]) * w;
        }
        const double lag = std::fmod(-std::arg(out / in) / (2.0 * kPi) * period + 4.0 * period, period);
        const amp::Transfer t(amp::voicing(m), 0, 5, 5, 5, 5, 5, kSampleRate);
        const double want = std::fmod(kLatency + t.phaseDelay() + 4.0 * period, period);
        INFO("played " + std::to_string(lag) + " samples (mod " + std::to_string(period) +
             "), the latency and the filters " + std::to_string(want));
        CHECK_NEAR(lag, want, 0.05);
    }

    // Through the engine: a dry track's click and one through the amp (fully dry) land together.
    {
        sub::Engine engine;
        engine.setClipFadeMs(0);
        constexpr int64_t kClick = 1000;
        stereoClickTrack(engine, 1.f, 0.f, kClick, 1.0);
        const uint32_t wet = stereoClickTrack(engine, 0.f, 1.f, kClick, 1.0);
        const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(wet), "amp", -1);
        setParam(engine, id, "mix", 0.f);
        CHECK_EQ(engine.processorInfo(id).latency, kLatency);
        const Samples out = engine.renderOffline(0.0, kSampleRate / 2);
        const std::vector<int64_t> left = nonzero(channel(out, 0)), right = nonzero(channel(out, 1));
        REQUIRE(left.size() == 1);
        CHECK(right == left);
    }
    // Fully wet: the engine starts the amp's track the latency early, so what comes out is
    // what an amp on its own puts out, the latency later, on the dry track's time.
    {
        sub::Engine engine;
        engine.setClipFadeMs(0);
        const Samples n = noise(kSampleRate, 23, 0.05f);
        const Samples silent(n.size(), 0.f);
        clipTrack(engine, makeWav(interleave({n, silent}), 2), 0.0, 1.0);
        const uint32_t wet = clipTrack(engine, makeWav(interleave({silent, n}), 2), 0.0, 1.0);
        const Samples dry = channel(engine.renderOffline(0.0, kSampleRate), 1);  // what the amp is fed
        const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(wet), "amp", -1);
        setParam(engine, id, "gain", 0.f);
        setParam(engine, id, "dual", 1.f);
        const Samples out = engine.renderOffline(0.0, kSampleRate);
        Samples l(dry.size() + kLatency, 0.f), r = dry;
        r.resize(l.size(), 0.f);
        Amp alone(kSampleRate, model(amp::Clean, {{"gain", 0.f}, {"dual", 1.f}}));
        alone.run({&l, &r});
        CHECK_ALLCLOSE(channel(out, 1), slice(r, kLatency), 0.0, 1e-6);
        CHECK(rms(channel(out, 1)) > 1e-3);
    }
}

TEST_CASE("the amp's tail covers its ringing, and silence rings out to exact zeros") {
    std::vector<std::pair<int, Values>> settings;
    for (int m = 0; m < kModels; ++m) settings.push_back({m, {{"volume", 10.f}}});
    settings.push_back({amp::Bass, {{"bass", 10.f}, {"middle", 10.f}, {"treble", 0.f}, {"volume", 10.f}}});
    for (const auto& [m, values] : settings) {
        INFO(modelName(m) + (values.size() > 1 ? " (its slowest stack)" : ""));
        Amp a(kSampleRate, model(m, values));
        Samples x = noise(kSampleRate / 2, 24, 0.5f);
        const int64_t end = static_cast<int64_t>(x.size());
        x.resize(static_cast<size_t>(5 * kSampleRate), 0.f);
        const Samples out = a.play(x);
        CHECK(allFinite(out));
        const int64_t tail = a.processor().tailSamples();
        CHECK(maxAbs(slice(out, end + tail)) < 1e-4);
        CHECK(allEqual(slice(out, end + 4 * kSampleRate), 0.0));
        for (const float v : out) CHECK(v == 0.f || std::abs(v) >= std::numeric_limits<float>::min());
    }
    // Denormal input passes as finite numbers and dies away to zeros too.
    Amp a(kSampleRate, model(amp::Lead));
    Samples tiny(static_cast<size_t>(3 * kSampleRate), 0.f);
    for (size_t i = 0; i < 1000; ++i) tiny[i] = (i % 2 ? 1e-40f : -1e-41f);
    const Samples quiet = a.play(tiny);
    CHECK(allFinite(quiet));
    CHECK(allEqual(slice(quiet, 2 * kSampleRate), 0.0));
}

TEST_CASE("reset and a new sample rate start the amp from silence") {
    const Samples n = noise(kSampleRate / 2, 25, 0.5f);
    const Samples silence(4096, 0.f);
    const Samples x = sine220(0.5);
    for (int m = 0; m < kModels; ++m) {
        INFO(modelName(m));
        Amp a(kSampleRate, model(m, {{"gain", 7.f}}));
        a.play(n);
        a.processor().reset();
        CHECK(allEqual(a.play(silence), 0.0));  // from the first sample: the stages at rest at their biases
        a.play(n);
        a.processor().reset();
        Amp fresh(kSampleRate, model(m, {{"gain", 7.f}}));
        CHECK_ARRAY_EQUAL(a.play(x), fresh.play(x));
    }
    // A new rate: level-matched there too.
    Amp a(kSampleRate, model(amp::Rock));
    a.play(n);
    a.processor().prepare(96000.0, kBlock);
    Amp at96(96000.0, model(amp::Rock));
    const Samples x96 = tone(220.0, 0.25, 1.0, 96000.0);
    const Samples out = a.play(x96);
    CHECK_ARRAY_EQUAL(out, at96.play(x96));
    CHECK_NEAR(rmsDb(out, 48000), -15.05, 1.5);
}

TEST_CASE("the amp stays finite and bounded at the extremes and at any rate") {
    const Samples hot = tone(220.0, 1.0, 0.5);
    const Samples loud = noise(kSampleRate / 2, 26, 1.0f);
    for (int m = 0; m < kModels; ++m) {
        std::vector<std::pair<std::string, Values>> cases = {{"all 0", kAllAtZero}, {"all 10", kAllAtTen}};
        for (int d = 0; d < 6; ++d) {
            Values v = kAllAtZero;
            v[static_cast<size_t>(d)].second = 10.f;
            cases.push_back({std::string(kDials[d]) + " alone at 10", v});
        }
        for (const auto& [name, values] : cases) {
            INFO(modelName(m) + ", " + name);
            for (const Samples* x : {&hot, &loud}) {
                Amp a(kSampleRate, model(m, values));
                const Samples out = a.play(*x);
                CHECK(allFinite(out));
                CHECK(maxAbs(out) <= 2.0);
            }
        }
    }
    for (const double rate : {22050.0, 44100.0, 96000.0, 192000.0}) {
        for (int m = 0; m < kModels; ++m) {
            INFO(modelName(m) + " at " + std::to_string(rate));
            Amp a(rate, model(m));
            const Samples out = a.play(tone(220.0, 0.25, 1.0, rate));
            CHECK(allFinite(out));
            CHECK(maxAbs(out) <= 2.0);
            CHECK_NEAR(rmsDb(out, static_cast<int64_t>(rate / 2)), -15.05, 1.5);
        }
    }
    // +12 dBFS of noise into Lead with every dial at 10.
    Samples screaming = noise(kSampleRate, 27, 4.0f);
    Amp lead(kSampleRate, model(amp::Lead, kAllAtTen));
    const Samples out = lead.play(screaming);
    CHECK(allFinite(out));
    CHECK(maxAbs(out) <= 2.0);
}

TEST_CASE("the amp puts out no DC") {
    const Samples x = sine220(2.0);
    for (int m = 0; m < kModels; ++m) {
        INFO(modelName(m));
        Amp a(kSampleRate, model(m));
        CHECK(std::abs(meanOf(a.play(x), frameAt(1.5))) < 2e-4);
    }
}

TEST_CASE("the amp keeps what folds back far down") {
    const Samples x = tone(997.0, 0.25, 0.5);
    for (int m = 0; m < kModels; ++m) {
        INFO(modelName(m));
        Amp high(kSampleRate, model(m, {{"gain", 10.f}})), noon(kSampleRate, model(m)),
            hot(kSampleRate, model(m, {{"gain", 10.f}, {"presence", 10.f}, {"volume", 10.f}}));
        const double atTen = aliasDb(high.play(x), 997.0), atFive = aliasDb(noon.play(x), 997.0),
                     allUp = aliasDb(hot.play(x), 997.0);
        INFO(std::to_string(atTen) + " / " + std::to_string(atFive) + " dB, with Presence and Volume at 10 too " +
             std::to_string(allUp) + " dB");
        CHECK(atTen <= -55.0);
        CHECK(atFive <= -65.0);
        // (A preamp already clipped, its edges boosted by Presence, into a power stage
        // driven hard: the power tubes' input low-pass keeps this down.)
        CHECK(allUp <= -55.0);
    }
}

TEST_CASE("the amp's displays show its input, each stage's drive, the sag and its output") {
    const Samples x = sine220(1.0);
    for (int m = 0; m < kModels; ++m) {
        INFO(modelName(m));
        Amp a(kSampleRate, model(m));
        const auto [l, r] = a.playStereo(x, x);
        std::vector<std::vector<float>> d;
        for (int i = 0; i < 7; ++i) d.push_back(a.display(i));
        for (const auto& values : d) CHECK_EQ(values.size(), size_t{187});
        for (size_t w = 1; w < d[Input].size(); ++w) CHECK_NEAR(d[Input][w], -12.0, 0.2);
        for (size_t w = 0; w < d[Output].size(); ++w) {
            double peak = 0.0;
            for (size_t i = 256 * w; i < 256 * (w + 1); ++i)
                peak = std::max({peak, std::abs<double>(l[i]), std::abs<double>(r[i])});
            CHECK_NEAR(d[Output][w], std::max(-90.0, db(peak)), 0.1);
        }
        for (const float s : d[Sag]) CHECK(s >= 0.f);
        const double v1 = meanDb(d[Drive1], 94), v2 = meanDb(d[Drive2], 94), v3 = meanDb(d[Drive3], 94);
        INFO("V1 " + std::to_string(v1) + ", V2 " + std::to_string(v2) + ", V3 " + std::to_string(v3));
        if (m == amp::Lead) {  // driven hardest after V1
            CHECK(v2 >= v1 + 6.0);
            CHECK(v3 >= v1 + 6.0);
        }
        if (m == amp::Clean) {  // V2 a clean cathode follower
            CHECK(v2 <= v1 - 10.0);
            CHECK(v2 <= v3 - 10.0);
        }
    }
    Amp a(kSampleRate, model(amp::Lead));
    a.play(Samples(kSampleRate / 4, 0.f));
    for (const int i :
         {static_cast<int>(Input), static_cast<int>(Output), static_cast<int>(Drive1), static_cast<int>(Power)})
        CHECK(allEqual(a.display(i), -90.0));
    CHECK(allEqual(a.display(Sag), 0.0));
}

#ifdef NDEBUG
TEST_CASE("the amp's cost stays bounded") {
    const Samples n = noise(static_cast<size_t>(10 * kSampleRate), 28, 0.25f);
    const double dual = bestOfThree(model(amp::Lead, {{"gain", 10.f}, {"dual", 1.f}}), n, n, {});
    const double mono = bestOfThree(model(amp::Lead, {{"gain", 10.f}}), n, n, {});
    INFO("Dual " + std::to_string(10.0 * dual) + " %, Mono " + std::to_string(10.0 * mono) + " % of real time");
    CHECK(dual < 0.1 * 10.0);
    CHECK(mono < dual);

    // A model change's first block (32 frames: two cells, two of the morph's levels) costs a
    // few steady ones, not ten (as four levels in its first cell did): medians over 60 changes.
    using Clock = std::chrono::steady_clock;
    Amp a(kSampleRate, model(amp::Rock));
    sub::ProcessContext ctx;
    ctx.sampleRate = kSampleRate;
    Samples l(32), r(32);
    float* channels[2] = {l.data(), r.data()};
    const Samples input = noise(static_cast<size_t>(kSampleRate), 32, 0.25f);
    std::vector<double> steady, starts;
    for (int b = 0; b < 6100; ++b) {
        const auto from = static_cast<std::ptrdiff_t>(b % 1400) * 32;
        std::copy_n(input.begin() + from, 32, l.begin());
        std::copy_n(input.begin() + from, 32, r.begin());
        const bool change = b >= 100 && b % 100 == 0;  // (each morph, 77 blocks, done before the next)
        if (change) a.set("type", static_cast<float>((b / 100) % kModels));
        const auto start = Clock::now();
        a.processor().process(ctx, channels, 2, 32);
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        if (change) {
            starts.push_back(seconds);
        } else if (b >= 100 && b % 100 >= 80) {
            steady.push_back(seconds);
        }
    }
    const auto median = [](std::vector<double> v) {
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        return v[v.size() / 2];
    };
    INFO("a change's first block " + std::to_string(1e6 * median(starts)) + " us, a steady one " +
         std::to_string(1e6 * median(steady)) + " us");
    CHECK(median(starts) < 6.0 * median(steady));
}
#endif

TEST_CASE("the amp plays the same in any block size") {
    const Samples x = noise(kSampleRate, 29, 0.3f);
    Amp reference(kSampleRate, model(amp::Lead));
    const Samples want = reference.play(x, {}, 64);
    for (const int block : {1, 37, 256, 1024}) {
        INFO(std::to_string(block));
        Amp a(kSampleRate, model(amp::Lead));
        CHECK_ARRAY_EQUAL(a.play(x, {}, block), want);
    }
    // Prepared for 64 frames and handed 1000 at once: it works in slices.
    Amp sliced(kSampleRate, model(amp::Lead), 64);
    CHECK_ARRAY_EQUAL(sliced.play(x, {}, 1000), want);
    // From silence and back to it, long enough to sleep, in Dual with the sides
    // coming in softly at different times: every flush and the sleep fall on the
    // same samples whatever the blocks (the supply's envelope's first tiny values
    // too, which a flush at the blocks' ends would drop in some and not others).
    {
        Samples left(static_cast<size_t>(kSampleRate / 10), 0.f), right(left.size() + 3000, 0.f);
        for (Samples* side : {&left, &right}) {
            Samples burst = noise(static_cast<size_t>(kSampleRate / 2), side == &left ? 30 : 31, 0.3f);
            for (size_t i = 0; i < burst.size(); ++i)
                burst[i] *=
                    static_cast<float>(std::sin(kPi * static_cast<double>(i) / static_cast<double>(burst.size())));
            side->insert(side->end(), burst.begin(), burst.end());
            side->resize(static_cast<size_t>(2 * kSampleRate), 0.f);
            side->insert(side->end(), burst.begin(), burst.end());
        }
        right.resize(left.size());
        for (const int m : {static_cast<int>(amp::Clean), static_cast<int>(amp::Blues), static_cast<int>(amp::Bass)}) {
            const Values values = model(m, {{"dual", 1.f}, {"volume", 10.f}});
            Amp reference(kSampleRate, values);
            const auto [wantLeft, wantRight] = reference.playStereo(left, right, {}, 64);
            for (const int block : {1, 1000, 1024}) {
                INFO(modelName(m) + " in blocks of " + std::to_string(block));
                Amp a(kSampleRate, values);
                const auto [l, r] = a.playStereo(left, right, {}, block);
                CHECK_ARRAY_EQUAL(l, wantLeft);
                CHECK_ARRAY_EQUAL(r, wantRight);
            }
        }
    }
    // With Gain gliding and the model morphing: the chunks fall on a grid of
    // their own (the meters'), but a block's end cuts one in two, which bends
    // a glide's ramp at another point: next to nothing.
    const std::vector<Change> glide = {{frameAt(0.3), "gain", 10.f}, {frameAt(0.5), "type", 1.f}};
    Amp a37(kSampleRate, model(amp::Lead)), a256(kSampleRate, model(amp::Lead));
    CHECK_ALLCLOSE(a37.play(x, glide, 37), a256.play(x, glide, 256), 0.0, 1e-4);
}

TEST_CASE("an amp at rest sleeps through silence and wakes as a fresh one") {
    // It sleeps: silence after noise costs a fraction of what the noise did. (Asleep or
    // awake, it puts out the same exact zeros: only the time tells. Both timed in the
    // same build, so this holds in a debug one too; asleep measured 0.04 to 0.06.)
    {
        const Samples n = noise(static_cast<size_t>(4 * kSampleRate), 28, 0.25f);
        const Samples quiet(n.size(), 0.f);
        Samples before = slice(n, 0, kSampleRate);
        before.resize(static_cast<size_t>(4 * kSampleRate), 0.f);  // (asleep by its end)
        const Values lead = model(amp::Lead, {{"gain", 10.f}, {"dual", 1.f}});
        const double playing = bestOfThree(lead, n, n, {}), silent = bestOfThree(lead, quiet, quiet, before);
        INFO("silence " + std::to_string(silent / playing) + " of the noise's time");
        CHECK(silent < 0.5 * playing);
    }
    Samples x = sine220(1.0);
    x.resize(static_cast<size_t>(4 * kSampleRate), 0.f);
    const Samples again = sine220(0.5);
    x.insert(x.end(), again.begin(), again.end());
    const int64_t back = frameAt(4.0);
    {
        Amp a(kSampleRate, model(amp::Lead));
        const auto [l, r] = a.playStereo(x, x);
        Amp fresh(kSampleRate, model(amp::Lead));
        const auto [fl, fr] = fresh.playStereo(again, again);
        CHECK_ARRAY_EQUAL(slice(l, back), fl);
        CHECK_ARRAY_EQUAL(slice(r, back), fr);
    }
    // The model changed while it was silent: it wakes as that model.
    {
        Amp a(kSampleRate, model(amp::Lead));
        const auto [l, r] = a.playStereo(x, x, {{frameAt(2.0), "type", static_cast<float>(amp::Bass)}});
        Amp fresh(kSampleRate, model(amp::Bass));
        const auto [fl, fr] = fresh.playStereo(again, again);
        CHECK_ARRAY_EQUAL(slice(l, back), fl);
    }
    // Dual with one side silent: that side's amp puts out exact zeros (asleep), the other plays on.
    {
        const Samples silence(x.size(), 0.f);
        Amp a(kSampleRate, model(amp::Lead, {{"dual", 1.f}}));
        const auto [l, r] = a.playStereo(x, silence);
        CHECK(allEqual(r, 0.0));
        Amp one(kSampleRate, model(amp::Lead));
        CHECK_ALLCLOSE(l, one.play(x), 0.0, 1e-6);
    }
}
