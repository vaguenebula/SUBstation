// The built-in Spectral Compressor: a compressor per frequency over an STFT.
// Its listing, its gentle defaults and the shared maths (SpectralDesign.h);
// its latency (the frame and a hop, to the sample) and transparency; levels
// that read pink noise flat; downward and upward compression, Range, Tilt,
// Smoothing and the Focus band with numbers; Delta; attack and release in
// time; Stereo Link; keying by a sidechain (alone and through an engine);
// every control changing without a click; automation to the sample; reset,
// silence ringing out to exact zeros, extremes, one channel; its displays, in
// step with what is heard; the engine lining other tracks up with it; and what
// it costs, in all and per audio callback.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Engine.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/SpectralDesign.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;
namespace spectral = sub::spectral;

namespace {

constexpr int kBlock = 1024;  // the renderer's largest block (Renderer::kMaxBlock)
constexpr int kN = 2048;      // the frame at 48 kHz
constexpr int kH = kN / 4;    // the hop
constexpr int kL = kN + kH;   // the latency: the frame, and the hop its work is spread over
constexpr int kPoints = spectral::kDisplayPoints;

enum Display { kInput = 0, kKey, kOutput, kGain, kInLevel, kOutLevel, kDisplays };

using Values = std::vector<std::pair<std::string, float>>;

// A parameter's change at a frame, as automation hands it over.
struct Change {
    int64_t frame;
    std::string id;
    float value;
};

// What goes into the sidechain: null channels are silence; `connected` says a source was chosen at all.
struct Key {
    const Samples* left = nullptr;
    const Samples* right = nullptr;
    bool connected = false;
};

// Settings most cases start from: steady, unbiased levels (equal attack and release read the mean), a hard
// knee, the widest smoothing and room for the gains.
Values steady(Values more = {}) {
    Values values = {{"knee", 0.f}, {"smooth", 100.f}, {"attack", 300.f}, {"release", 300.f}, {"range", 48.f}};
    values.insert(values.end(), more.begin(), more.end());
    return values;
}

// A Spectral Compressor on its own, outside an engine, at any sample rate: processed in blocks, its changes
// handed over as automation (so its blocks split there) as the renderer does, its sidechain as the renderer
// gives it. Every display it publishes is collected as it plays.
class Spectral {
public:
    explicit Spectral(const Values& values = {}, double rate = kSampleRate)
        : processor_(sub::BuiltinRegistry::instance().create("spectral")), rate_(rate) {
        for (const auto& [id, value] : values) set(id, value);
        processor_->prepare(rate, kBlock);
    }

    sub::Processor& processor() { return *processor_; }
    int frame() const { return processor_->tailSamples(); }  // (the tail is the frame)

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
    void run(const std::vector<Samples*>& channels, const Key& key = {}, const std::vector<Change>& changes = {},
             int block = 256) {
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
            processor_->setSidechainConnected(key.connected);
            processor_->setSidechain(key.left ? key.left->data() + start : nullptr,
                                     key.right ? key.right->data() + start : nullptr);
            ctx.samplePos = start;
            processor_->process(ctx, pointers, static_cast<int>(channels.size()), n);
            processor_->setSidechain(nullptr, nullptr);
            processor_->clearAutomation();
            for (int d = 0; d < kDisplays; ++d) positions_[d] = processor_->readDisplay(d, positions_[d], shown_[d]);
        }
    }
    Samples play(Samples mono, const std::vector<Change>& changes = {}, const Key& key = {}) {
        run({&mono}, key, changes);
        return mono;
    }
    std::pair<Samples, Samples> play(Samples left, Samples right, const std::vector<Change>& changes = {},
                                     const Key& key = {}) {
        run({&left, &right}, key, changes);
        return {left, right};
    }

    // Every value a display published so far.
    const std::vector<float>& shown(int display) const { return shown_[display]; }
    // A spectral display's frames (128 values each), in the order published.
    std::vector<std::vector<float>> frames(int display) const {
        std::vector<std::vector<float>> out;
        const std::vector<float>& v = shown_[display];
        for (size_t at = 0; at + kPoints <= v.size(); at += kPoints)
            out.emplace_back(v.begin() + static_cast<std::ptrdiff_t>(at),
                             v.begin() + static_cast<std::ptrdiff_t>(at + kPoints));
        return out;
    }

private:
    std::shared_ptr<sub::Processor> processor_;
    double rate_;
    std::vector<float> shown_[kDisplays];
    uint64_t positions_[kDisplays] = {};
};

// The input sample a published spectral frame is centred on (frame i, from 0, published at hop i + 1): the
// frame that went in three hops before, centred half a frame before that, so the one heard as it is published.
int64_t frameCentre(size_t i, int frame) {
    const int hop = frame / 4;
    return static_cast<int64_t>(i + 1) * hop - (frame + hop);
}

// The display point nearest a frequency.
int pointAt(double hz) {
    int best = 0;
    for (int j = 1; j < kPoints; ++j)
        if (std::abs(std::log(spectral::displayFrequency(j) / hz)) <
            std::abs(std::log(spectral::displayFrequency(best) / hz)))
            best = j;
    return best;
}

// Display frames [from, to) at point j: their mean in dB, or the mean of their powers in dB.
double meanDb(const std::vector<std::vector<float>>& frames, size_t from, size_t to, int j) {
    double sum = 0.0;
    for (size_t i = from; i < to; ++i) sum += frames[i][static_cast<size_t>(j)];
    return sum / static_cast<double>(to - from);
}
double meanPowerDb(const std::vector<std::vector<float>>& frames, size_t from, size_t to, int j) {
    double sum = 0.0;
    for (size_t i = from; i < to; ++i) sum += std::pow(10.0, frames[i][static_cast<size_t>(j)] / 10.0);
    return 10.0 * std::log10(sum / static_cast<double>(to - from));
}

// The frames published while input samples [from, to) (seconds) were heard.
std::pair<size_t, size_t> framesBetween(double from, double to, size_t count, double rate = kSampleRate,
                                        int frame = kN) {
    size_t first = count, last = 0;
    for (size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(frameCentre(i, frame)) / rate;
        if (t >= from && first == count) first = i;
        if (t < to) last = i + 1;
    }
    return {std::min(first, last), last};
}

// Pink noise (power ∝ 1/f from 20 Hz to 20 kHz, nothing outside) at `rmsDb` dBFS RMS: a block of 2^17 made by
// FFT synthesis with random phases, tiled.
Samples pinkNoise(size_t frames, double rmsDb, uint64_t seed, double rate = kSampleRate) {
    constexpr size_t kLength = size_t{1} << 17;
    Rng rng(seed);
    std::vector<Complex> bins(kLength);
    for (size_t k = 1; k < kLength / 2; ++k) {
        const double f = static_cast<double>(k) * rate / kLength;
        const double phase = 2.0 * kPi * rng.random();
        if (f < 20.0 || f > 20000.0) continue;
        bins[k] = std::polar(1.0 / std::sqrt(f), phase);
        bins[kLength - k] = std::conj(bins[k]);
    }
    fftPow2(bins, true);
    double sum = 0.0;
    for (const Complex& v : bins) sum += v.real() * v.real();
    const double scale = std::pow(10.0, rmsDb / 20.0) / std::sqrt(sum / kLength);
    Samples x(frames);
    for (size_t i = 0; i < frames; ++i) x[i] = static_cast<float>(bins[i % kLength].real() * scale);
    return x;
}

// Gaussian white noise at `rmsDb` dBFS RMS.
Samples whiteNoise(size_t frames, double rmsDb, uint64_t seed) {
    Rng rng(seed);
    const double sigma = std::pow(10.0, rmsDb / 20.0);
    Samples x(frames);
    for (float& v : x) v = static_cast<float>(sigma * rng.normal());
    return x;
}

// RMS of samples [from, to) in dB.
double rmsDb(const Samples& x, int64_t from, int64_t to) { return 20.0 * std::log10(rms(slice(x, from, to)) + 1e-30); }

// A sine that fades in over 100 ms.
Samples smoothSine(double freq, double seconds, double amplitude, double rate = kSampleRate) {
    Samples x(static_cast<size_t>(seconds * rate));
    const double fadeIn = 0.1 * rate;
    for (size_t i = 0; i < x.size(); ++i) {
        const double t = std::min(1.0, static_cast<double>(i) / fadeIn);
        x[i] = static_cast<float>(t * t * (3.0 - 2.0 * t) * amplitude * std::sin(2.0 * kPi * freq * i / rate));
    }
    return x;
}

// The largest 6th difference over [from, to): a steep high-pass (8 times, 18 dB, more sensitive at Nyquist than
// at a quarter of the rate, 10^5 times more than at 2 kHz). A step of d shows as up to 20 d; a smooth signal
// well below Nyquist hardly at all.
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

