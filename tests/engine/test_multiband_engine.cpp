// The built-in Multiband Dynamics: its listing; flat when it does nothing; each
// side's law (Above and Below, compressing and expanding, clamped, the knee,
// Amount), attack, release and Time as Ableton defines them; Peak and RMS;
// Input and Output gains; each band on its own, switched off (its frequencies
// the mid band's) and soloed; a sidechain keying each band by its own band;
// every control changing without a click; automation to the sample; reset and
// new sample rates; silence ringing out to exact zeros; the extremes; one
// channel and linked stereo; the tail; the displays; its cost; and an output
// that doesn't depend on how blocks are cut.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "Engine.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/MultibandDesign.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;
namespace mb = sub::multiband;

namespace {

constexpr int kBlock = 1024;  // the renderer's largest block (Renderer::kMaxBlock)

using Values = std::vector<std::pair<std::string, float>>;

// A parameter's change at a frame, as automation hands it over.
struct Change {
    int64_t frame;
    std::string id;
    float value;
};

int64_t at(double seconds, double rate = kSampleRate) { return static_cast<int64_t>(seconds * rate); }

// A Multiband Dynamics on its own, outside an engine, at any sample rate:
// processed in blocks, its changes handed over as automation (so its blocks
// split there) as the renderer does, a sidechain if given.
class Multiband {
public:
    explicit Multiband(const Values& values = {}, double rate = kSampleRate)
        : processor_(sub::BuiltinRegistry::instance().create("multiband")), rate_(rate) {
        for (const auto& [id, value] : values) set(id, value);
        processor_->prepare(rate, kBlock);
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

    // Processes one or two channels of equal length in place, `block` frames at a
    // time; `key` (two channels as long, or none) is its sidechain while `keyed`.
    void run(const std::vector<Samples*>& channels, const std::vector<Change>& changes = {}, int block = 256,
             const std::vector<const Samples*>& key = {}, bool keyed = false) {
        const auto frames = static_cast<int64_t>(channels[0]->size());
        sub::ProcessContext ctx;
        ctx.sampleRate = rate_;
        ctx.offline = true;
        float* pointers[2] = {};
        size_t next = 0;
        processor_->setSidechainConnected(keyed);
        for (int64_t start = 0; start < frames; start += block) {
            const int n = static_cast<int>(std::min<int64_t>(block, frames - start));
            while (next < changes.size() && changes[next].frame < start + n) {
                const Change& change = changes[next++];
                const int i = index(change.id);
                processor_->automate(i, processor_->params()[static_cast<size_t>(i)].toNormalized(change.value),
                                     static_cast<int32_t>(std::max<int64_t>(0, change.frame - start)));
            }
            for (size_t c = 0; c < channels.size(); ++c) pointers[c] = channels[c]->data() + start;
            if (key.size() == 2) processor_->setSidechain(key[0]->data() + start, key[1]->data() + start);
            ctx.samplePos = start;
            processor_->process(ctx, pointers, static_cast<int>(channels.size()), n);
            processor_->clearAutomation();
            processor_->setSidechain(nullptr, nullptr);
        }
    }
    // One channel: what comes out.
    Samples play(Samples mono, const std::vector<Change>& changes = {}, int block = 256) {
        run({&mono}, changes, block);
        return mono;
    }

    // Every value of a display so far.
    std::vector<float> display(const std::string& id) const {
        const std::vector<sub::DisplayInfo> displays = processor_->displays();
        for (size_t i = 0; i < displays.size(); ++i) {
            if (displays[i].id != id) continue;
            std::vector<float> values;
            processor_->readDisplay(static_cast<int>(i), 0, values);
            return values;
        }
        INFO(id);
        REQUIRE(false);
        return {};
    }

private:
    std::shared_ptr<sub::Processor> processor_;
    double rate_;
};

// The setting the tests start from unless they say otherwise: Peak, attack 1 ms, release 50 ms on every band.
Values base(Values extra = {}) {
    Values v = {{"mode", 0.f}};
    for (const char* band : {"low", "mid", "high"}) {
        v.push_back({std::string(band) + "_attack", 1.f});
        v.push_back({std::string(band) + "_release", 50.f});
    }
    v.insert(v.end(), extra.begin(), extra.end());
    return v;
}

// The same, with High and Low off: the mid band takes everything (steady-state numbers are exact).
Values single(Values extra = {}) {
    Values v = base({{"low_on", 0.f}, {"high_on", 0.f}});
    v.insert(v.end(), extra.begin(), extra.end());
    return v;
}

// One field set alike on every band.
Values everyBand(const std::string& field, float value) {
    return {{"low_" + field, value}, {"mid_" + field, value}, {"high_" + field, value}};
}

Values operator+(Values a, const Values& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

float amplitude(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }

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

Samples tone(double freq, double seconds, double level, double rate = kSampleRate) {
    Samples x(static_cast<size_t>(seconds * rate));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(level * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate));
    return x;
}

// A sine that fades in over 100 ms (so its start is no click of its own).
Samples smoothSine(double freq, double seconds, double rate = kSampleRate, double amplitude = 0.5) {
    Samples x(static_cast<size_t>(seconds * rate));
    const double fadeIn = 0.1 * rate;
    for (size_t i = 0; i < x.size(); ++i) {
        const double t = std::min(1.0, static_cast<double>(i) / fadeIn);
        x[i] = static_cast<float>(t * t * (3.0 - 2.0 * t) * amplitude * std::sin(2.0 * kPi * freq * i / rate));
    }
    return x;
}

Samples mix(const std::vector<Samples>& parts) {
    Samples sum(parts[0].size(), 0.f);
    for (const Samples& part : parts)
        for (size_t i = 0; i < sum.size(); ++i) sum[i] += part[i];
    return sum;
}

// The largest 6th difference over [from, to): a steep high-pass (|2 sin(pi f /
// sr)|^6), 8 times (18 dB) more sensitive at Nyquist than at a quarter of the
// sample rate and 2 10^5 times more than at 2 kHz (48 kHz). A step of d shows as
// up to 10 d; a smooth signal well below Nyquist hardly at all.
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

// A steady tone's amplitude in dB, from its RMS over the last 100 ms (whole
// periods of 40 Hz, 1 kHz and 10 kHz): the samples miss the crest after the
// crossovers' all-pass shifts the phase, the RMS doesn't.
double levelDb(const Samples& x, double rate = kSampleRate) {
    return 20.0 * std::log10(std::sqrt(2.0) * rms(slice(x, -static_cast<int64_t>(0.1 * rate))) + 1e-300);
}

// One tone's amplitude in dB (a Goertzel over [from, to), whole periods of it).
double toneDb(const Samples& x, double freq, int64_t from, int64_t to, double rate = kSampleRate) {
    std::complex<double> sum = 0.0;
    for (int64_t i = from; i < to; ++i)
        sum += static_cast<double>(x[static_cast<size_t>(i)]) *
               std::polar(1.0, -2.0 * kPi * freq * static_cast<double>(i) / rate);
    return 20.0 * std::log10(2.0 * std::abs(sum) / static_cast<double>(to - from) + 1e-300);
}
// Over the last 100 ms.
double toneDb(const Samples& x, double freq) {
    const auto n = static_cast<int64_t>(x.size());
    return toneDb(x, freq, n - at(0.1), n);
}

double energy(const Samples& x) {
    double sum = 0.0;
    for (const float v : x) sum += static_cast<double>(v) * v;
    return sum;
}

// The LR4 halves' shares of a frequency (in phase with each other).
double lowShare(double f, double crossover) { return 1.0 / (1.0 + std::pow(f / crossover, 4.0)); }
double highShare(double f, double crossover) { return 1.0 - lowShare(f, crossover); }

double db(double gain) { return 20.0 * std::log10(gain); }

}  // namespace

TEST_CASE("multiband dynamics is listed with its parameters") {
    const sub::BuiltinInfo info = builtinInfo("multiband");
    CHECK_EQ(info.name, std::string("Multiband Dynamics"));
    CHECK(!info.isInstrument());
    std::vector<std::string> ids = {"xover_low", "xover_high", "low_on", "high_on"};
    for (const char* band : {"low", "mid", "high"})
        for (const char* field :
             {"in", "out", "above", "above_ratio", "below", "below_ratio", "attack", "release", "solo"})
            ids.push_back(std::string(band) + "_" + field);
    for (const char* id : {"amount", "time", "output", "soft_knee", "mode", "sc_gain", "sc_mix"}) ids.push_back(id);
    REQUIRE(ids.size() == 38);
    CHECK(paramIds(info.params) == ids);

    const auto param = [&](const std::string& id) {
        for (const sub::ParamInfo& p : info.params)
            if (p.id == id) return p;
        REQUIRE(false);
        return sub::ParamInfo{};
    };
    const sub::ParamInfo low = param("xover_low");
    CHECK_EQ(low.name, std::string("Low-Mid Crossover"));
    CHECK_EQ(low.unit, std::string("Hz"));
    CHECK_EQ(low.minValue, 30.f);
    CHECK_EQ(low.maxValue, 18000.f);
    CHECK_EQ(low.defaultValue, 120.f);
    CHECK(low.isLog());
    CHECK_EQ(param("xover_high").defaultValue, 2500.f);
    const sub::ParamInfo ratio = param("mid_above_ratio");
    CHECK_EQ(ratio.name, std::string("Mid Above Ratio"));
    CHECK_EQ(ratio.unit, std::string("ratio"));
    CHECK_EQ(ratio.minValue, 0.25f);
    CHECK_EQ(ratio.maxValue, 100.f);
    CHECK_EQ(ratio.defaultValue, 1.f);
    CHECK(ratio.isLog());
    CHECK_EQ(param("high_below").name, std::string("High Below Threshold"));
    CHECK_EQ(param("high_below").minValue, -80.f);
    CHECK_EQ(param("high_below").maxValue, 0.f);
    CHECK_EQ(param("high_below").defaultValue, -40.f);
    CHECK_EQ(param("low_above").defaultValue, -20.f);
    CHECK_EQ(param("low_attack").defaultValue, 10.f);
    CHECK(param("low_attack").isLog());
    CHECK_EQ(param("low_release").defaultValue, 100.f);
    CHECK_EQ(param("low_release").maxValue, 3000.f);
    const sub::ParamInfo mode = param("mode");
    CHECK_EQ(mode.name, std::string("Peak/RMS"));
    CHECK(mode.valueLabels == (std::vector<std::string>{"Peak", "RMS"}));
    CHECK_EQ(mode.defaultValue, 1.f);
    const sub::ParamInfo time = param("time");
    CHECK_EQ(time.minValue, 10.f);
    CHECK_EQ(time.maxValue, 1000.f);
    CHECK_EQ(time.defaultValue, 100.f);
    CHECK(time.isLog());
    CHECK(param("low_on").valueLabels == (std::vector<std::string>{"Off", "On"}));
    CHECK_EQ(param("high_on").defaultValue, 1.f);
    CHECK_EQ(param("amount").defaultValue, 100.f);
    CHECK_EQ(param("sc_mix").defaultValue, 100.f);
    for (const sub::ParamInfo& p : info.params) {
        INFO(p.id);
        const bool solo = p.id.size() > 5 && p.id.substr(p.id.size() - 5) == "_solo";
        CHECK_EQ(p.automatable, !solo);
    }

    // Its displays, and no latency (no lookahead, as Ableton's).
    Multiband d;
    std::vector<std::pair<std::string, int>> displays;
    for (const sub::DisplayInfo& display : d.processor().displays())
        displays.emplace_back(display.id, display.samplesPerValue);
    std::vector<std::pair<std::string, int>> want;
    for (const char* band : {"low", "mid", "high"})
        for (const char* kind : {"_in", "_out", "_gain"}) want.emplace_back(std::string(band) + kind, 256);
    CHECK(displays == want);
    CHECK(d.processor().hasSidechain());
    CHECK_EQ(d.processor().latencySamples(), 0);

    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "multiband", -1);
    const sub::ProcessorInfo processor = engine.processorInfo(id);
    CHECK_EQ(processor.name, std::string("Multiband Dynamics"));
    CHECK_EQ(processor.latency, 0);
    CHECK(processor.tail > 0);
    CHECK(processor.hasSidechain);
}