// `x` delayed by `frames` (zeros first), the same length.
Samples delayed(const Samples& x, int frames) {
    Samples out(x.size(), 0.f);
    std::copy(x.begin(), x.end() - std::min<std::ptrdiff_t>(frames, static_cast<std::ptrdiff_t>(x.size())),
              out.begin() + std::min<std::ptrdiff_t>(frames, static_cast<std::ptrdiff_t>(x.size())));
    return out;
}

// The energy of `x` (Hann-windowed) between two frequencies, in dB.
double bandDb(const Samples& x, double low, double high, double rate = kSampleRate) {
    const std::vector<double> s = spectrum(x, hanning(x.size()));
    double sum = 0.0;
    for (size_t k = 0; k < s.size(); ++k) {
        const double f = static_cast<double>(k) * rate / static_cast<double>(x.size());
        if (f >= low && f <= high) sum += s[k] * s[k];
    }
    return 10.0 * std::log10(sum + 1e-300);
}

double gainToDb(double gain) { return 20.0 * std::log10(gain); }

}  // namespace

TEST_CASE("spectral: listing") {
    const sub::BuiltinInfo info = builtinInfo("spectral");
    CHECK_EQ(info.name, std::string("Spectral Compressor"));
    CHECK(!info.isInstrument());
    CHECK(paramIds(info.params) ==
          (std::vector<std::string>{"threshold", "ratio", "below", "upward", "tilt", "knee", "range", "smooth",
                                    "focus_lo", "focus_hi", "attack", "release", "link", "mix", "output", "delta"}));
    struct Expected {
        const char* name;
        const char* unit;
        float min, max, def;
        bool log;
    };
    const std::vector<Expected> expected = {
        {"Threshold", "dB", -72.f, 12.f, -18.f, false}, {"Ratio", ":1", 1.f, 20.f, 2.f, true},
        {"Below", "dB", -72.f, 12.f, -48.f, false},     {"Upward", ":1", 1.f, 10.f, 1.f, true},
        {"Tilt", "dB/oct", -6.f, 6.f, 0.f, false},      {"Knee", "dB", 0.f, 24.f, 6.f, false},
        {"Range", "dB", 0.f, 48.f, 24.f, false},        {"Smoothing", "%", 0.f, 100.f, 40.f, false},
        {"Focus Low", "Hz", 20.f, 20000.f, 20.f, true}, {"Focus High", "Hz", 20.f, 20000.f, 20000.f, true},
        {"Attack", "ms", 1.f, 1000.f, 20.f, true},      {"Release", "ms", 10.f, 5000.f, 150.f, true},
        {"Stereo Link", "%", 0.f, 100.f, 100.f, false}, {"Dry/Wet", "%", 0.f, 100.f, 100.f, false},
        {"Output", "dB", -24.f, 24.f, 0.f, false},      {"Delta", "", 0.f, 1.f, 0.f, false},
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
        CHECK_EQ(p.isLog(), expected[i].log);
        CHECK(p.automatable);
        CHECK(!p.hidden);
        CHECK_EQ(p.stepCount(), p.id == "delta" ? 1 : 0);
    }
    CHECK(info.params[15].valueLabels == (std::vector<std::string>{"Off", "On"}));

    Spectral s;
    CHECK(s.processor().hasSidechain());
    std::vector<std::pair<std::string, int>> displays;
    for (const auto& d : s.processor().displays()) displays.emplace_back(d.id, d.samplesPerValue);
    CHECK(displays ==
          (std::vector<std::pair<std::string, int>>{
              {"input", 4}, {"key", 4}, {"output", 4}, {"gain", 4}, {"in_level", 512}, {"out_level", 512}}));
}

TEST_CASE("spectral: the defaults are gentle") {
    // Inserted as it comes, it takes a few dB off a dense mix, mostly where tones stand out, and leaves a spectrum
    // as dense as pink noise at a mix's level nearly alone: pink noise at -20 dBFS reads -20 dB at every
    // frequency, under the default Threshold of -18 dB.
    const auto down = [](double level) {
        const Samples x = pinkNoise(3 * kSampleRate, level, 107);
        Spectral s;
        const Samples out = s.play(x);
        return rmsDb(x, kSampleRate - kL, 3 * kSampleRate - kL) - rmsDb(out, kSampleRate, 3 * kSampleRate);
    };
    const double at20 = down(-20.0), at14 = down(-14.0);
    INFO("pink at -20 dBFS down " + std::to_string(at20) + " dB, at -14 " + std::to_string(at14));
    CHECK(at20 < 1.0);
    CHECK(at14 > 1.0);
    CHECK(at14 < 3.5);
}

TEST_CASE("spectral: design maths") {
    for (const auto& [rate, frame] : std::vector<std::pair<double, int>>{{44100.0, 2048},
                                                                         {48000.0, 2048},
                                                                         {88200.0, 4096},
                                                                         {96000.0, 4096},
                                                                         {176400.0, 8192},
                                                                         {192000.0, 8192},
                                                                         {32000.0, 1024},
                                                                         {22050.0, 1024},
                                                                         {16000.0, 512},
                                                                         {8000.0, 256}}) {
        INFO(std::to_string(rate));
        CHECK_EQ(spectral::frameSize(rate), frame);
    }
    CHECK_NEAR(spectral::calibrationDb(48000.0, 2048), -34.26, 0.01);
    // A full-scale sine at a bin's centre (|X| = N/4) reads +19.93 dB at 1 kHz.
    CHECK_NEAR(20.0 * std::log10(2048.0 / 4.0) + spectral::calibrationDb(48000.0, 2048), 19.93, 0.01);
    CHECK_NEAR(spectral::pinkDb(2000.0), 3.0103, 1e-4);
    CHECK_EQ(spectral::pinkDb(5.0), spectral::pinkDb(20.0));  // held at 20 Hz
    CHECK_NEAR(spectral::smoothingOctaves(40.0), 0.129, 0.001);
    CHECK_EQ(spectral::smoothingOctaves(0.0), 0.0);
    CHECK_NEAR(spectral::smoothingOctaves(100.0), 2.0, 1e-12);
    CHECK_NEAR(spectral::displayFrequency(0), 20.0, 1e-9);
    CHECK_NEAR(spectral::displayFrequency(kPoints - 1), 20000.0, 1e-6);

    // The knee: nothing below it, continuous at both ends, `over` above it.
    for (const double knee : {0.0, 6.0, 24.0}) {
        INFO(std::to_string(knee));
        CHECK_EQ(spectral::kneed(-knee / 2 - 0.1, knee), 0.0);
        CHECK_NEAR(spectral::kneed(-knee / 2 + 1e-12, knee), 0.0, 1e-12);
        CHECK_NEAR(spectral::kneed(knee / 2 - 1e-12, knee), knee / 2, 1e-11);
        CHECK_EQ(spectral::kneed(knee / 2 + 3.0, knee), knee / 2 + 3.0);
    }
    const double down4 = spectral::downSlope(4.0), up2 = spectral::upSlope(2.0);
    CHECK_NEAR(spectral::gainDb(-20.0, -30.0, -90.0, down4, 0.0, 0.0, 48.0), -7.5, 1e-12);
    CHECK_NEAR(spectral::gainDb(-30.0, -30.0, -90.0, down4, 0.0, 12.0, 48.0), down4 * 36.0 / 24.0, 1e-12);
    CHECK_NEAR(spectral::gainDb(-60.0, -30.0, -40.0, 0.0, up2, 0.0, 48.0), 10.0, 1e-12);
    CHECK_NEAR(spectral::gainDb(-100.0, -30.0, -80.0, 0.0, up2, 0.0, 48.0), 0.0, 1e-12);  // under the upward floor
    CHECK_NEAR(spectral::gainDb(-80.0, -30.0, -60.0, 0.0, up2, 0.0, 48.0), 5.0, 1e-12);   // halfway up its fade
    CHECK_NEAR(spectral::gainDb(10.0, -30.0, -90.0, spectral::downSlope(20.0), 0.0, 0.0, 12.0), -12.0, 1e-12);
    CHECK_NEAR(spectral::gainDb(-60.0, -10.0, -10.0, 0.0, spectral::upSlope(10.0), 0.0, 6.0), 6.0, 1e-12);
    // The static curve out = level + gain rises everywhere.
    Rng rng(5);
    for (int setting = 0; setting < 50; ++setting) {
        const double threshold = rng.uniform(-72.0, 12.0);
        const double below = std::min(threshold, rng.uniform(-72.0, 12.0));
        const double down = spectral::downSlope(rng.uniform(1.0, 20.0)), up = spectral::upSlope(rng.uniform(1.0, 10.0));
        const double knee = rng.uniform(0.0, 24.0), range = rng.uniform(0.0, 48.0);
        double last = -1e300;
        for (double level = -150.0; level <= 30.0; level += 0.05) {
            const double out = level + spectral::gainDb(level, threshold, below, down, up, knee, range);
            CHECK(out >= last - 1e-9);
            last = out;
        }
    }
    CHECK_EQ(spectral::belowDb(-30.0, -10.0, 0.0, 500.0), -30.0);  // held at the threshold
    CHECK_NEAR(spectral::belowDb(-30.0, -50.0, 3.0, 2000.0), -47.0, 1e-12);
    CHECK_NEAR(spectral::thresholdDb(-30.0, 3.0, 4000.0), -24.0, 1e-12);
    // The Focus: all of it inside, half a sixth of an octave out, none from a third out.
    CHECK_NEAR(spectral::focusWeight(1000.0, 4000.0, 1000.0), 1.0, 1e-12);
    CHECK_NEAR(spectral::focusWeight(1000.0, 4000.0, 4000.0), 1.0, 1e-12);
    CHECK_NEAR(spectral::focusWeight(1000.0, 4000.0, 1000.0 / std::pow(2.0, 1.0 / 6.0)), 0.5, 1e-9);
    CHECK_NEAR(spectral::focusWeight(1000.0, 4000.0, 4000.0 * std::pow(2.0, 1.0 / 6.0)), 0.5, 1e-9);
    CHECK_NEAR(spectral::focusWeight(1000.0, 4000.0, 1000.0 / std::pow(2.0, 1.0 / 3.0)), 0.0, 1e-9);
    CHECK_EQ(spectral::focusWeight(1000.0, 4000.0, 200.0), 0.0);
    CHECK_EQ(spectral::focusWeight(1000.0, 4000.0, 10000.0), 0.0);
    for (const double f : {20.0, 100.0, 1000.0, 20000.0})
        CHECK_NEAR(spectral::focusWeight(20.0, 20000.0, f), 1.0, 1e-12);
    for (const double f : {15.0, 10.0, 0.0}) CHECK_EQ(spectral::focusWeight(20.0, 20000.0, f), 0.0);
}

TEST_CASE("spectral: latency and the impulse") {
    for (const double rate :
         {8000.0, 16000.0, 22050.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0}) {
        INFO(std::to_string(rate));
        const int frame = spectral::frameSize(rate);
        Spectral s({}, rate);
        CHECK_EQ(s.processor().latencySamples(), frame + frame / 4);
        CHECK_EQ(s.processor().latencySamples(), spectral::latencySamples(rate));
        CHECK_EQ(s.processor().tailSamples(), frame);
    }
    CHECK_EQ(spectral::latencySamples(48000.0), kL);
    for (const double rate : {48000.0, 96000.0}) {
        INFO(std::to_string(rate));
        Spectral s({{"threshold", 12.f}}, rate);
        const int latency = s.processor().latencySamples();
        Samples x(static_cast<size_t>(100 + 3 * latency), 0.f);
        x[100] = 1.f;
        const Samples out = s.play(x);
        CHECK_NEAR(out[static_cast<size_t>(100 + latency)], 1.0, 1e-5);
        Samples rest = out;
        rest[static_cast<size_t>(100 + latency)] = 0.f;
        CHECK(maxAbs(rest) < 1e-5);
    }
    // A new rate: the device starts from silence at its new frame.
    Spectral s({{"threshold", 12.f}});
    s.play(whiteNoise(20000, -10.0, 1));
    s.processor().prepare(96000.0, kBlock);
    CHECK_EQ(s.processor().latencySamples(), 4096 + 1024);
    Samples x(16384, 0.f);
    x[10] = 1.f;
    const Samples out = s.play(x);
    CHECK_EQ(argmax(out), size_t{10 + 5120});
    CHECK_NEAR(out[10 + 5120], 1.0, 1e-5);
}

TEST_CASE("spectral: transparent below the threshold") {
    const Samples left = pinkNoise(2 * kSampleRate, -20.0, 1), right = pinkNoise(2 * kSampleRate, -20.0, 2);
    for (const Values& values :
         {Values{{"threshold", 12.f}}, Values{{"range", 0.f}, {"threshold", -72.f}, {"ratio", 20.f}}}) {
        INFO(values[0].first);
        Spectral s(values);
        const auto [l, r] = s.play(left, right);
        CHECK_ALLCLOSE(slice(l, kL), slice(left, 0, -kL), 0.0, 2e-5);
        CHECK_ALLCLOSE(slice(r, kL), slice(right, 0, -kL), 0.0, 2e-5);
    }
}

TEST_CASE("spectral: dry is the input delayed, bit for bit") {
    const Samples left = pinkNoise(kSampleRate, -6.0, 3), right = whiteNoise(kSampleRate, -10.0, 4);
    Spectral s({{"mix", 0.f}, {"threshold", -72.f}, {"ratio", 20.f}, {"upward", 10.f}, {"below", 12.f}});
    const auto [l, r] = s.play(left, right);
    CHECK_ARRAY_EQUAL(l, delayed(left, kL));
    CHECK_ARRAY_EQUAL(r, delayed(right, kL));
    CHECK(allEqual(slice(l, 0, kL), 0.0));
}

TEST_CASE("spectral: pink noise reads its level") {
    for (const double rate : {48000.0, 44100.0, 96000.0}) {
        INFO(std::to_string(rate));
        const auto frames = static_cast<size_t>(2.5 * rate);
        const Samples x = pinkNoise(frames, -20.0, 7, rate);
        Spectral s(steady({{"threshold", 12.f}}), rate);
        s.play(x, x);
        const auto input = s.frames(kInput), key = s.frames(kKey);
        const auto [from, to] = framesBetween(1.0, 2.5, input.size(), rate, s.frame());
        REQUIRE(to > from + 50);
        double lowest = 1e9, highest = -1e9, keyLowest = 1e9, keyHighest = -1e9;
        for (int j = pointAt(100.0); j <= pointAt(10000.0); ++j) {
            const double in = meanPowerDb(input, from, to, j), k = meanPowerDb(key, from, to, j);
            lowest = std::min(lowest, in);
            highest = std::max(highest, in);
            keyLowest = std::min(keyLowest, k);
            keyHighest = std::max(keyHighest, k);
        }
        INFO("input " + std::to_string(lowest) + ".." + std::to_string(highest) + ", key " + std::to_string(keyLowest) +
             ".." + std::to_string(keyHighest));
        CHECK(lowest > -21.5);
        CHECK(highest < -18.5);
        CHECK(keyLowest > -21.5);
        CHECK(keyHighest < -18.5);
    }
}

TEST_CASE("spectral: downward") {
    const Samples x = pinkNoise(3 * kSampleRate, -20.0, 11);
    Spectral s(steady({{"threshold", -30.f}, {"ratio", 4.f}}));
    const Samples out = s.play(x);
    const auto gain = s.frames(kGain), input = s.frames(kInput), output = s.frames(kOutput);
    const auto [from, to] = framesBetween(1.0, 3.0, gain.size());
    double sum = 0.0;
    int count = 0;
    for (int j = pointAt(1000.0); j <= pointAt(5000.0); ++j, ++count) sum += meanDb(gain, from, to, j);
    const double average = sum / count;
    INFO("gain 1-5 kHz " + std::to_string(average));
    CHECK_NEAR(average, -7.25, 0.5);
    const double down = rmsDb(x, kSampleRate - kL, 3 * kSampleRate - kL) - rmsDb(out, kSampleRate, 3 * kSampleRate);
    INFO("down by " + std::to_string(down));
    CHECK_NEAR(down, 6.8, 0.6);
    // What comes out is what went in, turned down by the gains drawn.
    for (int j = pointAt(200.0); j <= pointAt(10000.0); ++j) {
        INFO(std::to_string(spectral::displayFrequency(j)));
        CHECK_NEAR(meanDb(output, from, to, j), meanDb(input, from, to, j) + meanDb(gain, from, to, j), 1.0);
    }
}

TEST_CASE("spectral: range caps it") {
    const Samples x = pinkNoise(3 * kSampleRate, -20.0, 11);
    Spectral s(steady({{"threshold", -30.f}, {"ratio", 4.f}, {"range", 3.f}}));
    const Samples out = s.play(x);
    const double down = rmsDb(x, kSampleRate - kL, 3 * kSampleRate - kL) - rmsDb(out, kSampleRate, 3 * kSampleRate);
    INFO("down by " + std::to_string(down));
    CHECK_NEAR(down, 3.0, 0.2);
    CHECK(*std::min_element(s.shown(kGain).begin(), s.shown(kGain).end()) >= -3.0001f);
}