TEST_CASE("multiband: doing nothing, it is flat: its bands add up to an all-pass") {
    const auto worstDb = [](const Samples& h) {
        const std::vector<double> magnitude = spectrum(h);
        double worst = 0.0;
        for (size_t k = 0; k < magnitude.size(); ++k) {
            const double f = static_cast<double>(k) * kSampleRate / static_cast<double>(h.size());
            if (f >= 20.0 && f <= 20000.0) worst = std::max(worst, std::abs(20.0 * std::log10(magnitude[k])));
        }
        return worst;
    };
    Multiband plain;
    const Samples neutral = plain.play(impulse(65536));
    CHECK(worstDb(neutral) < 0.01);
    // Any crossovers: wide apart, together, and the Low-Mid above the Mid-High (both split there).
    for (const Values& values :
         {Values{{"xover_low", 30.f}, {"xover_high", 18000.f}}, Values{{"xover_low", 1000.f}, {"xover_high", 1000.f}},
          Values{{"xover_low", 5000.f}, {"xover_high", 500.f}}}) {
        INFO(std::to_string(values[0].second) + " / " + std::to_string(values[1].second));
        Multiband d(values);
        const Samples h = d.play(impulse(65536));
        INFO("off by " + std::to_string(worstDb(h)) + " dB at most");
        CHECK(worstDb(h) < 0.01);
    }
    // Amount at 0 undoes every ratio: bit for bit what the defaults put out.
    Multiband none(Values{{"amount", 0.f}} + everyBand("above_ratio", 4.f) + everyBand("below_ratio", 4.f));
    CHECK_ARRAY_EQUAL(none.play(impulse(65536)), neutral);
    Multiband noiseNone(Values{{"amount", 0.f}} + everyBand("above_ratio", 4.f) + everyBand("below_ratio", 0.5f));
    Multiband noisePlain;
    const Samples in = noise(kSampleRate / 2, 1);
    CHECK_ARRAY_EQUAL(noiseNone.play(in), noisePlain.play(in));
}

TEST_CASE("multiband: above its threshold, a ratio over 1:1 compresses") {
    const double in = db(0.5);
    Multiband d(single({{"mid_above", -20.f}, {"mid_above_ratio", 4.f}}));
    const double want = in + (in + 20.0) * (1.0 / 4.0 - 1.0);  // -16.506
    CHECK_APPROX_TOL(levelDb(d.play(tone(1000.0, 1.0, 0.5))), want, 0.0, 0.05);
    Multiband limit(single({{"mid_above", -20.f}, {"mid_above_ratio", 100.f}}));
    CHECK_APPROX_TOL(levelDb(limit.play(tone(1000.0, 1.0, 0.5))), in + (in + 20.0) * (0.01 - 1.0), 0.0, 0.05);
    // Below the threshold, nothing.
    Multiband quiet(single({{"mid_above", -20.f}, {"mid_above_ratio", 4.f}}));
    CHECK_APPROX_TOL(levelDb(quiet.play(tone(1000.0, 1.0, 0.05))), db(0.05), 0.0, 0.02);
}

TEST_CASE("multiband: above its threshold, a ratio under 1:1 expands upwards, by 30 dB at most") {
    Multiband d(single({{"mid_above", -20.f}, {"mid_above_ratio", 0.5f}}));
    CHECK_APPROX_TOL(levelDb(d.play(tone(1000.0, 1.0, amplitude(-10.0)))), 0.0, 0.0, 0.05);
    Multiband most(single({{"mid_above", -40.f}, {"mid_above_ratio", 0.25f}}));
    const Samples out = most.play(tone(1000.0, 1.0, 0.5));
    CHECK_APPROX_TOL(levelDb(out), db(0.5) + mb::kMaxBoostDb, 0.0, 0.05);  // (101.9 dB asked)
    CHECK(allFinite(out));
}

TEST_CASE("multiband: below its threshold, a ratio over 1:1 lifts quiet sound, but never the floor") {
    Multiband d(single({{"mid_below", -40.f}, {"mid_below_ratio", 4.f}}));
    CHECK_APPROX_TOL(levelDb(d.play(tone(1000.0, 1.0, 0.001))), -45.0, 0.0, 0.1);
    // At -84 dB the upward gain is half faded: 33 dB asked, 16.5 given.
    Multiband faint(single({{"mid_below", -40.f}, {"mid_below_ratio", 4.f}}));
    CHECK_APPROX_TOL(levelDb(faint.play(tone(1000.0, 1.0, amplitude(-84.0)))), -67.5, 0.0, 0.1);
    CHECK_APPROX(mb::upwardFade(-84.f), 0.5);
    CHECK_EQ(mb::upwardFade(-96.f), 0.f);
    CHECK_EQ(mb::upwardFade(-72.f), 1.f);
    // Silence stays silence.
    Multiband silent(single({{"mid_below", -40.f}, {"mid_below_ratio", 100.f}, {"mid_in", 24.f}}));
    CHECK(allEqual(silent.play(Samples(kSampleRate, 0.f)), 0.0));
}

TEST_CASE("multiband: below its threshold, a ratio under 1:1 expands downwards, by 96 dB at most") {
    Multiband d(single({{"mid_below", -30.f}, {"mid_below_ratio", 0.5f}}));
    CHECK_APPROX_TOL(levelDb(d.play(tone(1000.0, 1.0, amplitude(-50.0)))), -70.0, 0.0, 0.1);
    Multiband gate(single({{"mid_below", -30.f}, {"mid_below_ratio", 0.25f}}));
    const Samples out = gate.play(tone(1000.0, 1.0, amplitude(-80.0)));
    CHECK(maxAbs(slice(out, -at(0.1))) < 2e-9);  // -150 dB asked, -96 given: -176 dB
    CHECK(maxAbs(slice(out, -at(0.1))) > 0.0);
}

TEST_CASE("multiband: both sides work at once") {
    const Values both =
        single({{"mid_above", -20.f}, {"mid_above_ratio", 4.f}, {"mid_below", -50.f}, {"mid_below_ratio", 2.f}});
    for (const auto& [in, out] :
         std::vector<std::pair<double, double>>{{-10.0, -17.5}, {-60.0, -55.0}, {-35.0, -35.0}}) {
        INFO(std::to_string(in) + " dB in");
        Multiband d(both);
        CHECK_APPROX_TOL(levelDb(d.play(tone(1000.0, 1.0, amplitude(in)))), out, 0.0, 0.1);
    }
}

TEST_CASE("multiband: Soft Knee bends the curve in over 6 dB") {
    const Values knee = single({{"soft_knee", 1.f}, {"mid_above", -20.f}, {"mid_above_ratio", 4.f}});
    for (const auto& [in, gain] :
         std::vector<std::pair<double, double>>{{-20.0, -0.5625}, {-17.0, -2.25}, {-23.0, 0.0}}) {
        INFO(std::to_string(in) + " dB in");
        Multiband d(knee);
        CHECK_APPROX_TOL(levelDb(d.play(tone(1000.0, 1.0, amplitude(in)))), in + gain, 0.0, 0.03);
    }
    // The shared curve says the same.
    CHECK_APPROX_TOL(mb::staticGainDb(-20.f, -20.f, 4.f, -40.f, 1.f, true, 1.f), -0.5625, 0.0, 1e-6);
    CHECK_APPROX_TOL(mb::staticGainDb(-17.f, -20.f, 4.f, -40.f, 1.f, true, 1.f), -2.25, 0.0, 1e-5);
    CHECK_EQ(mb::staticGainDb(-23.f, -20.f, 4.f, -40.f, 1.f, true, 1.f), 0.f);
    CHECK_APPROX_TOL(mb::staticGainDb(-43.f, -20.f, 1.f, -40.f, 4.f, true, 1.f), 2.25, 0.0, 1e-5);
}

TEST_CASE("multiband: Amount scales every ratio's effect") {
    const double in = db(0.5);
    Multiband half(single({{"mid_above", -20.f}, {"mid_above_ratio", 4.f}, {"amount", 50.f}}));
    CHECK_APPROX_TOL(levelDb(half.play(tone(1000.0, 1.0, 0.5))), in + 0.5 * (in + 20.0) * -0.75, 0.0, 0.05);
    Multiband none(single({{"mid_above", -20.f}, {"mid_above_ratio", 4.f}, {"amount", 0.f}}));
    Multiband unity(single());
    const Samples x = tone(1000.0, 1.0, 0.5);
    CHECK_ARRAY_EQUAL(none.play(x), unity.play(x));
}

TEST_CASE("multiband: attack and release are each side's, as Ableton defines them, and Time scales them") {
    // Each display value stands for 256 samples: a time is read at a value's centre.
    const auto centre = [](size_t v) { return (static_cast<double>(v) + 0.5) * 256.0 / kSampleRate; };
    const auto rise = [&](const std::vector<float>& gain, double from, double mark) {
        for (size_t v = 0; v < gain.size(); ++v)
            if (centre(v) > from && std::abs(gain[v]) >= mark) return (centre(v) - from) * 1000.0;
        return -1.0;
    };
    const auto fall = [&](const std::vector<float>& gain, double from, double mark) {
        double last = -1.0;
        for (size_t v = 0; v < gain.size(); ++v)
            if (centre(v) > from && std::abs(gain[v]) > mark) last = centre(v);
        return (last - from) * 1000.0;
    };
    // Above: 100:1 at -20 dB; the tone from -40 to -6 dB at 0.5 s, and back at 1.5 s.
    for (const auto& [time, attack, release] :
         std::vector<std::tuple<float, double, double>>{{100.f, 100.0, 225.0}, {50.f, 50.0, 125.0}}) {
        INFO("Time " + std::to_string(time));
        Multiband d(single({{"mid_above", -20.f},
                            {"mid_above_ratio", 100.f},
                            {"mid_attack", 100.f},
                            {"mid_release", 200.f},
                            {"time", time}}));
        Samples x = tone(1000.0, 2.5, 0.01);
        for (int64_t i = at(0.5); i < at(1.5); ++i) x[static_cast<size_t>(i)] *= 50.f;
        d.play(x);
        const std::vector<float> gain = d.display("mid_gain");
        REQUIRE(gain.size() == 468);
        const double full = (db(0.5) + 20.0) * (0.01 - 1.0);  // -13.84
        CHECK_APPROX_TOL(gain[at(1.45) / 256], full, 0.0, 0.05);
        // 63 % of the way in its attack; 37 % left after its release, the 25 ms peak window holding first.
        CHECK_APPROX_TOL(rise(gain, 0.5, 0.632 * std::abs(full)), attack, 0.0, 4.0);
        CHECK_APPROX_TOL(fall(gain, 1.5, 0.368 * std::abs(full)), release, 0.0, 4.0);
    }
    // Below: its attack comes as the level drops under the threshold (after the window lets go of the
    // louder peaks), its release as the level comes back (at once).
    Multiband below(
        single({{"mid_below", -30.f}, {"mid_below_ratio", 4.f}, {"mid_attack", 100.f}, {"mid_release", 200.f}}));
    Samples x = tone(1000.0, 2.5, 0.1);
    for (int64_t i = at(0.5); i < at(1.5); ++i) x[static_cast<size_t>(i)] *= amplitude(-30.0);
    below.play(x);
    const std::vector<float> gain = below.display("mid_gain");
    CHECK_APPROX_TOL(gain[at(1.45) / 256], 15.0, 0.0, 0.05);
    CHECK_APPROX_TOL(rise(gain, 0.5, 0.632 * 15.0), 125.0, 0.0, 4.0);
    CHECK_APPROX_TOL(fall(gain, 1.5, 0.368 * 15.0), 200.0, 0.0, 4.0);
}