TEST_CASE("spectral: upward under Below") {
    const Samples quiet = pinkNoise(3 * kSampleRate, -50.0, 13);
    const auto outDb = [](const Samples& out) { return rmsDb(out, kSampleRate, 3 * kSampleRate); };
    const double in = rmsDb(quiet, kSampleRate - kL, 3 * kSampleRate - kL);
    Spectral a(steady({{"threshold", -30.f}, {"ratio", 1.f}, {"upward", 2.f}, {"below", -30.f}}));
    const Samples lifted = a.play(quiet);
    INFO("Below -30: " + std::to_string(outDb(lifted)));
    CHECK_NEAR(outDb(lifted), -40.0, 1.0);
    // Below over the threshold: the threshold is used.
    Spectral b(steady({{"threshold", -30.f}, {"ratio", 1.f}, {"upward", 2.f}, {"below", 0.f}}));
    CHECK_ARRAY_EQUAL(b.play(quiet), lifted);
    // Below under the level: nothing.
    Spectral c(steady({{"threshold", -30.f}, {"ratio", 1.f}, {"upward", 2.f}, {"below", -60.f}}));
    CHECK_NEAR(outDb(c.play(quiet)), in, 0.3);
    // Both ways: lifted towards Below, never over the threshold.
    Spectral d(steady({{"threshold", -30.f}, {"ratio", 4.f}, {"upward", 2.f}, {"below", -40.f}}));
    const double both = outDb(d.play(quiet));
    INFO("Below -40, ratio 4: " + std::to_string(both));
    CHECK_NEAR(both, -45.0, 0.6);
    // Under the upward floor (-90 dB) nothing is lifted; silence stays exact zeros.
    const Samples faint = pinkNoise(3 * kSampleRate, -110.0, 14);
    Spectral e(steady({{"threshold", -30.f}, {"ratio", 1.f}, {"upward", 2.f}, {"below", -30.f}}));
    CHECK_NEAR(outDb(e.play(faint)), rmsDb(faint, kSampleRate - kL, 3 * kSampleRate - kL), 0.5);
    Spectral f(steady({{"threshold", -30.f}, {"ratio", 1.f}, {"upward", 10.f}, {"below", 12.f}}));
    CHECK(allEqual(f.play(Samples(kSampleRate, 0.f)), 0.0));
}

TEST_CASE("spectral: tilt follows a spectrum") {
    // White noise at -30 dBFS reads -35.4 dB at 1 kHz, 3.01 dB more each octave up.
    const Samples x = whiteNoise(3 * kSampleRate, -30.0, 17);
    const auto gainAt = [](Spectral& s, double hz) {
        const auto gain = s.frames(kGain);
        const auto [from, to] = framesBetween(1.0, 3.0, gain.size());
        return meanDb(gain, from, to, pointAt(hz));
    };
    Spectral tilted(steady({{"threshold", -45.4f}, {"ratio", 2.f}, {"tilt", 3.f}}));
    tilted.play(x);
    double lowest = 1e9, highest = -1e9;
    for (int j = pointAt(200.0); j <= pointAt(10000.0); ++j) {
        const double g = gainAt(tilted, spectral::displayFrequency(j));
        INFO(std::to_string(spectral::displayFrequency(j)) + " Hz: " + std::to_string(g));
        CHECK_NEAR(g, -5.0, 0.8);
        lowest = std::min(lowest, g);
        highest = std::max(highest, g);
    }
    CHECK(highest - lowest < 1.5);
    Spectral flat(steady({{"threshold", -45.4f}, {"ratio", 2.f}, {"tilt", 0.f}}));
    flat.play(x);
    CHECK(gainAt(flat, 100.0) > -1.0);
    CHECK_NEAR(gainAt(flat, 10000.0), -10.0, 1.0);
    // The tilt turns Below too.
    Spectral up(steady({{"threshold", 12.f}, {"ratio", 1.f}, {"upward", 2.f}, {"below", -25.4f}, {"tilt", 3.f}}));
    up.play(x);
    for (int j = pointAt(200.0); j <= pointAt(10000.0); ++j) {
        INFO(std::to_string(spectral::displayFrequency(j)) + " Hz");
        CHECK_NEAR(gainAt(up, spectral::displayFrequency(j)), 5.0, 0.8);
    }
}

TEST_CASE("spectral: smoothing spreads the gain") {
    Samples x = whiteNoise(3 * kSampleRate, -60.0, 19);
    const Samples tone = sine(1000.0, 3.0, 0.5);
    for (size_t i = 0; i < x.size(); ++i) x[i] += tone[i];
    const auto gainAt = [](Spectral& s, double hz) {
        const auto gain = s.frames(kGain);
        const auto [from, to] = framesBetween(1.0, 3.0, gain.size());
        return meanDb(gain, from, to, pointAt(hz));
    };
    Spectral sharp(steady({{"threshold", -20.f}, {"ratio", 4.f}, {"smooth", 0.f}}));
    sharp.play(x);
    INFO("smoothing 0: " + std::to_string(gainAt(sharp, 707.0)) + " / " + std::to_string(gainAt(sharp, 1000.0)) +
         " / " + std::to_string(gainAt(sharp, 1414.0)));
    CHECK(gainAt(sharp, 1000.0) <= -6.0);
    CHECK(gainAt(sharp, 707.0) >= -0.5);
    CHECK(gainAt(sharp, 1414.0) >= -0.5);
    Spectral wide(steady({{"threshold", -20.f}, {"ratio", 4.f}, {"smooth", 100.f}}));
    wide.play(x);
    INFO("smoothing 100: " + std::to_string(gainAt(wide, 707.0)) + " / " + std::to_string(gainAt(wide, 1414.0)));
    CHECK(gainAt(wide, 707.0) <= -3.0);
    CHECK(gainAt(wide, 1414.0) <= -3.0);
}

TEST_CASE("spectral: the Focus band") {
    const Samples x = pinkNoise(3 * kSampleRate, -20.0, 11);
    Spectral s(steady({{"threshold", -30.f}, {"ratio", 4.f}, {"focus_lo", 1000.f}, {"focus_hi", 4000.f}}));
    const Samples out = s.play(x);
    const auto gain = s.frames(kGain);
    const auto [from, to] = framesBetween(1.0, 3.0, gain.size());
    for (int j = 0; j < kPoints; ++j) {
        const double f = spectral::displayFrequency(j), g = meanDb(gain, from, to, j);
        INFO(std::to_string(f) + " Hz: " + std::to_string(g));
        if (f < 700.0 || f > 5500.0) CHECK(g > -0.1);
        if (f >= 1100.0 && f <= 3600.0) CHECK_NEAR(g, -7.25, 0.5);
    }
    for (const double f : {900.0, 4600.0}) {
        const double g = meanDb(gain, from, to, pointAt(f));
        INFO(std::to_string(f) + " Hz: " + std::to_string(g));
        CHECK(g < -2.5);
        CHECK(g > -6.0);
    }
    const double down = rmsDb(x, kSampleRate - kL, 3 * kSampleRate - kL) - rmsDb(out, kSampleRate, 3 * kSampleRate);
    INFO("down by " + std::to_string(down));
    CHECK(down > 0.6);
    CHECK(down < 1.3);
    // Crossed edges: nothing is processed.
    Spectral crossed(steady({{"threshold", -30.f}, {"ratio", 4.f}, {"focus_lo", 8000.f}, {"focus_hi", 1000.f}}));
    const Samples same = crossed.play(x);
    CHECK(*std::min_element(crossed.shown(kGain).begin(), crossed.shown(kGain).end()) > -0.01f);
    CHECK_ALLCLOSE(slice(same, kL), slice(x, 0, -kL), 0.0, 2e-5);
}