TEST_CASE("multiband: Peak follows the peaks, RMS the power, and switching between them is smooth") {
    Multiband peak(single());
    peak.play(tone(1000.0, 0.5, 0.5));
    CHECK_APPROX_TOL(peak.display("mid_in").back(), db(0.5), 0.0, 0.05);
    Multiband rmsMode(single({{"mode", 1.f}}));
    rmsMode.play(tone(1000.0, 0.5, 0.5));
    CHECK_APPROX_TOL(rmsMode.display("mid_in").back(), db(0.5) - 3.0103, 0.0, 0.05);

    // Mid-tone, compressing: the level moves 3 dB within about 30 ms, the gain follows (at the release
    // time, the change shrinking), without a click.
    const Samples x = smoothSine(220.0, 2.0);
    Multiband d(single({{"mid_above", -20.f}, {"mid_above_ratio", 4.f}}));
    const Samples out = d.play(x, {{at(0.6), "mode", 1.f}, {at(1.2), "mode", 0.f}, {at(1.21), "mode", 1.f}});
    CHECK(allFinite(out));
    INFO("tone " + std::to_string(clickiness(x, at(0.2))) + ", through the switches " +
         std::to_string(clickiness(out, at(0.2))));
    CHECK(clickiness(out, at(0.2)) < 2e-4);
    const std::vector<float> level = d.display("mid_in"), gain = d.display("mid_gain");
    CHECK_APPROX_TOL(level[at(0.59) / 256], db(0.5), 0.0, 0.1);
    CHECK_APPROX_TOL(level[at(0.64) / 256], db(0.5) - 3.0103, 0.0, 0.1);
    CHECK_APPROX_TOL(gain[at(0.59) / 256], (db(0.5) + 20.0) * -0.75, 0.0, 0.1);
    CHECK_APPROX_TOL(gain[at(1.0) / 256], (db(0.5) - 3.0103 + 20.0) * -0.75, 0.0, 0.1);
}

TEST_CASE("multiband: Input drives the band into its thresholds; the Outputs come after") {
    const Values values = single({{"mid_in", 6.f}, {"mid_above", -20.f}, {"mid_above_ratio", 4.f}});
    const Samples x = tone(1000.0, 1.0, amplitude(-26.0));
    Multiband d(values);
    CHECK_APPROX_TOL(levelDb(d.play(x)), -20.0, 0.0, 0.05);
    CHECK_APPROX_TOL(d.display("mid_in").back(), -20.0, 0.0, 0.05);
    CHECK_APPROX_TOL(d.display("mid_gain").back(), 0.0, 0.0, 0.01);
    Multiband out(values + Values{{"mid_out", -6.f}});
    CHECK_APPROX_TOL(levelDb(out.play(x)), -26.0, 0.0, 0.05);
    Multiband output(values + Values{{"mid_out", -6.f}, {"output", -6.f}});
    CHECK_APPROX_TOL(levelDb(output.play(x)), -32.0, 0.0, 0.05);
}

TEST_CASE("multiband: each band is its own, and a band switched off belongs to the mid band") {
    // 10 kHz: the high band's share (99.6 %) is compressed, the mid band's 0.4 % passes in phase.
    const double highIn = db(0.1 * highShare(10000.0, 2500.0));
    const double highOut =
        db(0.1 * highShare(10000.0, 2500.0) * amplitude((highIn + 30.0) * -0.75) + 0.1 * lowShare(10000.0, 2500.0));
    Multiband high(base({{"high_above", -30.f}, {"high_above_ratio", 4.f}}));
    CHECK_APPROX_TOL(levelDb(high.play(tone(10000.0, 1.0, 0.1))), highOut, 0.0, 0.15);  // -27.42
    CHECK_APPROX_TOL(highOut, -27.42, 0.0, 0.02);
    Multiband highOff(base({{"high_above", -30.f}, {"high_above_ratio", 4.f}, {"high_on", 0.f}}));
    CHECK_APPROX_TOL(levelDb(highOff.play(tone(10000.0, 1.0, 0.1))), -20.0, 0.0, 0.02);  // the mid band's: 1:1

    // 40 Hz in the low band, likewise.
    const double lowIn = db(0.1 * lowShare(40.0, 120.0));
    const double lowOut =
        db(0.1 * lowShare(40.0, 120.0) * amplitude((lowIn + 30.0) * -0.75) + 0.1 * highShare(40.0, 120.0));
    Multiband low(base({{"low_above", -30.f}, {"low_above_ratio", 4.f}}));
    CHECK_APPROX_TOL(levelDb(low.play(tone(40.0, 2.0, 0.1))), lowOut, 0.0, 0.15);  // -27.28
    CHECK_APPROX_TOL(lowOut, -27.28, 0.0, 0.02);
    Multiband lowOff(base({{"low_above", -30.f}, {"low_above_ratio", 4.f}, {"low_on", 0.f}}));
    CHECK_APPROX_TOL(levelDb(lowOff.play(tone(40.0, 2.0, 0.1))), -20.0, 0.0, 0.02);

    // Off, a band takes the mid band's settings (and the mid band's detector hears it).
    Multiband followsMid(base({{"high_on", 0.f}, {"mid_above", -30.f}, {"mid_above_ratio", 4.f}}));
    CHECK_APPROX_TOL(levelDb(followsMid.play(tone(10000.0, 1.0, 0.1))), -20.0 - 10.0 * 0.75, 0.0, 0.1);

    // Switching them on and off mid-tone (a tone each band carries part of): no click.
    const Samples x = smoothSine(220.0, 3.0);
    Multiband d(base({{"xover_high", 400.f},
                      {"xover_low", 120.f},
                      {"low_above", -40.f},
                      {"low_above_ratio", 4.f},
                      {"high_above", -40.f},
                      {"high_above_ratio", 0.5f},
                      {"mid_below", -10.f},
                      {"mid_below_ratio", 2.f}}));
    const Samples out = d.play(x, {{at(0.5), "high_on", 0.f},
                                   {at(1.0), "low_on", 0.f},
                                   {at(1.5), "high_on", 1.f},
                                   {at(2.0), "low_on", 1.f},
                                   {at(2.5), "low_on", 0.f},
                                   {at(2.51), "low_on", 1.f}});
    CHECK(allFinite(out));
    INFO("through the switches " + std::to_string(clickiness(out, at(0.2))));
    CHECK(clickiness(out, at(0.2)) < 2e-4);
    // (and they did something: the level moved)
    CHECK(std::abs(levelDb(slice(out, 0, at(0.5))) - levelDb(slice(out, 0, at(1.0)))) > 1.0);
}

TEST_CASE("multiband: solo lets only the soloed bands be heard") {
    const Samples x = mix({tone(40.0, 1.0, 0.1), tone(1000.0, 1.0, 0.1), tone(10000.0, 1.0, 0.1)});
    const auto levels = [&](const Values& values) {
        Multiband d(values);
        const Samples out = d.play(x);
        return std::vector<double>{toneDb(out, 40.0) + 20.0, toneDb(out, 1000.0) + 20.0, toneDb(out, 10000.0) + 20.0};
    };
    const std::vector<double> mid = levels({{"mid_solo", 1.f}});
    CHECK_APPROX_TOL(mid[1], db(lowShare(1000.0, 2500.0)), 0.0, 0.1);  // -0.22
    CHECK(mid[0] < -36.0);
    CHECK(mid[2] < -46.0);
    const std::vector<double> outer = levels({{"low_solo", 1.f}, {"high_solo", 1.f}});
    CHECK(outer[1] < -30.0);  // its 2.5 % in the high band
    CHECK_APPROX_TOL(outer[0], 0.0, 0.0, 0.3);
    CHECK_APPROX_TOL(outer[2], 0.0, 0.0, 0.3);
    // A band switched off can't be soloed: its solo is ignored.
    const std::vector<double> ignored = levels({{"high_solo", 1.f}, {"high_on", 0.f}});
    for (const double level : ignored) CHECK_APPROX_TOL(level, 0.0, 0.0, 0.05);
    // Switched off, it follows the mid band's solo.
    const std::vector<double> follows = levels({{"mid_solo", 1.f}, {"high_on", 0.f}});
    CHECK_APPROX_TOL(follows[2], 0.0, 0.0, 0.05);
    CHECK(follows[0] < -36.0);

    // Toggling solos mid-tone: no click.
    const Samples s = smoothSine(220.0, 2.0);
    Multiband d({{"xover_high", 400.f}});
    const Samples out = d.play(s, {{at(0.4), "mid_solo", 1.f},
                                   {at(0.8), "low_solo", 1.f},
                                   {at(1.2), "mid_solo", 0.f},
                                   {at(1.6), "low_solo", 0.f}});
    INFO("through the solos " + std::to_string(clickiness(out, at(0.2))));
    CHECK(clickiness(out, at(0.2)) < 2e-4);
}

TEST_CASE("multiband: a sidechain keys each band by the same band of the key") {
    sub::Engine engine;
    engine.setClipFadeMs(0);
    const Samples main = mix({tone(40.0, 1.0, 0.1), tone(1000.0, 1.0, 0.1), tone(10000.0, 1.0, 0.1)});
    const uint32_t track = clipTrack(engine, makeWav(stereo(main), 2), 0.0, 1.0);
    const uint32_t keyTrack = clipTrack(engine, makeWav(stereo(tone(1000.0, 1.0, 1.0 - 1.0 / 32768)), 2), 0.0, 1.0);
    engine.setTrackGain(keyTrack, 0.f);  // heard only through the sidechain, taken before its fader
    const uint32_t device = engine.addBuiltinProcessor(engine.trackChain(track), "multiband", -1);
    for (const auto& [id, value] : base(everyBand("above", -25.f) + everyBand("above_ratio", 4.f)))
        setParam(engine, device, id, value);
    const auto levels = [&] {
        const Samples out = channel(engine.renderOffline(0.0, kSampleRate), 0);
        return std::vector<double>{toneDb(out, 40.0) + 20.0, toneDb(out, 1000.0) + 20.0, toneDb(out, 10000.0) + 20.0};
    };
    const std::vector<double> unkeyed = levels();
    // Keyed by its own input: each band's level is about -20 dB, against -25.
    for (const double level : unkeyed) CHECK(level < -3.3 && level > -4.0);

    engine.setProcessorSidechain(device, keyTrack, sub::SidechainTap::PreFader);
    // The key's 1 kHz keys the mid band alone: its 97.5 % share is cut 18.6 dB, the high band's 2.5 % passes.
    const double midKey = db(lowShare(1000.0, 2500.0));
    const auto keyedOut = [&](double keyDb) {
        return db(lowShare(1000.0, 2500.0) * amplitude((keyDb + midKey + 25.0) * -0.75) + highShare(1000.0, 2500.0));
    };
    std::vector<double> keyed = levels();
    CHECK_APPROX_TOL(keyed[0], 0.0, 0.0, 0.15);
    CHECK_APPROX_TOL(keyed[2], 0.0, 0.0, 0.15);
    CHECK_APPROX_TOL(keyed[1], keyedOut(0.0), 0.0, 0.4);  // -17.1
    CHECK_APPROX_TOL(keyedOut(0.0), -17.1, 0.0, 0.05);
    // The key's gain.
    setParam(engine, device, "sc_gain", -12.f);
    keyed = levels();
    CHECK_APPROX_TOL(keyed[1], keyedOut(-12.0), 0.0, 0.4);  // -9.2
    setParam(engine, device, "sc_gain", 0.f);
    // Sidechain Mix at 0: the device's own input keys it, exactly as with no sidechain.
    setParam(engine, device, "sc_mix", 0.f);
    const Samples ownKey = engine.renderOffline(0.0, kSampleRate);
    setParam(engine, device, "sc_mix", 100.f);
    engine.clearProcessorSidechain(device);
    const Samples noKey = engine.renderOffline(0.0, kSampleRate);
    CHECK_ARRAY_EQUAL(ownKey, noKey);
    for (size_t i = 0; i < 3; ++i) CHECK_APPROX_TOL(levels()[i], unkeyed[i], 0.0, 1e-6);
    // A silent sidechain keys nothing: it doesn't fall back to its own input.
    engine.setProcessorSidechain(device, engine.addTrack(), sub::SidechainTap::PreFader);
    for (const double level : levels()) CHECK_APPROX_TOL(level, 0.0, 0.0, 0.1);

    // Keyed, a band's `in` display is the trigger's level, what its thresholds are compared with: the key's
    // band, after the band's Input; at Sidechain Mix 0 the device's own.
    for (const float scMix : {100.f, 0.f}) {
        INFO("sc_mix " + std::to_string(scMix));
        Multiband direct(base({{"mid_in", 6.f}, {"sc_mix", scMix}}));
        Samples l = tone(1000.0, 0.5, 0.1), r = l;
        const Samples key = tone(1000.0, 0.5, 0.5);
        direct.run({&l, &r}, {}, 256, {&key, &key}, true);
        const double trigger = scMix > 0.f ? 0.5 : 0.1;
        CHECK_APPROX_TOL(direct.display("mid_in").back(), db(trigger * lowShare(1000.0, 2500.0)) + 6.0, 0.0, 0.1);
        CHECK_EQ(direct.display("mid_out").back(), direct.display("mid_in").back());  // (1:1: no change)
    }
}