TEST_CASE("spectral: Delta is the difference") {
    const Samples left = pinkNoise(3 * kSampleRate, -20.0, 11), right = pinkNoise(3 * kSampleRate, -20.0, 12);
    const Values setup = steady({{"threshold", -30.f}, {"ratio", 4.f}});
    Spectral plain(setup), delta(steady({{"threshold", -30.f}, {"ratio", 4.f}, {"delta", 1.f}}));
    const auto [l0, r0] = plain.play(left, right);
    const auto [l1, r1] = delta.play(left, right);
    const Samples dl = delayed(left, kL), dr = delayed(right, kL);
    Samples wantL(dl.size()), wantR(dr.size());
    for (size_t i = 0; i < dl.size(); ++i) {
        wantL[i] = dl[i] - l0[i];
        wantR[i] = dr[i] - r0[i];
    }
    CHECK_ALLCLOSE(l1, wantL, 0.0, 1e-6);
    CHECK_ALLCLOSE(r1, wantR, 0.0, 1e-6);
    const double level = rmsDb(l1, kSampleRate, 3 * kSampleRate);
    INFO("Delta's level " + std::to_string(level));
    CHECK_NEAR(level, -25.2, 1.0);

    // At Dry/Wet 50 %: half the difference.
    Spectral half(steady({{"threshold", -30.f}, {"ratio", 4.f}, {"delta", 1.f}, {"mix", 50.f}}));
    const Samples h = half.play(left, right).first;
    Samples wantHalf(dl.size());
    for (size_t i = 0; i < dl.size(); ++i) wantHalf[i] = 0.5f * (dl[i] - l0[i]);
    CHECK_ALLCLOSE(h, wantHalf, 0.0, 1e-6);

    // Nothing over the threshold: nothing to hear.
    Spectral none(steady({{"threshold", 12.f}, {"delta", 1.f}}));
    CHECK(maxAbs(none.play(left)) < 2e-5);
}

TEST_CASE("spectral: attack and release") {
    // Pink noise stepping from -40 to -10 dBFS at 1 s and back at 3 s, over a threshold of -30 dB at Ratio 2. The
    // envelopes follow each bin's magnitude (as the Compressor's follower does), so after the step up the level
    // stands 20 log10(1 - (1 - a) e^(-t/attack)) dB under where it settles (a = 10^(-30/20), the step), and after
    // the step down 20 log10(a + (1 - a) e^(-t/release)) dB under it: a release time constant takes 8.7 dB off a
    // fall far below (a follower of power would take 4.3). The cut, half the level's excess, is measured where
    // the level is 12 dB under its held value: 0.257 attack times after the step up (a follower of power: 0.064)
    // and 1.48 release times after the step down (a follower of power: 2.78). A frame straddling the step
    // already reads much of the new level (the square root of its share of the window's energy), so the rise
    // comes about 9 ms early at 500 ms (a frame-by-frame model of it without noise: 119.6 ms, not 128.6), and
    // with a 20 ms attack a little before the step itself.
    Samples x = pinkNoise(5 * kSampleRate, -10.0, 23);
    for (size_t i = 0; i < x.size(); ++i)
        if (i < size_t{kSampleRate} || i >= size_t{3 * kSampleRate}) x[i] *= 0.031622777f;
    const double step = std::pow(10.0, -30.0 / 20.0), under = std::pow(10.0, -12.0 / 20.0);
    const double riseShare = -std::log((1.0 - under) / (1.0 - step));  // attack times
    const double fallShare = -std::log((under - step) / (1.0 - step));  // release times
    struct Times {
        double rise, fall, held;
    };
    const auto measure = [&](float attack, float release) {
        Spectral s(steady({{"threshold", -30.f}, {"ratio", 2.f}, {"attack", attack}, {"release", release}}));
        s.play(x);
        const auto gain = s.frames(kGain);
        std::vector<double> cut(gain.size(), 0.0);
        int count = 0;
        for (int j = pointAt(2000.0); j <= pointAt(4000.0); ++j, ++count)
            for (size_t i = 0; i < gain.size(); ++i) cut[i] -= gain[i][static_cast<size_t>(j)];
        for (double& c : cut) c /= count;
        // When the cut first crosses `level` (rising or falling) after `after` s, interpolated between frames.
        const auto crossing = [&](double after, double level, bool rising) {
            for (size_t i = 1; i < cut.size(); ++i) {
                const double t = static_cast<double>(frameCentre(i, kN)) / kSampleRate;
                if (t < after) continue;
                const bool crossed = rising ? cut[i] >= level : cut[i] <= level;
                if (!crossed) continue;
                const double t0 = static_cast<double>(frameCentre(i - 1, kN)) / kSampleRate;
                const double share = (level - cut[i - 1]) / (cut[i] - cut[i - 1]);
                return t0 + share * (t - t0) - after;
            }
            return 1e9;
        };
        const auto [from, to] = framesBetween(2.5, 3.0, cut.size());
        double held = 0.0;
        for (size_t i = from; i < to; ++i) held += cut[i] / static_cast<double>(to - from);
        // 12 dB of level under its held value is 6 dB of cut under the held cut (Ratio 2).
        return Times{crossing(1.0, held - 6.0, true), crossing(3.0, held - 6.0, false), held};
    };
    const Times slow = measure(500.f, 200.f);
    INFO("rise " + std::to_string(slow.rise) + " s (" + std::to_string(riseShare * 0.5) + "), fall " +
         std::to_string(slow.fall) + " s (" + std::to_string(fallShare * 0.2) + "), held " +
         std::to_string(slow.held) + " dB");
    CHECK_NEAR(slow.held, 9.7, 0.5);
    CHECK_NEAR(slow.rise, riseShare * 0.5 - 0.009, 0.02);  // 120 ms (a follower of power: 32)
    CHECK_NEAR(slow.fall, fallShare * 0.2, 0.02);          // 297 ms (a follower of power: 556)
    const Times fastAttack = measure(20.f, 200.f), fastRelease = measure(500.f, 50.f);
    INFO("attack 20 ms: rise " + std::to_string(fastAttack.rise) + " s; release 50 ms: fall " +
         std::to_string(fastRelease.fall) + " s");
    CHECK(fastAttack.rise < slow.rise - 0.08);
    CHECK_NEAR(fastRelease.fall, fallShare * 0.05, 0.02);
}

TEST_CASE("spectral: stereo link") {
    const Samples left = pinkNoise(3 * kSampleRate, -10.0, 29), right = pinkNoise(3 * kSampleRate, -40.0, 31);
    const auto down = [](const Samples& in, const Samples& out) {
        return rmsDb(in, kSampleRate - kL, 3 * kSampleRate - kL) - rmsDb(out, kSampleRate, 3 * kSampleRate);
    };
    const auto run = [&](float link) {
        Spectral s(steady({{"threshold", -30.f}, {"ratio", 4.f}, {"link", link}}));
        const auto [l, r] = s.play(left, right);
        return std::pair<double, double>{down(left, l), down(right, r)};
    };
    // The left reads -10.3 dB (the 2-octave box reads pink noise 0.34 dB low), 19.7 over the threshold: cut
    // 14.75 dB in the middle of the spectrum, less where the box reaches past 20 Hz, and least in the lowest bin,
    // whose gain the 3-tap smoothing pulls towards DC's (outside the Focus): 13.8 dB in all (the prototype's
    // 13.74).
    const auto [linkedL, linkedR] = run(100.f);
    INFO("linked " + std::to_string(linkedL) + " / " + std::to_string(linkedR));
    CHECK_NEAR(linkedL, 13.8, 0.6);
    CHECK_NEAR(linkedR, 13.8, 0.6);
    const auto [ownL, ownR] = run(0.f);
    INFO("each its own " + std::to_string(ownL) + " / " + std::to_string(ownR));
    CHECK_NEAR(ownL, 13.8, 0.6);
    CHECK_NEAR(ownR, 0.0, 0.3);
    // Halfway in dB: the right reads -25 dB, 5 over the threshold (3.5 dB in the middle, 3.2 in all).
    const auto [halfL, halfR] = run(50.f);
    INFO("half " + std::to_string(halfL) + " / " + std::to_string(halfR));
    CHECK_NEAR(halfL, 13.8, 0.6);
    CHECK_NEAR(halfR, 3.2, 0.5);
}