TEST_CASE("multiband: one channel is keyed by both of the key's, and a key connected again starts from silence") {
    // One main channel of 1 kHz at 0.1; the key's 1 kHz at 1.0 on its right channel only: the mean of
    // the two (0.5) keys it.
    const Values values = base(everyBand("above", -25.f) + everyBand("above_ratio", 4.f));
    Multiband d(values);
    Samples x = tone(1000.0, 1.0, 0.1);
    const Samples silent(x.size(), 0.f), right = tone(1000.0, 1.0, 1.0);
    d.run({&x}, {}, 256, {&silent, &right}, true);
    const double want = db(lowShare(1000.0, 2500.0) * amplitude((db(0.5 * lowShare(1000.0, 2500.0)) + 25.0) * -0.75) +
                           highShare(1000.0, 2500.0));
    CHECK_APPROX_TOL(levelDb(x) + 20.0, want, 0.0, 0.5);  // -13.2
    CHECK_APPROX_TOL(want, -13.2, 0.0, 0.05);

    // Keyed by a loud key, then let go of while the key still sounds (its split frozen mid-ring), then
    // keyed again: the same as a device keyed for the first time there.
    const Samples key = tone(60.0, 1.0, 1.0), main = mix({tone(60.0, 1.0, 0.1), tone(1000.0, 1.0, 0.1)});
    Multiband again(values), fresh(values);
    for (Multiband* device : {&again, &fresh}) {
        Samples a = main, b = main;
        device->run({&a, &b}, {}, 256, {&key, &key}, device == &again);  // (fresh: not connected)
        for (int i = 0; i < 2; ++i) {                                    // two seconds of its own input
            a = main;
            b = main;
            device->run({&a, &b}, {}, 256, {&key, &key}, false);
        }
    }
    Samples a1 = main, b1 = main, a2 = main, b2 = main;
    again.run({&a1, &b1}, {}, 256, {&key, &key}, true);
    fresh.run({&a2, &b2}, {}, 256, {&key, &key}, true);
    CHECK_ALLCLOSE(slice(a1, 0, at(0.05)), slice(a2, 0, at(0.05)), 0.0, 1e-6);
    CHECK(maxAbs(slice(a1, 0, at(0.05))) > 0.05);
}

TEST_CASE("multiband: every control changes without a click") {
    // A 220 Hz tone split three ways (Mid-High at 400 Hz, so every band carries part of it), every control
    // jumping in turn, as automation's steps make them, and several at once.
    const Samples x = smoothSine(220.0, 5.0);
    std::vector<Change> changes;
    double t = 0.3;
    const auto add = [&](const std::string& id, float value, bool sameFrame = false) {
        if (!sameFrame) t += 0.09;
        changes.push_back({at(t), id, value});
    };
    for (const char* band : {"low", "mid", "high"}) {
        const std::string b = band;
        add(b + "_above_ratio", 8.f);
        add(b + "_above", -40.f);
        add(b + "_above", -20.f);
        add(b + "_below_ratio", 0.5f);
        add(b + "_below", -20.f);
        add(b + "_below", -40.f);
        add(b + "_in", 12.f);
        add(b + "_out", -12.f);
        add(b + "_above_ratio", 1.f);
    }
    for (const auto& [id, value] : std::vector<std::pair<std::string, float>>{
             {"amount", 0.f},         {"amount", 70.f},  {"output", -12.f}, {"xover_low", 300.f}, {"xover_low", 80.f},
             {"xover_high", 1000.f},  {"high_on", 0.f},  {"low_on", 0.f},   {"high_on", 1.f},     {"low_on", 1.f},
             {"mid_solo", 1.f},       {"low_solo", 1.f}, {"mid_solo", 0.f}, {"low_solo", 0.f},    {"mode", 0.f},
             {"soft_knee", 1.f},      {"time", 1000.f},  {"time", 10.f},    {"mode", 1.f},        {"soft_knee", 0.f},
             {"mid_below_ratio", 8.f}})
        add(id, value);
    // Several at the same frame.
    add("mid_above_ratio", 4.f);
    add("mid_above", -35.f, true);
    add("xover_low", 200.f, true);
    add("xover_high", 500.f, true);
    add("amount", 100.f, true);
    add("high_on", 0.f, true);
    add("mode", 0.f, true);
    REQUIRE(t < 4.8);

    Multiband d(everyBand("attack", 1.f) + Values{{"xover_high", 400.f}});
    const Samples out = d.play(x, changes);
    CHECK(allFinite(out));
    const double input = clickiness(x, at(0.2));
    const double output = clickiness(out, at(0.2));
    INFO("tone " + std::to_string(input) + ", through the changes " + std::to_string(output));
    CHECK(output < 2e-4);

    // What the measure makes of a click: the tone through the defaults, cut 12 dB at once at a peak near 2 s.
    Multiband plain;
    Samples spliced = plain.play(x);
    int64_t peak = at(2.0);
    for (int64_t i = at(2.0) - 120; i < at(2.0) + 120; ++i)
        if (std::abs(spliced[static_cast<size_t>(i)]) > std::abs(spliced[static_cast<size_t>(peak)])) peak = i;
    for (size_t i = static_cast<size_t>(peak); i < spliced.size(); ++i) spliced[i] *= amplitude(-12.0);
    CHECK(clickiness(spliced, at(0.2)) > 1000 * output);
}

TEST_CASE("multiband: its automation plays through the engine sample-accurately") {
    sub::Engine engine;
    engine.setClipFadeMs(0);
    // (221.25 Hz: at its peak where the automation steps, so the first sample changed shows.)
    const Samples x = smoothSine(221.25, 3.0);
    const uint32_t track = clipTrack(engine, makeWav(stereo(x), 2), 0.0, 3.0);
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "multiband", -1);
    const Samples untouched = engine.renderOffline(0.0, 3 * kSampleRate);

    // Output: 0 dB until beat 2 (1 s), then -12 dB.
    const sub::ParamInfo output = paramInfo(engine, id, "output");
    const float zero = output.toNormalized(0.f), cut = output.toNormalized(-12.f);
    using Points = std::vector<sub::AutomationPoint>;
    engine.setTrackAutomation(track, {{id, "output", Points{{0.0, zero, 0.f}, {2.0, zero, 0.f}, {2.0, cut, 0.f}}}});
    const Samples out = engine.renderOffline(0.0, 3 * kSampleRate);
    CHECK(allFinite(out));
    int64_t first = -1;
    for (size_t i = 0; i < out.size() && first < 0; ++i)
        if (out[i] != untouched[i]) first = static_cast<int64_t>(i) / 2;
    CHECK_EQ(first, int64_t{kSampleRate});
    // Within 0.05 dB of -12 dB after 40 ms (the glide's two 5 ms one-poles), sample for sample.
    const Samples after = frames(out, kSampleRate + at(0.04)), before = frames(untouched, kSampleRate + at(0.04));
    Samples want = before;
    for (float& v : want) v *= amplitude(-12.0);
    CHECK_ALLCLOSE(after, want, 0.0, 0.5 * amplitude(-12.0) * (amplitude(0.05) - 1.0));
    for (int c = 0; c < 2; ++c) {
        INFO(std::to_string(c));
        CHECK(clickiness(channel(out, c), kSampleRate / 5) <
              2.0 * clickiness(channel(untouched, c), kSampleRate / 5) + 1e-4);
    }
}