TEST_CASE("spectral: sidechain keys it") {
    const size_t length = static_cast<size_t>(2.5 * kSampleRate);
    const Samples main = pinkNoise(length, -30.0, 37);
    const Samples key = sine(100.0, 2.5, 0.5);
    const Samples quietKey(length, 0.f);
    constexpr size_t kWindow = 65536;
    const auto bands = [&](const Samples& in, const Samples& out) {
        const Samples a = slice(in, kSampleRate - kL, kSampleRate - kL + kWindow);
        const Samples b = slice(out, kSampleRate, kSampleRate + kWindow);
        return std::pair<double, double>{bandDb(a, 85.0, 115.0) - bandDb(b, 85.0, 115.0),
                                         bandDb(a, 1000.0, 10000.0) - bandDb(b, 1000.0, 10000.0)};
    };
    const Values setup = steady({{"threshold", -20.f}, {"ratio", 4.f}, {"smooth", 20.f}});
    {
        Spectral s(setup);
        const auto [l, r] = s.play(main, main, {}, Key{&key, &key, true});
        const auto [ducked, rest] = bands(main, l);
        INFO("85-115 Hz down " + std::to_string(ducked) + " dB, 1-10 kHz " + std::to_string(rest) + " dB");
        CHECK(ducked >= 10.0);
        CHECK(std::abs(rest) < 0.5);
        CHECK_ARRAY_EQUAL(l, r);
        // The levels compared peak at the key's 100 Hz.
        const auto shown = s.frames(kKey);
        const auto [from, to] = framesBetween(1.0, 2.5, shown.size());
        int peak = 0;
        for (int j = 1; j < kPoints; ++j)
            if (meanDb(shown, from, to, j) > meanDb(shown, from, to, peak)) peak = j;
        INFO("the key peaks at " + std::to_string(spectral::displayFrequency(peak)) + " Hz");
        CHECK(std::abs(std::log2(spectral::displayFrequency(peak) / 100.0)) < 0.15);
    }
    // One main channel: the louder key channel keys it.
    {
        Spectral s(setup);
        const Samples out = s.play(main, {}, Key{&quietKey, &key, true});
        CHECK(bands(main, out).first >= 10.0);
    }
    // Keyed by silence (a source chosen, nothing from it): nothing changes, even with Upward on.
    const Samples loud = pinkNoise(length, -6.0, 41);
    for (const Values& values :
         {setup, steady({{"threshold", -20.f}, {"ratio", 4.f}, {"upward", 4.f}, {"below", 12.f}})}) {
        Spectral s(values);
        const Samples out = s.play(loud, {}, Key{nullptr, nullptr, true});
        CHECK_ALLCLOSE(slice(out, kL), slice(loud, 0, -kL), 0.0, 2e-5);
    }
    // Not connected: its own input keys it.
    {
        Spectral s(setup);
        const Samples out = s.play(loud, {}, Key{&key, &key, false});
        CHECK(rmsDb(loud, kSampleRate - kL, 2 * kSampleRate - kL) - rmsDb(out, kSampleRate, 2 * kSampleRate) > 6.0);
    }
    // Through an engine: another track's 100 Hz keys it, heard only through the sidechain.
    sub::Engine engine;
    engine.setClipFadeMs(0);
    const uint32_t track = clipTrack(engine, makeWav(stereo(main), 2), 0.0, 2.5);
    const uint32_t keyTrack = clipTrack(engine, makeWav(stereo(key), 2), 0.0, 2.5);
    engine.setTrackGain(keyTrack, 0.f);
    const uint32_t device = engine.addBuiltinProcessor(engine.trackChain(track), "spectral", -1);
    for (const auto& [id, value] : setup) setParam(engine, device, id, value);
    engine.setProcessorSidechain(device, keyTrack, sub::SidechainTap::PreFader);
    const Samples keyed = channel(engine.renderOffline(0.0, static_cast<int64_t>(length)), 0);
    const auto [ducked, rest] = bands(main, keyed);
    INFO("through the engine: 85-115 Hz down " + std::to_string(ducked) + " dB, 1-10 kHz " + std::to_string(rest));
    CHECK(ducked >= 10.0);
    CHECK(std::abs(rest) < 0.5);
}

TEST_CASE("spectral: no clicks") {
    // A 220 Hz tone (nothing else: noise would swamp the measure), every control jumping back and forth every
    // 0.25 s. Those that act through the frames crossfade over a frame; the others ramp over 20 ms.
    const Samples tone = smoothSine(220.0, 2.5, 0.25);
    Samples half = tone;
    for (float& v : half) v *= 0.5f;
    const auto s = [](double seconds) { return static_cast<int64_t>(seconds * kSampleRate); };
    struct Jumps {
        std::string id;
        float a, b;
        Values with;
    };
    const std::vector<Jumps> framed = {
        {"threshold", -60.f, 0.f, {}},
        {"ratio", 1.f, 20.f, {}},
        {"upward", 1.f, 10.f, {{"below", 12.f}}},
        {"below", -72.f, 12.f, {{"upward", 10.f}}},
        {"tilt", -6.f, 6.f, {}},
        {"knee", 0.f, 24.f, {}},
        {"range", 0.f, 48.f, {}},
        {"smooth", 0.f, 100.f, {}},
        {"focus_lo", 20.f, 5000.f, {}},
        {"focus_hi", 20000.f, 100.f, {}},
        {"link", 0.f, 100.f, {}},
        {"attack", 1.f, 1000.f, {}},
        {"release", 10.f, 5000.f, {}},
    };
    for (const Jumps& jumps : framed) {
        INFO(jumps.id);
        Values values = steady({{"threshold", -40.f}, {"ratio", 4.f}});
        values.insert(values.end(), jumps.with.begin(), jumps.with.end());
        values.emplace_back(jumps.id, jumps.a);
        std::vector<Change> changes;
        for (int k = 0; k < 8; ++k) changes.push_back({s(0.4 + 0.25 * k), jumps.id, k % 2 ? jumps.a : jumps.b});
        Spectral d(values);
        const auto [l, r] = d.play(tone, half, changes);
        CHECK(allFinite(l) && allFinite(r));
        const double worst = std::max(clickiness(l, s(0.2)), clickiness(r, s(0.2)));
        INFO("clickiness " + std::to_string(worst));
        CHECK(worst < 2e-4);  // (a step of 1e-5 would show as up to 2e-4; the frames' rounding about 4e-6)
    }

    // Dry/Wet, Output and Delta ramp: against the same change switched at once (two renders spliced), where
    // the change is heard (with the input it came with, the latency on), at a moment the tone is at its peak.
    struct Ramped {
        std::string id;
        float a, b;
    };
    for (const Ramped& ramped : {Ramped{"mix", 0.f, 100.f}, Ramped{"output", -24.f, 24.f}, Ramped{"delta", 0.f, 1.f}}) {
        INFO(ramped.id);
        const Values base = steady({{"threshold", -40.f}, {"ratio", 4.f}});
        // Near 1 s, a quarter period past a whole number of periods: the tone's peak.
        const double period = kSampleRate / 220.0;
        const auto at = static_cast<int64_t>(std::round(std::round(kSampleRate / period) * period + period / 4));
        const int64_t heard = at + kL;
        Values first = base, second = base;
        first.emplace_back(ramped.id, ramped.a);
        second.emplace_back(ramped.id, ramped.b);
        Spectral d(first), a(first), b(second);
        const Samples out = d.play(tone, {{at, ramped.id, ramped.b}});
        Samples spliced = a.play(tone);
        const Samples after = b.play(tone);
        CHECK_ARRAY_EQUAL(slice(out, 0, heard), slice(spliced, 0, heard));  // nothing changes before it is heard
        std::copy(after.begin() + heard, after.end(), spliced.begin() + heard);
        const double smooth = clickiness(out, heard - 64, heard + s(0.05));
        const double hard = clickiness(spliced, heard - 64, heard + s(0.05));
        INFO("ramped " + std::to_string(smooth) + ", switched " + std::to_string(hard));
        CHECK(smooth * 50.0 < hard);
    }
}

TEST_CASE("spectral: automation to the sample") {
    // Output turned down at input sample 30000: the output of that sample (the latency on) is the ramp's first
    // step.
    const Samples x = pinkNoise(kSampleRate, -20.0, 43);
    const Values setup = {{"threshold", -30.f}};
    Spectral plain(setup), automated(setup), quiet({{"threshold", -30.f}, {"output", -24.f}});
    const Samples a = plain.play(x);
    const Samples b = automated.play(x, {{30000, "output", -24.f}});
    const Samples c = quiet.play(x);
    constexpr int64_t kHeard = 30000 + kL;
    CHECK_ARRAY_EQUAL(slice(b, 0, kHeard), slice(a, 0, kHeard));
    CHECK(b[kHeard] != a[kHeard]);
    CHECK(b[kHeard + 958] != c[kHeard + 958]);                          // still on its way
    CHECK_ARRAY_EQUAL(slice(b, kHeard + 960), slice(c, kHeard + 960));  // there after 20 ms

    // Threshold: each frame takes the parameters from when its centre went in, so the change crossfades in
    // about the sample it came at (a frame reaches N/2 either side of its centre).
    Spectral deeper(setup);
    const Samples d = deeper.play(x, {{30000, "threshold", -60.f}});
    int64_t first = -1;
    for (size_t i = 0; i < d.size() && first < 0; ++i)
        if (d[i] != a[i]) first = static_cast<int64_t>(i) - kL;  // in input time
    CHECK(first >= 30000 - kN / 2);
    CHECK(first <= 30000);
}