TEST_CASE("multiband: reset and a new sample rate start it from silence") {
    const Values values = base(everyBand("above_ratio", 4.f) + everyBand("below_ratio", 2.f));
    Multiband fresh(values);
    const Samples want = fresh.play(impulse(8192));
    Multiband d(values);
    d.play(noise(kSampleRate, 8));
    d.processor().reset();
    CHECK_ARRAY_EQUAL(d.play(impulse(8192)), want);

    // A new rate: retuned to it (as a device made at that rate), and the levels the same.
    const double compressed = db(0.5) + (db(0.5) + 20.0) * -0.75;
    for (const double rate : {96000.0, 44100.0}) {
        INFO(std::to_string(rate));
        const Values compressing = single({{"mid_above", -20.f}, {"mid_above_ratio", 4.f}});
        Multiband r(compressing);
        r.play(noise(kSampleRate, 9));
        r.processor().prepare(rate, kBlock);
        Multiband atRate(compressing, rate);
        const Samples y = tone(1000.0, 1.0, 0.5, rate);
        const Samples out = r.play(y);
        CHECK_ARRAY_EQUAL(out, atRate.play(y));
        CHECK_APPROX_TOL(levelDb(out, rate), compressed, 0.0, 0.05);
    }
}

TEST_CASE("multiband: silence rings out to exact zeros") {
    for (const float mode : {0.f, 1.f}) {
        INFO(mode == 0.f ? "Peak" : "RMS");
        Multiband d(everyBand("below", -40.f) + everyBand("below_ratio", 100.f) + everyBand("in", 24.f) +
                    Values{{"output", 24.f}, {"mode", mode}});
        Samples l = noise(kSampleRate / 2, 10, 1.f), r = noise(kSampleRate / 2, 11, 1.f);
        l.resize(static_cast<size_t>(2.5 * kSampleRate), 0.f);
        r.resize(l.size(), 0.f);
        d.run({&l, &r});
        CHECK(allFinite(l) && allFinite(r));
        CHECK(allEqual(slice(l, -kSampleRate), 0.0));
        CHECK(allEqual(slice(r, -kSampleRate), 0.0));
        for (const float v : l) CHECK(v == 0.f || std::abs(v) >= std::numeric_limits<float>::min());
    }
    // Denormal input passes as finite numbers, and dies away to zeros too.
    Multiband d(everyBand("below_ratio", 4.f));
    Samples tiny(static_cast<size_t>(kSampleRate), 0.f);
    for (size_t i = 0; i < 1000; ++i) tiny[i] = (i % 2 ? 1e-40f : -1e-41f);
    const Samples quiet = d.play(tiny);
    CHECK(allFinite(quiet));
    CHECK(allEqual(slice(quiet, kSampleRate / 2), 0.0));
}

TEST_CASE("multiband: it stays stable at the extremes and at any sample rate") {
    const auto band = [](const std::string& field, float value) { return everyBand(field, value); };
    const std::vector<Values> extremes = {
        {{"xover_low", 30.f}, {"xover_high", 18000.f}},
        {{"xover_low", 18000.f}, {"xover_high", 30.f}},
        {{"xover_low", 18000.f}, {"xover_high", 18000.f}},
        {{"xover_low", 30.f}, {"xover_high", 30.f}},
        band("above", -80.f) + band("above_ratio", 0.25f) + band("below", -80.f) + band("below_ratio", 100.f) +
            band("in", 24.f) + band("out", 24.f) + band("attack", 0.1f) + band("release", 1.f) +
            Values{{"output", 24.f}, {"time", 10.f}, {"soft_knee", 1.f}, {"mode", 0.f}},
        band("above", 0.f) + band("above_ratio", 100.f) + band("below", 0.f) + band("below_ratio", 0.25f) +
            band("in", -24.f) + band("out", -24.f) + band("attack", 1000.f) + band("release", 3000.f) +
            Values{{"output", -24.f}, {"time", 1000.f}, {"mode", 1.f}},
        band("above", -80.f) + band("above_ratio", 0.25f) + band("below", 0.f) + band("below_ratio", 100.f) +
            band("in", 24.f) + band("out", 24.f) + band("attack", 0.1f) + band("release", 1.f) +
            Values{{"output", 24.f}, {"time", 10.f}, {"xover_low", 30.f}, {"xover_high", 18000.f}},
        band("above", -80.f) + band("above_ratio", 0.25f) + band("in", 24.f) + band("out", 24.f) +
            band("attack", 0.1f) + band("release", 1.f) +
            Values{{"output", 24.f}, {"time", 10.f}, {"low_on", 0.f}, {"high_on", 0.f}, {"soft_knee", 1.f}},
    };
    for (const double rate : {44100.0, 192000.0}) {
        for (size_t e = 0; e < extremes.size(); ++e) {
            INFO(std::to_string(rate) + " Hz, setting " + std::to_string(e));
            Multiband d(extremes[e], rate);
            Samples l = noise(static_cast<size_t>(rate / 2), 12, 1.f),
                    r = noise(static_cast<size_t>(rate / 2), 13, 1.f);
            l.resize(l.size() + static_cast<size_t>(rate / 4), 0.f);
            r.resize(l.size(), 0.f);
            d.run({&l, &r});
            CHECK(allFinite(l) && allFinite(r));
            CHECK(maxAbs(l) <= 5e5);
            CHECK(maxAbs(r) <= 5e5);
        }
    }
}

TEST_CASE("multiband: it gets over a broken input's NaNs and infinities") {
    // 10 ms of them in noise: once the input is finite again, so is the output (within its 25 ms
    // window and a 32-sample flush); then it plays as if they had never come (an infinite level
    // compressed it fully, and that lets go at the release time).
    const Values values = base(everyBand("above_ratio", 4.f) + everyBand("below_ratio", 4.f));
    for (const float mode : {0.f, 1.f}) {
        INFO(mode == 0.f ? "Peak" : "RMS");
        const Samples left = noise(kSampleRate, 18, 0.3f), right = noise(kSampleRate, 19, 0.3f);
        Samples l = left, r = right;
        for (int64_t i = at(0.3); i < at(0.31); ++i) {
            l[static_cast<size_t>(i)] = i % 3 == 0 ? std::numeric_limits<float>::quiet_NaN()
                                                   : (i % 3 == 1 ? std::numeric_limits<float>::infinity() : 1e38f);
            r[static_cast<size_t>(i)] = -std::numeric_limits<float>::infinity();
        }
        Multiband d(values + Values{{"mode", mode}}), clean(values + Values{{"mode", mode}});
        d.run({&l, &r});
        Samples cl = left, cr = right;
        clean.run({&cl, &cr});
        CHECK(allFinite(slice(l, at(0.36))) && allFinite(slice(r, at(0.36))));
        CHECK(maxAbs(slice(l, at(0.36))) < 10.0);
        CHECK_ALLCLOSE(slice(l, at(0.9)), slice(cl, at(0.9)), 0.0, 1e-5);
        CHECK_ALLCLOSE(slice(r, at(0.9)), slice(cr, at(0.9)), 0.0, 1e-5);
    }
}

TEST_CASE("multiband: one channel plays as the left of two that are the same") {
    const Values values = base(everyBand("above_ratio", 4.f) + everyBand("below_ratio", 2.f));
    const Samples x = mix({noise(kSampleRate, 14, 0.2f), tone(300.0, 1.0, 0.3)});
    const std::vector<Change> changes = {{at(0.3), "xover_low", 400.f}, {at(0.5), "mode", 1.f}};
    Multiband mono(values), two(values);
    const Samples alone = mono.play(x, changes);
    Samples l = x, r = x;
    two.run({&l, &r}, changes);
    CHECK_ARRAY_EQUAL(alone, l);
    CHECK_ARRAY_EQUAL(r, l);
}

TEST_CASE("multiband: its stereo is linked, and a silent side stays silent") {
    Multiband d(single({{"mid_above", -20.f}, {"mid_above_ratio", 4.f}}));
    Samples l = tone(1000.0, 1.0, 0.5), r(l.size(), 0.f);
    d.run({&l, &r});
    CHECK_APPROX_TOL(levelDb(l), db(0.5) + (db(0.5) + 20.0) * -0.75, 0.0, 0.05);
    CHECK(allEqual(r, 0.0));
    // A tone on the right compresses the left too (both get the louder side's gain).
    Multiband linked(single({{"mid_above", -20.f}, {"mid_above_ratio", 4.f}}));
    Samples quiet = tone(1000.0, 1.0, 0.05), loud = tone(1000.0, 1.0, 0.5);
    linked.run({&quiet, &loud});
    CHECK_APPROX_TOL(levelDb(quiet), db(0.05) + (db(0.5) + 20.0) * -0.75, 0.0, 0.05);

    // RMS on two channels that differ reads the louder channel's power, as on one: each channel's mean square,
    // the larger taken (the mean of the larger square each sample would read about 2.1 dB hot on these).
    Multiband rmsMode(single({{"mode", 1.f}}));
    Samples sine = tone(1000.0, 0.5, 0.5), cosine(sine.size());
    for (size_t i = 0; i < cosine.size(); ++i)
        cosine[i] = static_cast<float>(0.5 * std::cos(2.0 * kPi * 1000.0 * static_cast<double>(i) / kSampleRate));
    rmsMode.run({&sine, &cosine});
    CHECK_APPROX_TOL(rmsMode.display("mid_in").back(), db(0.5) - 3.0103, 0.0, 0.05);
    Multiband wide(single({{"mode", 1.f}}));
    Samples left = noise(kSampleRate, 21, 0.3f), right = noise(kSampleRate, 22, 0.3f);
    const double louder = 20.0 * std::log10(std::max(rms(slice(left, at(0.5))), rms(slice(right, at(0.5)))));
    wide.run({&left, &right});
    const std::vector<float> read = wide.display("mid_in");
    double mean = 0.0;
    for (size_t i = read.size() / 2; i < read.size(); ++i) mean += read[i];
    mean /= static_cast<double>(read.size() - read.size() / 2);
    INFO("decorrelated noise: read " + std::to_string(mean) + " dB, the louder channel " + std::to_string(louder));
    CHECK_APPROX_TOL(mean, louder, 0.0, 0.5);
}

TEST_CASE("multiband: its tail covers the crossovers' ringing") {
    for (const float crossover : {30.f, 120.f}) {
        INFO(std::to_string(crossover) + " Hz");
        // Unequal band gains, where the LR4 halves' repeated poles don't cancel.
        Multiband d({{"xover_low", crossover}, {"low_out", -24.f}, {"high_out", 24.f}});
        const int tail = d.processor().tailSamples();
        CHECK(tail >= (crossover < 100.f ? 0.1 : 0.025) * kSampleRate);
        const Samples h = d.play(impulse(kSampleRate));
        const double after = energy(slice(h, tail)), total = energy(h);
        INFO("after the tail: " + std::to_string(10.0 * std::log10(after / total + 1e-300)) + " dB");
        CHECK(after < 1e-12 * total);
    }
}

TEST_CASE("multiband: its displays show each band's level in and out and its gain change") {
    // Three bands, the mid compressing 4:1 above -20 dB; 1 kHz at 0.5 (97.5 % of it in the mid band).
    Multiband d(base({{"mid_above", -20.f}, {"mid_above_ratio", 4.f}}));
    d.play(tone(1000.0, 100 * 256.0 / kSampleRate, 0.5));
    for (const char* band : {"low", "mid", "high"})
        for (const char* kind : {"_in", "_out", "_gain"})
            CHECK_EQ(d.display(std::string(band) + kind).size(), size_t{100});
    const double in = db(0.5 * lowShare(1000.0, 2500.0));  // -6.24
    CHECK_APPROX_TOL(d.display("mid_in").back(), in, 0.0, 0.1);
    CHECK_APPROX_TOL(d.display("mid_gain").back(), (in + 20.0) * -0.75, 0.0, 0.1);  // -10.31
    CHECK_APPROX_TOL(d.display("mid_out").back(), d.display("mid_in").back() + d.display("mid_gain").back(), 0.0, 0.01);
    CHECK(d.display("low_in").back() <= -60.f);
    CHECK_EQ(d.display("low_gain").back(), 0.f);
    // With no processing, in and out are the same.
    CHECK_EQ(d.display("high_in").back(), d.display("high_out").back());

    // A band switched off publishes the floor and no gain (the mid band shows what it does).
    Multiband off(base({{"high_on", 0.f}, {"mid_above", -20.f}, {"mid_above_ratio", 4.f}}));
    off.play(tone(10000.0, 0.1, 0.5));
    CHECK_EQ(off.display("high_in").back(), mb::kDisplayFloorDb);
    CHECK_EQ(off.display("high_out").back(), mb::kDisplayFloorDb);
    CHECK_EQ(off.display("high_gain").back(), 0.f);
    CHECK_APPROX_TOL(off.display("mid_in").back(), db(0.5), 0.0, 0.1);
    CHECK(off.display("mid_gain").back() < -10.f);
    // Silence reads the floor.
    Multiband silent;
    silent.play(Samples(2560, 0.f));
    CHECK_EQ(silent.display("mid_in").back(), mb::kDisplayFloorDb);
    CHECK_EQ(silent.display("mid_out").back(), mb::kDisplayFloorDb);
}

TEST_CASE("multiband: what it costs") {
    Multiband d(everyBand("above_ratio", 4.f) + everyBand("below_ratio", 4.f));
    Samples l = noise(static_cast<size_t>(10 * kSampleRate), 15, 0.3f), r = noise(l.size(), 16, 0.3f);
    const auto start = std::chrono::steady_clock::now();
    d.run({&l, &r});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("multiband: 10 s of stereo at 48 kHz in %.3f s (%.2f %% of a core)\n", seconds, seconds * 10.0);
    CHECK(allFinite(l));
#ifdef NDEBUG
    CHECK(seconds < 0.5);  // (a bound for pathologies only)
#endif
}

TEST_CASE("multiband: the output doesn't depend on how blocks are cut") {
    Samples x = smoothSine(220.0, 1.0);
    const Samples hiss = noise(x.size(), 17, 0.05f);
    for (size_t i = 0; i < x.size(); ++i) x[i] += hiss[i];
    const Values values = everyBand("above_ratio", 4.f) + everyBand("below_ratio", 4.f);
    const std::vector<Change> changes = {{at(0.2), "xover_high", 600.f},
                                         {at(0.4), "mid_above", -35.f},
                                         {at(0.6), "high_on", 0.f},
                                         {at(0.7), "mode", 0.f}};
    std::vector<Samples> outs;
    std::vector<std::vector<float>> displays;
    for (const int block : {1, 13, 256, 1024}) {
        Multiband d(values);
        Samples l = x, r = hiss;
        d.run({&l, &r}, changes, block);
        outs.push_back(interleave({l, r}));
        std::vector<float> all;
        for (const char* band : {"low", "mid", "high"}) {
            for (const char* kind : {"_in", "_out", "_gain"}) {
                const std::vector<float> values = d.display(std::string(band) + kind);
                all.insert(all.end(), values.begin(), values.end());
            }
        }
        displays.push_back(all);
    }
    for (size_t i = 1; i < outs.size(); ++i) {
        INFO(std::to_string(i));
        CHECK_ARRAY_EQUAL(outs[i], outs[0]);
        CHECK_ARRAY_EQUAL(displays[i], displays[0]);
    }
    CHECK_EQ(displays[0].size(), size_t{9 * (kSampleRate / 256)});
}