TEST_CASE("spectral: automation through the engine") {
    sub::Engine engine;
    engine.setClipFadeMs(0);
    const Samples x = pinkNoise(3 * kSampleRate, -12.0, 101);
    const uint32_t track = clipTrack(engine, makeWav(stereo(x), 2), 0.0, 3.0);
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "spectral", -1);
    setParam(engine, id, "threshold", -30.f);
    const Samples untouched = engine.renderOffline(0.0, 3 * kSampleRate);
    using Points = std::vector<sub::AutomationPoint>;
    const auto firstChange = [&](const Samples& out) {
        for (size_t i = 0; i < out.size(); ++i)
            if (out[i] != untouched[i]) return static_cast<int64_t>(i) / 2;
        return int64_t{-1};
    };
    // Output steps from 0 to -24 dB at beat 2 (1 s): the ramp starts there, to the sample, and lands 20 ms on.
    CHECK_EQ(paramInfo(engine, id, "output").fromNormalized(0.5f), 0.f);
    engine.setTrackAutomation(track, {{id, "output", Points{{0.0, 0.5f, 0.f}, {2.0, 0.5f, 0.f}, {2.0, 0.f, 0.f}}}});
    const Samples quieter = engine.renderOffline(0.0, 3 * kSampleRate);
    CHECK_EQ(firstChange(quieter), int64_t{kSampleRate});
    const auto down = static_cast<float>(std::pow(10.0, -24.0 / 20.0));
    for (int64_t i = kSampleRate + 960; i < 3 * kSampleRate; i += 97) {
        CHECK_NEAR(at(quieter, i, 0), at(untouched, i, 0) * down, 1e-6);
    }
    // Threshold steps at 1 s: the frames centred after it act on it (a frame reaches N/2 either side).
    setParam(engine, id, "output", 0.f);  // (automation left it where it last took it)
    CHECK_EQ(paramInfo(engine, id, "threshold").fromNormalized(0.5f), -30.f);
    engine.setTrackAutomation(track, {{id, "threshold", Points{{0.0, 0.5f, 0.f}, {2.0, 0.5f, 0.f}, {2.0, 0.f, 0.f}}}});
    const Samples deeper = engine.renderOffline(0.0, 3 * kSampleRate);
    const int64_t first = firstChange(deeper);
    CHECK(first >= kSampleRate - kN / 2);
    CHECK(first <= kSampleRate);
    CHECK(rmsDb(channel(deeper, 0), 2 * kSampleRate, 3 * kSampleRate) <
          rmsDb(channel(untouched, 0), 2 * kSampleRate, 3 * kSampleRate) - 3.0);
}

TEST_CASE("spectral: reset") {
    const Samples burst = pinkNoise(kSampleRate / 2, -6.0, 47);
    Samples silenceThenBurst(4 * kN, 0.f);
    silenceThenBurst.insert(silenceThenBurst.end(), burst.begin(), burst.end());
    Spectral fresh({{"threshold", -40.f}, {"upward", 3.f}});
    const Samples want = fresh.play(silenceThenBurst);
    Spectral s({{"threshold", -40.f}, {"upward", 3.f}});
    s.play(pinkNoise(kSampleRate, 0.0, 53));
    s.processor().reset();
    const Samples out = s.play(silenceThenBurst);
    CHECK(allEqual(slice(out, 0, 4 * kN), 0.0));
    CHECK_ARRAY_EQUAL(out, want);

    // What is set after a reset, before anything plays (as a project loading sets it), holds from the start:
    // nothing ramps in from what was there before.
    Spectral later({{"threshold", -40.f}, {"upward", 3.f}});
    for (const auto& [id, value] : Values{{"threshold", -50.f}, {"output", -12.f}, {"mix", 50.f}, {"delta", 1.f}})
        later.set(id, value);
    Spectral given({{"threshold", -50.f}, {"upward", 3.f}, {"output", -12.f}, {"mix", 50.f}, {"delta", 1.f}});
    CHECK_ARRAY_EQUAL(later.play(burst), given.play(burst));
}

TEST_CASE("spectral: silence rings out to exact zeros") {
    for (const Values& values : {Values{}, Values{{"upward", 10.f}, {"below", 12.f}}}) {
        INFO(values.empty() ? "defaults" : "lifting");
        Samples x = whiteNoise(kSampleRate, -6.0, 59);
        x.resize(11 * kSampleRate, 0.f);
        Spectral s(values);
        const auto [l, r] = s.play(x, x);
        CHECK(allFinite(l));
        // The last sound comes out the latency on, and the last frames to hear it end a frame after that.
        CHECK(anyNonzero(slice(l, kSampleRate + kL, kSampleRate + kL + kN)));
        CHECK(allEqual(slice(l, kSampleRate + kL + kN), 0.0));
        CHECK(allEqual(slice(r, kSampleRate + kL + kN), 0.0));
        // The analyser reads the floor once the frames it shows are silent.
        const auto input = s.frames(kInput);
        const auto [from, to] = framesBetween(1.0 + 1.5 * kN / kSampleRate, 11.0, input.size());
        REQUIRE(to > from);
        for (size_t i = from; i < to; ++i) CHECK(allEqual(input[i], -150.0));
        for (int d = 0; d < kDisplays; ++d) CHECK(allFinite(s.shown(d)));
    }
}

TEST_CASE("spectral: extremes are stable") {
    std::vector<Values> settings;
    {
        Values lowest, highest;
        const sub::BuiltinInfo info = builtinInfo("spectral");
        for (const sub::ParamInfo& p : info.params) {
            lowest.emplace_back(p.id, p.minValue);
            highest.emplace_back(p.id, p.maxValue);
        }
        settings.push_back(lowest);
        settings.push_back(highest);
    }
    settings.push_back({{"threshold", -72.f},
                        {"ratio", 20.f},
                        {"knee", 0.f},
                        {"smooth", 0.f},
                        {"attack", 1.f},
                        {"release", 10.f},
                        {"range", 48.f}});
    settings.push_back({{"threshold", 12.f},
                        {"below", 12.f},
                        {"upward", 10.f},
                        {"knee", 24.f},
                        {"range", 48.f},
                        {"tilt", -6.f},
                        {"smooth", 0.f},
                        {"output", 24.f}});
    settings.push_back({{"threshold", -72.f},
                        {"ratio", 20.f},
                        {"below", -72.f},
                        {"upward", 10.f},
                        {"range", 48.f},
                        {"delta", 1.f},
                        {"mix", 100.f},
                        {"tilt", 6.f},
                        {"link", 0.f}});
    settings.push_back({{"threshold", -40.f},
                        {"upward", 10.f},
                        {"below", 12.f},
                        {"range", 48.f},
                        {"delta", 1.f},
                        {"focus_lo", 20000.f},
                        {"focus_hi", 20.f},
                        {"output", 24.f}});
    settings.push_back({{"threshold", -60.f},
                        {"ratio", 20.f},
                        {"knee", 24.f},
                        {"smooth", 100.f},
                        {"attack", 1000.f},
                        {"release", 5000.f},
                        {"mix", 37.f},
                        {"link", 50.f},
                        {"delta", 1.f}});
    settings.push_back({{"threshold", 0.f},
                        {"ratio", 1.f},
                        {"upward", 10.f},
                        {"below", -10.f},
                        {"range", 48.f},
                        {"focus_lo", 2000.f},
                        {"focus_hi", 2500.f},
                        {"attack", 1.f},
                        {"release", 10.f},
                        {"smooth", 0.f},
                        {"output", -24.f}});

    std::vector<std::pair<std::string, Samples>> signals;
    {
        Rng rng(61);
        signals.emplace_back("loud noise", rng.uniformSamples(kSampleRate, -2.0, 2.0));
        signals.emplace_back("faint noise", whiteNoise(kSampleRate, -120.0, 67));
        signals.emplace_back("DC", Samples(kSampleRate, 0.5f));
        Samples nyquist(kSampleRate);
        for (size_t i = 0; i < nyquist.size(); ++i) nyquist[i] = i % 2 ? -1.f : 1.f;
        signals.emplace_back("Nyquist", nyquist);
        signals.emplace_back("silence", Samples(kSampleRate, 0.f));
    }
    for (size_t k = 0; k < settings.size(); ++k) {
        const Values& values = settings[k];
        float range = 24.f, output = 0.f;
        for (const auto& [id, value] : values) {
            if (id == "range") range = value;
            if (id == "output") output = value;
        }
        for (const auto& [name, x] : signals) {
            INFO("setting " + std::to_string(k) + ", " + name);
            Spectral s(values);
            const Samples other = whiteNoise(x.size(), -30.0, 71);
            const auto [l, r] = s.play(x, other);
            CHECK(allFinite(l) && allFinite(r));
            for (int d = 0; d < kDisplays; ++d) CHECK(allFinite(s.shown(d)));
            const double bound =
                rms(slice(x, 0, -kL)) * (std::pow(10.0, range / 20.0) + 1.0) * std::pow(10.0, output / 20.0) * 1.1 +
                1e-6;
            INFO(std::to_string(rms(slice(l, kL))) + " against at most " + std::to_string(bound));
            CHECK(rms(slice(l, kL)) <= bound);
            if (name == "silence") CHECK(allEqual(l, 0.0));
        }
    }
}

TEST_CASE("spectral: one channel") {
    const Samples x = pinkNoise(2 * kSampleRate, -12.0, 73);
    for (const float link : {100.f, 50.f, 0.f}) {
        INFO(std::to_string(link));
        const Values values = {{"threshold", -40.f}, {"ratio", 4.f}, {"upward", 2.f}, {"below", -50.f}, {"link", link}};
        Spectral mono(values), stereo(values);
        const Samples m = mono.play(x);
        const auto [l, r] = stereo.play(x, x);
        CHECK_ARRAY_EQUAL(m, l);
        CHECK_ARRAY_EQUAL(m, r);
    }
}

TEST_CASE("spectral: displays") {
    Spectral s(steady({{"threshold", -30.f}, {"ratio", 4.f}, {"range", 3.f}}));
    sub::Processor& p = s.processor();
    const auto read = [&](int display, uint64_t from) {
        std::vector<float> values;
        const uint64_t next = p.readDisplay(display, from, values);
        return std::pair<std::vector<float>, uint64_t>{values, next};
    };
    Samples x = pinkNoise(100 * kH, -20.0, 79);
    s.play(x, x);
    for (const int d : {kInput, kKey, kOutput, kGain}) {
        INFO(std::to_string(d));
        CHECK_EQ(s.shown(d).size(), size_t{12800});
        const auto [values, next] = read(d, 0);  // a reader from the start: the latest 8192, whole frames
        CHECK_EQ(next, uint64_t{12800});
        CHECK_EQ(values.size(), size_t{8192});
        CHECK_EQ((next - values.size()) % kPoints, uint64_t{0});
        CHECK_ARRAY_EQUAL(values, slice(s.shown(d), 12800 - 8192));
    }
    CHECK_EQ(s.shown(kInLevel).size(), size_t{100});
    CHECK_EQ(s.shown(kOutLevel).size(), size_t{100});
    x = pinkNoise(10 * kH, -20.0, 83);
    s.play(x, x);
    CHECK_EQ(read(kGain, 12800).first.size(), size_t{1280});
    CHECK_EQ(read(kInLevel, 100).first.size(), size_t{10});
    // A reader 9000 hops behind gets the latest 64 frames, starting on a frame.
    x = pinkNoise(9000 * kH, -20.0, 89);
    s.play(x, x);
    const auto [late, next] = read(kOutput, 12800 + 1280);
    CHECK_EQ(late.size(), size_t{8192});
    CHECK_EQ(next, uint64_t{(100 + 10 + 9000) * 128});
    CHECK_EQ((next - late.size()) % kPoints, uint64_t{0});

    // In step with what is heard: the frame published at a hop is the one that went in three hops before, so a
    // burst starting at input sample S first shows in the frame published at hop S / H + 4 (frame S / H + 3,
    // centred within half a frame of S), not a hop sooner or later.
    {
        constexpr int64_t kStart = 10 * kH + kH / 2;
        Samples burst(40 * kH, 0.f);
        const Samples noise = whiteNoise(burst.size(), -6.0, 97);
        std::copy(noise.begin() + kStart, noise.end(), burst.begin() + kStart);
        Spectral b(steady({{"threshold", 12.f}}));
        b.play(burst);
        const auto input = b.frames(kInput);
        size_t first = 0;
        while (first < input.size() && allEqual(input[first], -150.0)) ++first;
        CHECK_EQ(first, size_t{kStart / kH + 3});
        CHECK(std::abs(frameCentre(first, kN) - kStart) < kN / 2);
        CHECK(*std::max_element(input[first].begin(), input[first].end()) > -60.f);
    }

    // In and Out: the peaks, Out 3 dB under In with Range 3.
    {
        Spectral r(steady({{"threshold", -30.f}, {"ratio", 4.f}, {"range", 3.f}}));
        const Samples noise = pinkNoise(3 * kSampleRate, -20.0, 11);
        r.play(noise, noise);
        const std::vector<float>& in = r.shown(kInLevel);
        const std::vector<float>& out = r.shown(kOutLevel);
        const std::vector<float> lastIn = slice(in, -94), lastOut = slice(out, -94);
        const double difference = mean(lastIn) - mean(lastOut);
        INFO("In - Out " + std::to_string(difference));
        CHECK_NEAR(difference, 3.0, 0.6);
    }
    // With Delta on, the output display is the cut's spectrum.
    {
        Spectral r(steady({{"threshold", -30.f}, {"ratio", 4.f}, {"delta", 1.f}}));
        const Samples noise = pinkNoise(3 * kSampleRate, -20.0, 11);
        r.play(noise);
        const auto input = r.frames(kInput), output = r.frames(kOutput), gain = r.frames(kGain);
        const auto [from, to] = framesBetween(1.0, 3.0, gain.size());
        for (int j = pointAt(1000.0); j <= pointAt(5000.0); ++j) {
            INFO(std::to_string(spectral::displayFrequency(j)));
            const double g = meanDb(gain, from, to, j);
            CHECK_NEAR(meanDb(output, from, to, j),
                       meanDb(input, from, to, j) + gainToDb(1.0 - std::pow(10.0, g / 20.0)), 1.5);
        }
    }
}

TEST_CASE("spectral: engine latency") {
    // A click on a plain track and the same click through the device: the engine delays the plain track by
    // the latency, so they come out together.
    sub::Engine engine;
    engine.setClipFadeMs(0);
    constexpr int64_t kClick = 1000;
    stereoClickTrack(engine, 1.f, 0.f, kClick, 1.0);
    const uint32_t wet = stereoClickTrack(engine, 0.f, 1.f, kClick, 1.0);
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(wet), "spectral", -1);
    setParam(engine, id, "threshold", 12.f);
    CHECK_EQ(engine.processorInfo(id).latency, kL);
    const Samples out = engine.renderOffline(0.0, kSampleRate);
    const Samples left = channel(out, 0), right = channel(out, 1);
    const std::vector<int64_t> dry = above(left, 0.1), through = above(right, 0.1);
    REQUIRE(dry.size() == 1);
    REQUIRE(through.size() == 1);
    CHECK_EQ(dry[0], through[0]);
    CHECK_NEAR(right[static_cast<size_t>(through[0])], left[static_cast<size_t>(dry[0])], 1e-4);
    CHECK(maxAbs(slice(right, 0, through[0])) < 1e-4);
}

#ifdef NDEBUG
TEST_CASE("spectral: cost") {
    // 10 s of stereo pink noise at the defaults in well under half a second: catches a transform per sample or
    // an allocation storm, not a benchmark. Then how the work falls into audio callbacks.
    const Samples x = pinkNoise(10 * kSampleRate, -14.0, 97);
    Samples l = x, r = x;
    Spectral s;
    const auto start = std::chrono::steady_clock::now();
    s.run({&l, &r}, {}, {}, 256);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    INFO(std::to_string(seconds) + " s");
    CHECK(seconds < 0.5);

    // No audio callback carries a whole frame: its work is spread over the hop after it. At 192 kHz in blocks of
    // 32 (a frame every 64 blocks), the 99th percentile of the blocks' times is a small share of a hop's time (a
    // frame worked out in one block would be over that percentile, and all of a hop's time).
    constexpr double kRate = 192000.0;
    constexpr int kSmall = 32;
    auto processor = sub::BuiltinRegistry::instance().create("spectral");
    processor->prepare(kRate, kBlock);
    const Samples fast = pinkNoise(static_cast<size_t>(2 * kRate), -14.0, 99, kRate);
    Samples left = fast, right = fast;
    sub::ProcessContext ctx;
    ctx.sampleRate = kRate;
    std::vector<double> blocks;
    double total = 0.0;
    for (size_t start = 0; start + kSmall <= fast.size(); start += kSmall) {
        float* pointers[2] = {left.data() + start, right.data() + start};
        const auto from = std::chrono::steady_clock::now();
        processor->process(ctx, pointers, 2, kSmall);
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
        if (start < static_cast<size_t>(kRate / 4)) continue;  // (settled)
        blocks.push_back(took);
        total += took;
    }
    std::sort(blocks.begin(), blocks.end());
    const double perHop = total / static_cast<double>(blocks.size()) * (spectral::frameSize(kRate) / 4 / kSmall);
    const double p99 = blocks[blocks.size() * 99 / 100];
    INFO("99th percentile " + std::to_string(p99 * 1e6) + " us of a hop's " + std::to_string(perHop * 1e6) + " us");
    CHECK(p99 < 0.3 * perHop);
}
#endif
