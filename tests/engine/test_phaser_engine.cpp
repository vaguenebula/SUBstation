// The built-in Phaser-Flanger: its notches are where its design puts them (the
// curve its editor draws), Spread moves them apart and the LFOs sweep them,
// every shape as its design draws it, bent by Duty, LFO 2 mixed in, stereo by
// Phase or Spin; the flanger's comb and its feedback are exact to the sample,
// the doubler's copy too; the envelope, Safe Bass, Warmth (its saturation
// antialiased), Output and Dry/Wet do what they say; a change of mode or of
// Notches lands on the new setting exactly; every control changes without a
// click, and a sweep into the stages' limits bends smoothly; automation plays
// to the sample; synced LFOs follow the song and random ones repeat; reset and
// a new rate start it cleanly, silence rings out to exact zeros (nothing
// denormal on the way), its tail covers its ringing; it stays stable at the
// extremes, one channel plays as either of two, and its displays are what
// plays.

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "Engine.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/DisperserDesign.h"
#include "builtin/DspBlocks.h"
#include "builtin/PhaserDesign.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;
namespace phaser = sub::phaser;
namespace disperser = sub::disperser;

namespace {

constexpr int kBlock = 1024;  // the renderer's largest block (Renderer::kMaxBlock)

using Values = std::vector<std::pair<std::string, float>>;

// A parameter's change at a frame, as automation hands it over.
struct Change {
    int64_t frame;
    std::string id;
    float value;
};

// The settings most tests start from: no sweep and no feedback (the rest the
// defaults: Phaser, 4 notches at 1 kHz, Spread 50 %, Dry/Wet 50 %), and more.
Values still(const Values& more = {}) {
    Values values = {{"amount", 0.f}, {"feedback", 0.f}};
    values.insert(values.end(), more.begin(), more.end());
    return values;
}

// A Phaser-Flanger on its own, outside an engine, at any sample rate: processed
// in blocks, its changes handed over as automation (so its blocks split there)
// as the renderer does. Its transport is stopped unless playing() says so.
class Phaser {
public:
    explicit Phaser(double rate = kSampleRate, const Values& values = {})
        : processor_(sub::BuiltinRegistry::instance().create("phaser")), rate_(rate) {
        for (const auto& [id, value] : values) set(id, value);
        processor_->prepare(rate, kBlock);
    }

    sub::Processor& processor() { return *processor_; }
    void prepare(double rate) {
        rate_ = rate;
        processor_->prepare(rate, kBlock);
    }

    int index(const std::string& id) const {
        const auto& params = processor_->params();
        for (size_t i = 0; i < params.size(); ++i)
            if (params[i].id == id) return static_cast<int>(i);
        INFO(id);
        REQUIRE(false);
        return -1;
    }
    void set(const std::string& id, float value) { processor_->setParam(index(id), value); }

    // The transport: playing (or not) at `tempo`, each block's beat counted from
    // `startBeat` at the frame this is called on; `beatAt`, if set, gives a
    // block's beat from its frame instead (a loop, a locate).
    void playing(bool on, double tempo = 120.0, double startBeat = 0.0) {
        playing_ = on;
        tempo_ = tempo;
        startBeat_ = startBeat;
        since_ = played_;
    }
    std::function<double(int64_t)> beatAt;

    // Processes one or two channels of equal length in place, `block` frames at a time.
    void run(const std::vector<Samples*>& channels, const std::vector<Change>& changes = {}, int block = 256) {
        const auto frames = static_cast<int64_t>(channels[0]->size());
        sub::ProcessContext ctx;
        ctx.sampleRate = rate_;
        ctx.offline = true;
        ctx.playing = playing_;
        ctx.tempo = tempo_;
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
            const int64_t at = played_ + start;
            ctx.samplePos = at;
            ctx.beatPos = beatAt ? beatAt(at) : startBeat_ + static_cast<double>(at - since_) / ctx.samplesPerBeat();
            processor_->process(ctx, pointers, static_cast<int>(channels.size()), n);
            processor_->clearAutomation();
        }
        played_ += frames;
    }
    // One channel: what comes out.
    Samples play(Samples mono, const std::vector<Change>& changes = {}, int block = 256) {
        run({&mono}, changes, block);
        return mono;
    }

    // Every value of display `id` since the last call.
    std::vector<float> display(const std::string& id) {
        const std::vector<sub::DisplayInfo> infos = processor_->displays();
        for (size_t i = 0; i < infos.size(); ++i) {
            if (infos[i].id != id) continue;
            std::vector<float> out;
            positions_[i] = processor_->readDisplay(static_cast<int>(i), positions_[i], out);
            return out;
        }
        INFO(id);
        REQUIRE(false);
        return {};
    }

private:
    std::shared_ptr<sub::Processor> processor_;
    double rate_;
    bool playing_ = false;
    double tempo_ = 120.0, startBeat_ = 0.0;
    int64_t played_ = 0, since_ = 0;
    uint64_t positions_[16] = {};
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

// The largest 6th difference over [from, to): a steep high-pass, about 64 times
// (36 dB) more sensitive at Nyquist than at a quarter of the sample rate and
// 10^5 times more than at 2 kHz (48 kHz). A step of d shows as up to 20 d, a
// kink (a change of slope s) as 6 s; a smooth signal well below Nyquist hardly at all.
double clickiness(const Samples& x, int64_t from = 0, int64_t to = -1) {
    std::vector<double> d(x.begin(), x.end());
    for (int k = 0; k < 6; ++k)
        for (size_t i = d.size() - 1; i > 0; --i) d[i] -= d[i - 1];
    if (to < 0 || to > static_cast<int64_t>(d.size())) to = static_cast<int64_t>(d.size());
    double worst = 0.0;
    for (int64_t i = std::max<int64_t>(from, 6); i < to; ++i)
        worst = std::max(worst, std::abs(d[static_cast<size_t>(i)]));
    return worst;
}

// The transform of `x` at `freq` (Hz) over [from, from + length) (all of it by default).
std::complex<double> transform(const Samples& x, double freq, double rate = kSampleRate, size_t from = 0,
                               size_t length = 0) {
    if (length == 0) length = x.size() - from;
    const std::complex<double> step = std::polar(1.0, -2.0 * kPi * freq / rate);
    std::complex<double> z = std::polar(1.0, -2.0 * kPi * freq / rate * static_cast<double>(from)), sum = 0.0;
    for (size_t n = from; n < from + length; ++n) {
        sum += static_cast<double>(x[n]) * z;
        z *= step;
        if ((n & 1023) == 0) z /= std::abs(z);  // (a recurrence: kept on the circle)
    }
    return sum;
}

double db(double gain) { return 20.0 * std::log10(std::max(gain, 1e-30)); }

// A steady tone's amplitude at `freq` over [from, from + length) (a whole number of its cycles).
double toneLevel(const Samples& x, double freq, size_t from, size_t length, double rate = kSampleRate) {
    return 2.0 * std::abs(transform(x, freq, rate, from, length)) / static_cast<double>(length);
}

double energy(const Samples& x) {
    double sum = 0.0;
    for (const float v : x) sum += static_cast<double>(v) * v;
    return sum;
}

size_t powerOfTwoAtLeast(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

double frac(double x) { return x - std::floor(x); }
double wrapHalf(double x) { return x - std::floor(x + 0.5); }                   // into -0.5..0.5
double wrapAngle(double x) { return x - 2.0 * kPi * std::floor(x / (2.0 * kPi) + 0.5); }  // into -π..π

double minOf(const std::vector<float>& v) { return *std::min_element(v.begin(), v.end()); }
double maxOfValues(const std::vector<float>& v) { return *std::max_element(v.begin(), v.end()); }

int tailOf(Phaser& p) { return p.processor().tailSamples(); }

// The CPU time this thread has used, in seconds: what a piece of work costs, whatever else
// the machine is doing (the wall clock would count the time other processes had the core).
double threadSeconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    const auto ticks = [](const FILETIME& t) {
        return static_cast<double>((static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime);
    };
    return 1e-7 * (ticks(kernel) + ticks(user));  // (100 ns ticks, counted at the scheduler's ~16 ms)
#else
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return static_cast<double>(t.tv_sec) + 1e-9 * static_cast<double>(t.tv_nsec);
#endif
}

}  // namespace

TEST_CASE("phaser-flanger: listed with its parameters") {
    const sub::BuiltinInfo info = builtinInfo("phaser");
    CHECK_EQ(info.name, std::string("Phaser-Flanger"));
    CHECK(!info.isInstrument());
    CHECK(paramIds(info.params) ==
          (std::vector<std::string>{"mode",       "notches",     "center",    "spread",   "blend",   "flange_time",
                                    "doubler_time", "amount",    "feedback",  "fb_invert", "sync",   "freq",
                                    "rate",       "wave",        "duty",      "spin_on",  "phase",   "spin",
                                    "lfo2_mix",   "sync2",       "freq2",     "rate2",    "env_on",  "env_amount",
                                    "env_attack", "env_release", "safe_bass", "warmth",   "output",  "mix"}));
    const auto param = [&](const std::string& id) -> const sub::ParamInfo& {
        for (const sub::ParamInfo& p : info.params)
            if (p.id == id) return p;
        INFO(id);
        REQUIRE(false);
        return info.params[0];
    };
    CHECK(param("mode").valueLabels == (std::vector<std::string>{"Phaser", "Flanger", "Doubler"}));
    CHECK_EQ(param("mode").defaultValue, 0.f);
    const sub::ParamInfo& notches = param("notches");
    CHECK_EQ(notches.minValue, 1.f);
    CHECK_EQ(notches.maxValue, 42.f);
    CHECK_EQ(notches.stepCount(), 41);  // whole notches, automation too
    CHECK_EQ(notches.fromNormalized(notches.toNormalized(17.f)), 17.f);
    CHECK_EQ(notches.defaultValue, 4.f);
    const sub::ParamInfo& center = param("center");
    CHECK(center.isLog());
    CHECK_EQ(center.minValue, 70.f);
    CHECK_EQ(center.maxValue, 18500.f);
    CHECK_EQ(center.defaultValue, 1000.f);
    CHECK_EQ(center.unit, std::string("Hz"));
    for (const char* id : {"freq", "freq2"}) {
        INFO(id);
        CHECK(param(id).isLog());
        CHECK_APPROX(param(id).minValue, 0.01);
        CHECK_EQ(param(id).maxValue, 5.f);
    }
    CHECK(param("wave").valueLabels == phaser::waveLabels());
    CHECK(param("wave").valueLabels ==
          (std::vector<std::string>{"Sine", "Triangle", "Triangle Analog", "Triangle 8", "Triangle 16", "Saw Up",
                                    "Saw Down", "Rectangle", "Random", "Random S&H"}));
    CHECK_EQ(param("wave").defaultValue, 1.f);  // Triangle, as Live opens it
    for (const char* id : {"rate", "rate2"}) {
        INFO(id);
        CHECK(param(id).valueLabels == sub::dsp::syncedDivisionLabels());
        CHECK_EQ(param(id).maxValue, static_cast<float>(sub::dsp::syncedDivisionLabels().size() - 1));
    }
    CHECK_EQ(param("rate").valueLabels[static_cast<size_t>(param("rate").defaultValue)], std::string("1 Bar"));
    CHECK_EQ(param("rate").defaultValue, 15.f);
    CHECK_EQ(param("rate2").valueLabels[static_cast<size_t>(param("rate2").defaultValue)], std::string("1/4"));
    CHECK_EQ(param("rate2").defaultValue, 10.f);
    CHECK(param("flange_time").isLog());
    CHECK_APPROX(param("flange_time").minValue, 0.1);
    CHECK_EQ(param("flange_time").maxValue, 20.f);
    CHECK(param("doubler_time").isLog());
    CHECK_EQ(param("doubler_time").minValue, 20.f);
    CHECK_EQ(param("doubler_time").maxValue, 150.f);
    CHECK(param("safe_bass").isLog());
    CHECK_EQ(param("safe_bass").minValue, 5.f);
    CHECK_EQ(param("safe_bass").maxValue, 3000.f);
    CHECK_EQ(param("safe_bass").defaultValue, 5.f);  // off
    CHECK_EQ(param("phase").unit, std::string("°"));
    CHECK_EQ(param("phase").maxValue, 360.f);
    CHECK_EQ(param("duty").minValue, -100.f);
    CHECK_EQ(param("env_amount").minValue, -100.f);
    CHECK_EQ(param("mix").name, std::string("Dry/Wet"));
    CHECK_EQ(param("mix").defaultValue, 50.f);  // the deepest notches
    for (const char* id : {"fb_invert", "sync", "spin_on", "sync2", "env_on"}) {
        INFO(id);
        CHECK(param(id).valueLabels == (std::vector<std::string>{"Off", "On"}));
        CHECK_EQ(param(id).defaultValue, 0.f);
    }
    for (const sub::ParamInfo& p : info.params) {
        INFO(p.id);
        CHECK(p.automatable);
        CHECK(!p.hidden);
    }

    // The engine makes it: no latency (the delay is the effect), a tail, its displays.
    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "phaser", -1);
    const sub::ProcessorInfo processor = engine.processorInfo(id);
    CHECK_EQ(processor.name, std::string("Phaser-Flanger"));
    CHECK_EQ(processor.latency, 0);
    CHECK(processor.tail > 0);
    const std::vector<sub::DisplayInfo> displays = engine.processorDisplays(id);
    std::vector<std::string> ids;
    for (const sub::DisplayInfo& d : displays) {
        ids.push_back(d.id);
        CHECK_EQ(d.samplesPerValue, 256);
    }
    CHECK(ids == (std::vector<std::string>{"phase", "phase_r", "lfo", "mod", "env", "sweep_l", "sweep_r", "q_l", "q_r",
                                           "input", "output"}));
}

TEST_CASE("phaser-flanger: the phaser's notches are where its design puts them") {
    struct Case {
        int notches;
        float spread;
    };
    for (const Case& k : {Case{1, 50.f}, Case{4, 50.f}, Case{4, 0.f}, Case{4, 100.f}, Case{12, 50.f}}) {
        INFO(std::to_string(k.notches) + " notches, spread " + std::to_string(k.spread));
        const double q = phaser::qOfSpread(k.spread / 100.0);
        const std::vector<double> notches = phaser::notchFrequencies(k.notches, 1000.0, q, kSampleRate);
        REQUIRE(notches.size() == static_cast<size_t>(k.notches));
        // The closed form against the stage itself: N stages turn each an odd number of half circles.
        const disperser::Stage stage = disperser::design(1000.0, q, kSampleRate);
        double worstPhase = 0.0;
        for (size_t i = 0; i < notches.size(); ++i) {
            if (i > 0) CHECK(notches[i] > notches[i - 1]);
            const double w = 2.0 * kPi * notches[i] / kSampleRate;
            const double turned = k.notches * std::arg(disperser::response(stage, w));
            worstPhase = std::max(worstPhase, std::abs(wrapAngle(turned + (2.0 * static_cast<double>(i) + 1.0) * kPi)));
        }
        CHECK(worstPhase < 1e-9);

        // What plays: deep notches there, and the design's curve everywhere else.
        Phaser p(kSampleRate, still({{"notches", static_cast<float>(k.notches)}, {"spread", k.spread}}));
        const Samples h = p.play(impulse(1 << 15));
        for (const double f : notches) {
            INFO("notch at " + std::to_string(f));
            CHECK(db(std::abs(transform(h, f))) < -40.0);
        }
        phaser::Response r;
        r.notches = k.notches;
        r.centerHz = 1000.0;
        r.q = q;
        r.mix = 0.5;
        double worst = 0.0;
        for (int i = 0; i <= 200; ++i) {
            const double f = 20.0 * std::pow(1000.0, i / 200.0);
            const double want = phaser::responseDb(r, f, kSampleRate);
            if (want < -30.0) continue;
            worst = std::max(worst, std::abs(db(std::abs(transform(h, f))) - want));
        }
        INFO("off the design by " + std::to_string(worst) + " dB at most");
        CHECK(worst < 0.1);
    }

    // One notch: exactly at the centre; a tone there all but gone.
    CHECK_APPROX(phaser::notchFrequencies(1, 1000.0, phaser::qOfSpread(0.5), kSampleRate)[0], 1000.0);
    Phaser one(kSampleRate, still({{"notches", 1.f}}));
    const Samples tone = smoothSine(1000.0, 1.0);
    const Samples out = one.play(tone);
    CHECK(rms(slice(out, kSampleRate / 2)) < 1e-3 * rms(slice(tone, kSampleRate / 2)));  // below -60 dB

    // stagePhase is the stage's own phase, unwrapped from 0 down to -2π.
    double worst = 0.0;
    for (const double rate : {44100.0, 48000.0}) {
        for (const double centre : {70.0, 1000.0, 18500.0}) {
            for (const double q : {0.15, 0.866, 5.0}) {
                const disperser::Stage s = disperser::design(centre, q, rate);
                for (int i = 0; i < 200; ++i) {
                    const double f = 20.0 * std::pow(0.4999 * rate / 20.0, i / 199.0);
                    const double closed = phaser::stagePhase(f, centre, q, rate);
                    CHECK(closed <= 0.0);
                    CHECK(closed > -2.0 * kPi);
                    const double own = std::arg(disperser::response(s, 2.0 * kPi * f / rate));
                    worst = std::max(worst, std::abs(wrapAngle(closed - own)));
                }
            }
        }
    }
    CHECK(worst < 1e-9);
}

TEST_CASE("phaser-flanger: Spread moves the notches apart") {
    const auto notchesAt = [](double spread) {
        return phaser::notchFrequencies(4, 1000.0, phaser::qOfSpread(spread), kSampleRate);
    };
    const auto ratio = [&](double spread) {
        const std::vector<double> n = notchesAt(spread);
        return n.back() / n.front();
    };
    CHECK(ratio(0.0) < 1.7);
    CHECK_APPROX_REL(ratio(0.5), 9.54, 0.01);
    CHECK(ratio(1.0) > 100.0);
    const std::vector<std::pair<double, std::vector<double>>> expected = {
        {0.0, {787.7, 959.5, 1042.2, 1269.0}},
        {0.5, {322.0, 789.5, 1266.2, 3071.7}},
        {1.0, {62.0, 324.5, 3048.8, 12437.4}},
    };
    for (const auto& [spread, want] : expected) {
        const std::vector<double> got = notchesAt(spread);
        for (size_t i = 0; i < want.size(); ++i) CHECK_NEAR(got[i], want[i], 0.1);
    }
    CHECK_APPROX(phaser::qOfSpread(0.0), 5.0);
    CHECK_APPROX(phaser::qOfSpread(0.5), std::sqrt(0.03) * 5.0);
    CHECK_APPROX(phaser::qOfSpread(1.0), 0.15);
}

TEST_CASE("phaser-flanger: the LFO sweeps the notches") {
    // A 1 Hz sine, all the way: three octaves either side of 1 kHz.
    Phaser p(kSampleRate, still({{"notches", 1.f}, {"amount", 100.f}, {"wave", 0.f}, {"freq", 1.f}}));
    p.play(smoothSine(440.0, 2.0));
    const std::vector<float> phase = p.display("phase"), lfo = p.display("lfo"), sweep = p.display("sweep_l"),
                             q = p.display("q_l");
    REQUIRE(phase.size() == 375);
    double phaseOff = 0.0, lfoOff = 0.0, sweepOff = 0.0, qOff = 0.0;
    for (size_t k = 0; k < phase.size(); ++k) {
        phaseOff = std::max(phaseOff, std::abs(wrapHalf(phase[k] - 256.0 * static_cast<double>(k + 1) / kSampleRate)));
        lfoOff = std::max(lfoOff, std::abs(lfo[k] - std::sin(2.0 * kPi * phase[k])));
        const double want = phaser::phaserCenterHz(1000.0, 0.0, lfo[k], kSampleRate);
        sweepOff = std::max(sweepOff, std::abs(sweep[k] / want - 1.0));
        qOff = std::max(qOff, std::abs(q[k] - phaser::qOfSpread(0.5)));
    }
    CHECK(phaseOff < 1e-6);
    CHECK(lfoOff < 1e-5);
    INFO("the sweep lags the LFO by " + std::to_string(100.0 * sweepOff) + " % at most");
    CHECK(sweepOff < 0.03);  // (the modulation's 1 ms smoothing)
    CHECK(qOff < 1e-6);
    CHECK_APPROX_REL(minOf(sweep), 125.0, 0.02);
    CHECK_APPROX_REL(maxOfValues(sweep), 8000.0, 0.02);

    // Blend 1: the modulation moves Spread instead, the centre stays.
    Phaser spread(kSampleRate,
                  still({{"notches", 1.f}, {"amount", 100.f}, {"wave", 0.f}, {"freq", 1.f}, {"blend", 1.f}}));
    spread.play(smoothSine(440.0, 2.0));
    const std::vector<float> centre = spread.display("sweep_l"), qs = spread.display("q_l");
    CHECK_APPROX_REL(minOf(centre), 1000.0, 0.001);
    CHECK_APPROX_REL(maxOfValues(centre), 1000.0, 0.001);
    CHECK_APPROX_REL(maxOfValues(qs), phaser::qOfSpread(0.0), 0.02);
    CHECK_APPROX_REL(minOf(qs), phaser::qOfSpread(1.0), 0.02);
}

TEST_CASE("phaser-flanger: the LFO's shapes and Duty, LFO 2, and Triangle Analog's rate reach the modulation") {
    // Every shape at Duty 0 and ±80 %: the LFO's value is the shape's at its phase (away from a
    // shape's jumps, where a value a float's rounding away would be the other side's).
    for (int wave = 0; wave < 10; ++wave) {
        for (const float duty : {0.f, 80.f, -80.f}) {
            INFO(phaser::waveLabels()[static_cast<size_t>(wave)] + ", Duty " + std::to_string(duty));
            Phaser p(kSampleRate, still({{"amount", 100.f}, {"wave", static_cast<float>(wave)}, {"freq", 1.f},
                                         {"duty", duty}}));
            p.play(Samples(2 * kSampleRate, 0.f));
            const std::vector<float> phase = p.display("phase"), lfo = p.display("lfo");
            const auto shape = static_cast<phaser::Wave>(wave);
            size_t compared = 0;
            double worst = 0.0;
            for (size_t k = 0; k < phase.size(); ++k) {
                const double at = phase[k];
                const double seconds = 256.0 * static_cast<double>(k + 1) / kSampleRate;
                const auto cycle = static_cast<uint32_t>(std::llround(seconds - at));
                const auto value = [&](double ph) { return phaser::waveValue(shape, ph, cycle, duty / 100.0, 1.0); };
                if (at < 1e-3 || at > 1.0 - 1e-3 || std::abs(value(at - 1e-4) - value(at + 1e-4)) > 0.01) continue;
                worst = std::max(worst, std::abs(static_cast<double>(lfo[k]) - value(at)));
                ++compared;
            }
            CHECK(compared > 300);
            CHECK(worst < 1e-5);
        }
    }
    // Duty bends the shape: at 80 %, the sine's first half takes 86 % of the cycle.
    CHECK_APPROX(phaser::waveValue(phaser::Wave::Sine, 0.43, 0, 0.8, 1.0), 1.0);
    CHECK_APPROX(phaser::waveValue(phaser::Wave::Sine, 0.93, 0, 0.8, 1.0), -1.0);
    CHECK_EQ(phaser::waveValue(phaser::Wave::Rectangle, 0.85, 0, 0.8, 1.0), 1.f);
    CHECK_EQ(phaser::waveValue(phaser::Wave::Rectangle, 0.87, 0, 0.8, 1.0), -1.f);

    // LFO 2 alone (its Mix 100 %): the modulation is its triangle at Freq 2, whatever LFO 1 does
    // (within the modulation's 1 ms smoothing, which rounds the triangle's turns).
    const auto triangle = [](double phase) {
        return static_cast<double>(sub::dsp::Lfo::shape(sub::dsp::LfoShape::Triangle, frac(phase), 0));
    };
    const Values two = still({{"amount", 100.f}, {"wave", 0.f}, {"freq", 0.37f}, {"lfo2_mix", 100.f}, {"freq2", 2.f}});
    {
        Phaser p(kSampleRate, two);
        p.play(Samples(2 * kSampleRate, 0.f));
        const std::vector<float> mod = p.display("mod");
        double worst = 0.0;
        for (size_t k = 0; k < mod.size(); ++k) {
            const double seconds = 256.0 * static_cast<double>(k + 1) / kSampleRate;
            worst = std::max(worst, std::abs(mod[k] - triangle(2.0 * seconds)));
        }
        CHECK(worst < 0.02);
    }
    // Synced (1/4 at 120 BPM: 2 Hz again), from the song: a quarter of a beat in, a quarter of a cycle on.
    {
        Values synced = two;
        synced.insert(synced.end(), {{"sync2", 1.f}, {"rate2", 10.f}});
        Phaser p(kSampleRate, synced);
        p.playing(true, 120.0, 0.25);
        p.play(Samples(2 * kSampleRate, 0.f));
        const std::vector<float> mod = p.display("mod");
        double worst = 0.0;
        for (size_t k = 0; k < mod.size(); ++k) {
            const double seconds = 256.0 * static_cast<double>(k + 1) / kSampleRate;
            worst = std::max(worst, std::abs(mod[k] - triangle(0.25 + 2.0 * seconds)));
        }
        CHECK(worst < 0.02);
    }
    // Half and half: the average of the two.
    {
        Values half = two;
        half.emplace_back("lfo2_mix", 50.f);
        Phaser p(kSampleRate, half);
        p.play(Samples(2 * kSampleRate, 0.f));
        const std::vector<float> mod = p.display("mod"), lfo = p.display("lfo");
        double worst = 0.0;
        for (size_t k = 0; k < mod.size(); ++k) {
            const double seconds = 256.0 * static_cast<double>(k + 1) / kSampleRate;
            worst = std::max(worst, std::abs(mod[k] - (0.5 * lfo[k] + 0.5 * triangle(2.0 * seconds))));
        }
        CHECK(worst < 0.02);
    }

    // Triangle Analog follows its rate: all but square at 0.5 Hz (reaching 1), a quieter rounded
    // triangle at 5 Hz (0.46 high).
    for (const auto& [freq, peak] : {std::pair{0.5f, 1.0}, std::pair{5.f, 0.46}}) {
        INFO(std::to_string(freq) + " Hz");
        Phaser p(kSampleRate, still({{"amount", 100.f}, {"wave", 2.f}, {"freq", freq}}));
        p.play(Samples(4 * kSampleRate, 0.f));
        const std::vector<float> lfo = p.display("lfo");
        CHECK_APPROX_REL(maxOfValues(lfo), peak, 0.02);
        CHECK_APPROX_REL(-minOf(lfo), peak, 0.02);
    }
}

TEST_CASE("phaser-flanger: stereo: the right LFO runs Phase ahead, or spins faster") {
    const Values sweeping = still({{"amount", 100.f}, {"wave", 0.f}, {"freq", 1.f}});
    // Phase 180: the right sweeps the other way, mirrored about the centre (in log).
    {
        Phaser p(kSampleRate, sweeping);
        Samples l = smoothSine(440.0, 2.0), r = l;
        p.run({&l, &r});
        const std::vector<float> left = p.display("sweep_l"), right = p.display("sweep_r");
        double worst = 0.0;
        for (size_t k = 0; k < left.size(); ++k) worst = std::max(worst, std::abs(left[k] * right[k] / 1e6 - 1.0));
        CHECK(worst < 0.03);
    }
    // Phase 0: the same.
    {
        Values values = sweeping;
        values.emplace_back("phase", 0.f);
        Phaser p(kSampleRate, values);
        Samples l = smoothSine(440.0, 1.0), r = l;
        p.run({&l, &r});
        CHECK(p.display("sweep_l") == p.display("sweep_r"));
        CHECK_ARRAY_EQUAL(l, r);
    }
    // Spin 50 %: the right runs 1.5 times as fast, half a cycle ahead after a second.
    {
        Values values = sweeping;
        values.emplace_back("spin_on", 1.f);
        values.emplace_back("spin", 50.f);
        Phaser p(kSampleRate, values);
        Samples l(2 * kSampleRate, 0.f), r = l;
        p.run({&l, &r});
        const std::vector<float> left = p.display("phase"), right = p.display("phase_r");
        const auto apart = [&](double seconds) {
            const auto k = static_cast<size_t>(seconds * kSampleRate / 256.0) - 1;
            return frac(right[k] - left[k]);
        };
        CHECK_NEAR(wrapHalf(apart(1.0) - 0.5), 0.0, 0.01);
        CHECK_NEAR(wrapHalf(apart(2.0)), 0.0, 0.01);
    }
    // Phase automated 0 -> 270 at 1 s: it glides there the short way round (through 315°, not 90°).
    {
        Values values = sweeping;
        values.emplace_back("phase", 0.f);
        Phaser p(kSampleRate, values);
        Samples l(2 * kSampleRate, 0.f), r = l;
        p.run({&l, &r}, {{kSampleRate, "phase", 270.f}});
        const std::vector<float> left = p.display("phase"), right = p.display("phase_r");
        double largestStep = 0.0;
        double previous = 0.0;
        for (size_t k = 0; k < left.size(); ++k) {
            const double apart = frac(right[k] - left[k]);
            if (k > 0) largestStep = std::max(largestStep, std::abs(wrapHalf(apart - previous)));
            if (k > 0 && 256.0 * (k + 1) > kSampleRate && apart > 0.01) CHECK(apart > 0.74);  // never through 0.5
            previous = apart;
        }
        CHECK(largestStep < 0.05);
        const auto at = static_cast<size_t>(1.25 * kSampleRate / 256.0);
        CHECK_NEAR(wrapHalf(frac(right[at] - left[at]) - 0.75), 0.0, 0.01);
    }
}

TEST_CASE("phaser-flanger: the flanger is a comb at its delay, fed back to the sample") {
    // 1 ms: 48 samples. Half the input, and half of it 48 samples later.
    Phaser p(kSampleRate, still({{"mode", 1.f}, {"flange_time", 1.f}}));
    const Samples h = p.play(impulse(4096));
    Samples want(h.size(), 0.f);
    want[0] = want[48] = 0.5f;
    CHECK_ALLCLOSE(h, want, 0.0, 1e-7);
    // Its notches: an odd number of half cycles in 1 ms.
    CHECK(db(std::abs(transform(h, 500.0))) < -40.0);
    CHECK(db(std::abs(transform(h, 1500.0))) < -40.0);
    CHECK_NEAR(db(std::abs(transform(h, 1000.0))), 0.0, 0.05);

    // Feedback 50 %: each pass 0.475 of the last, inverted with Ø.
    for (const bool invert : {false, true}) {
        INFO(invert ? "inverted" : "as it is");
        Phaser fed(kSampleRate, {{"amount", 0.f}, {"mode", 1.f}, {"flange_time", 1.f}, {"feedback", 50.f},
                                 {"fb_invert", invert ? 1.f : 0.f}});
        const Samples echoes = fed.play(impulse(4096));
        const double sign = invert ? -1.0 : 1.0;
        CHECK_NEAR(echoes[48], 0.5, 1e-6);
        CHECK_NEAR(echoes[96], sign * 0.2375, 1e-6);
        CHECK_NEAR(echoes[144], 0.1128125, 1e-6);
        CHECK_NEAR(echoes[47], 0.0, 1e-7);
        CHECK_NEAR(echoes[95], 0.0, 1e-7);
    }
}

TEST_CASE("phaser-flanger: the doubler is a copy Time later") {
    Phaser wet(kSampleRate, still({{"mode", 2.f}, {"doubler_time", 30.f}, {"mix", 100.f}}));
    const Samples h = wet.play(impulse(4096));
    CHECK_EQ(nonzero(h), (std::vector<int64_t>{1440}));
    CHECK_NEAR(h[1440], 1.0, 1e-7);
    Phaser blend(kSampleRate, still({{"mode", 2.f}, {"doubler_time", 30.f}, {"mix", 25.f}}));
    const Samples b = blend.play(impulse(4096));
    CHECK_NEAR(b[0], 0.75, 1e-7);
    CHECK_NEAR(b[1440], 0.25, 1e-7);
    CHECK_EQ(nonzero(b), (std::vector<int64_t>{0, 1440}));
}

TEST_CASE("phaser-flanger: the modulation moves the delays") {
    Phaser flanger(kSampleRate,
                   still({{"mode", 1.f}, {"flange_time", 4.f}, {"amount", 100.f}, {"wave", 0.f}, {"freq", 1.f}}));
    flanger.play(Samples(2 * kSampleRate, 0.f));
    const std::vector<float> f = flanger.display("sweep_l");
    CHECK_APPROX_REL(minOf(f), 1.0, 0.03);  // two octaves either side
    CHECK_APPROX_REL(maxOfValues(f), 16.0, 0.03);
    CHECK(allEqual(flanger.display("q_l"), 0.0));
    Phaser doubler(kSampleRate,
                   still({{"mode", 2.f}, {"doubler_time", 40.f}, {"amount", 100.f}, {"wave", 0.f}, {"freq", 1.f}}));
    doubler.play(Samples(2 * kSampleRate, 0.f));
    const std::vector<float> d = doubler.display("sweep_l");
    CHECK_APPROX_REL(minOf(d), 34.0, 0.02);  // 15 % either side
    CHECK_APPROX_REL(maxOfValues(d), 46.0, 0.02);
}

TEST_CASE("phaser-flanger: the envelope follower moves the sweep with the input's level") {
    const auto run = [](float envOn, float envAmount) {
        auto p = std::make_unique<Phaser>(kSampleRate, still({{"notches", 1.f}, {"center", 200.f}, {"env_on", envOn},
                                                              {"env_amount", envAmount}, {"env_attack", 0.1f},
                                                              {"env_release", 50.f}}));
        Samples x = sine(1000.0, 0.5);
        x.resize(static_cast<size_t>(1.2 * kSampleRate), 0.f);
        p->play(x);
        return p;
    };
    const auto index = [](double seconds) { return static_cast<size_t>(seconds * kSampleRate / 256.0); };
    {
        auto p = run(1.f, 100.f);
        const std::vector<float> env = p->display("env"), sweep = p->display("sweep_l");
        for (size_t k = index(0.2); k < index(0.5) - 1; ++k) {
            CHECK(env[k] > 0.85f);
            CHECK(env[k] < 0.89f);
            CHECK(sweep[k] > 1150.f);
            CHECK(sweep[k] < 1300.f);
        }
        for (size_t k = index(1.0); k < env.size(); ++k) {
            CHECK_EQ(env[k], 0.f);
            CHECK_NEAR(sweep[k], 200.0, 1.0);
        }
    }
    {
        auto p = run(1.f, -100.f);  // the other way: down 2.6 octaves
        const std::vector<float> sweep = p->display("sweep_l");
        for (size_t k = index(0.2); k < index(0.5) - 1; ++k) CHECK_APPROX_REL(sweep[k], 200.0 / 6.17, 0.05);
    }
    {
        auto p = run(0.f, 100.f);  // off
        CHECK(allEqual(p->display("env"), 0.0));
        CHECK(allEqual(p->display("sweep_l"), 200.0));
    }
}

TEST_CASE("phaser-flanger: Safe Bass keeps the lows out of the effect") {
    // One notch at 100 Hz: a 100 Hz tone all but gone, unless Safe Bass keeps it dry.
    const Samples tone = smoothSine(100.0, 1.5);
    const size_t from = kSampleRate / 2, length = kSampleRate;  // (100 cycles)
    for (const float safe : {5.f, 800.f}) {
        Phaser p(kSampleRate, still({{"notches", 1.f}, {"center", 100.f}, {"safe_bass", safe}}));
        const Samples out = p.play(tone);
        const double level = db(toneLevel(out, 100.0, from, length) / 0.5);
        INFO("Safe Bass " + std::to_string(safe) + ": " + std::to_string(level) + " dB");
        if (safe < 10.f) {
            CHECK(level < -30.0);
        } else {
            CHECK_NEAR(level, 0.0, 0.5);
        }
    }
    // A 1 ms flanger above 200 Hz: its comb as it was, the bands adding up to an all-pass.
    Phaser comb(kSampleRate, still({{"mode", 1.f}, {"flange_time", 1.f}, {"safe_bass", 200.f}}));
    const Samples h = comb.play(impulse(1 << 15));
    CHECK(db(std::abs(transform(h, 1500.0))) < -40.0);
    CHECK_NEAR(db(std::abs(transform(h, 1000.0))), 0.0, 0.2);
    // Both as the design draws them.
    Phaser notch(kSampleRate, still({{"notches", 1.f}, {"center", 100.f}, {"safe_bass", 800.f}}));
    const Samples hn = notch.play(impulse(1 << 15));
    phaser::Response r;
    r.notches = 1;
    r.centerHz = 100.0;
    r.q = phaser::qOfSpread(0.5);
    r.safeBassHz = 800.0;
    phaser::Response rc;
    rc.mode = phaser::Mode::Flanger;
    rc.delayMs = 1.0;
    rc.safeBassHz = 200.0;
    for (const double f : {100.0, 300.0, 1000.0, 1500.0}) {
        INFO(std::to_string(f) + " Hz");
        for (const auto& [response, measured] : {std::pair{r, &hn}, std::pair{rc, &h}}) {
            const double want = phaser::responseDb(response, f, kSampleRate);
            if (want > -30.0) CHECK_NEAR(db(std::abs(transform(*measured, f))), want, 0.1);
        }
    }
}

TEST_CASE("phaser-flanger: Warmth darkens and saturates the effect") {
    const auto doubler = [](float warmth) {
        return Phaser(kSampleRate, still({{"mode", 2.f}, {"doubler_time", 20.f}, {"mix", 100.f}, {"warmth", warmth}}));
    };
    // 10 kHz at 0.1: as it is without Warmth; the 5 kHz one-pole's -6.4 dB with it.
    const Samples high = smoothSine(10000.0, 1.0, kSampleRate, 0.1);
    const size_t from = kSampleRate / 4, length = kSampleRate / 2;  // (5000 cycles)
    {
        Phaser p = doubler(0.f);
        CHECK_NEAR(db(toneLevel(p.play(high), 10000.0, from, length) / 0.1), 0.0, 0.01);
    }
    {
        Phaser p = doubler(100.f);
        const double level = db(toneLevel(p.play(high), 10000.0, from, length) / 0.1);
        CHECK(level > -7.0);
        CHECK(level < -6.0);
        // (quiet, the wet path is the filter the design draws: its saturation adds next to nothing)
        phaser::Response r;
        r.mode = phaser::Mode::Doubler;
        r.delayMs = 20.0;
        r.warmth = 1.0;
        r.mix = 1.0;
        CHECK_NEAR(level, db(std::abs(phaser::wetTransfer(r, 2.0 * kPi * 10000.0 / kSampleRate, kSampleRate))), 0.1);
        CHECK_NEAR(level, phaser::responseDb(r, 10000.0, kSampleRate), 0.1);
    }
    // 200 Hz at 0.9: a 3rd harmonic with it, none without.
    const Samples loud = smoothSine(200.0, 1.5, kSampleRate, 0.9);
    for (const float warmth : {0.f, 100.f}) {
        Phaser p = doubler(warmth);
        const Samples out = p.play(loud);
        const double third = db(toneLevel(out, 600.0, kSampleRate / 4, kSampleRate) /
                                toneLevel(out, 200.0, kSampleRate / 4, kSampleRate));
        INFO("Warmth " + std::to_string(warmth) + ": the 3rd harmonic at " + std::to_string(third) + " dB");
        if (warmth > 0.f) {
            CHECK(third > -35.0);
        } else {
            CHECK(third < -100.0);
        }
    }
    // Loud and high, the harmonics beyond Nyquist fold back (the saturation runs at the rate) but
    // far down: a 9 kHz tone's 3rd at 21 kHz and its 5th at 3 kHz, a 7 kHz tone's 5th at 13 kHz
    // (unantialiased, 20, 26 and 34 dB under the tone).
    for (const auto& [tone, folds, under] :
         {std::tuple{9000.0, std::vector<double>{21000.0, 3000.0}, std::vector<double>{28.0, 45.0}},
          std::tuple{7000.0, std::vector<double>{13000.0}, std::vector<double>{45.0}}}) {
        Phaser p = doubler(100.f);
        const Samples out = p.play(smoothSine(tone, 1.0, kSampleRate, 0.9));
        const double level = toneLevel(out, tone, kSampleRate / 4, kSampleRate / 2);
        for (size_t i = 0; i < folds.size(); ++i) {
            const double alias = db(toneLevel(out, folds[i], kSampleRate / 4, kSampleRate / 2) / level);
            INFO(std::to_string(tone) + " Hz folding to " + std::to_string(folds[i]) + " Hz: " +
                 std::to_string(alias) + " dB");
            CHECK(alias < -under[i]);
        }
    }
}

TEST_CASE("phaser-flanger: Output and Dry/Wet") {
    const Samples in = noise(kSampleRate, 3);
    {
        Phaser p(kSampleRate, {{"mix", 0.f}, {"output", 6.f}});
        const Samples out = p.play(in);
        Samples want(in.size());
        for (size_t i = 0; i < in.size(); ++i) want[i] = static_cast<float>(1.9952623 * in[i]);
        CHECK_ALLCLOSE(out, want, 1e-6, 1e-7);
    }
    // Fully dry, Output 0 dB, Safe Bass off: the input bit for bit, whatever the rest does.
    for (const float mode : {0.f, 1.f, 2.f}) {
        INFO("mode " + std::to_string(mode));
        Phaser p(kSampleRate, {{"mode", mode}, {"mix", 0.f}, {"feedback", 95.f}, {"warmth", 100.f},
                               {"amount", 100.f}, {"notches", 42.f}, {"wave", 9.f}, {"env_on", 1.f}});
        Samples l = in, r = noise(kSampleRate, 4);
        const Samples right = r;
        p.run({&l, &r});
        CHECK_ARRAY_EQUAL(l, in);
        CHECK_ARRAY_EQUAL(r, right);
    }
}

TEST_CASE("phaser-flanger: a change of mode lands on the new mode") {
    // F is a multiple of the 16-frame chunk, so the devices' chunks line up.
    const Samples in = noise(2 * kSampleRate, 6);
    constexpr int64_t F = 24000, kFade = 1440;
    const Values base = {{"feedback", 0.f}, {"amount", 50.f}};
    const auto device = [&](float mode) {
        Values values = base;
        values.emplace_back("mode", mode);
        return std::make_unique<Phaser>(kSampleRate, values);
    };
    // Into a delay mode: from the end of the fade, exactly a device in that mode all along
    // (every glide ran in the old mode too, and the line was written).
    for (const auto& [from, to] :
         {std::pair{0.f, 1.f}, std::pair{1.f, 2.f}, std::pair{0.f, 2.f}, std::pair{2.f, 1.f}}) {
        INFO(std::to_string(from) + " -> " + std::to_string(to));
        const Samples out = device(from)->play(in, {{F, "mode", to}});
        const Samples want = device(to)->play(in);
        CHECK_ARRAY_EQUAL(slice(out, F + kFade), slice(want, F + kFade));
        CHECK_ARRAY_EQUAL(slice(out, 0, F), slice(device(from)->play(in), 0, F));
    }
    // Into the Phaser: its cascade starts from silence; once that has rung in, the Phaser all along.
    for (const float from : {1.f, 2.f}) {
        INFO(std::to_string(from) + " -> Phaser");
        const Samples out = device(from)->play(in, {{F, "mode", 0.f}});
        const Samples want = device(0.f)->play(in);
        CHECK_ALLCLOSE(slice(out, F + kFade + 9600), slice(want, F + kFade + 9600), 0.0, 1e-5);
    }
    // A change during a fade waits for it: two changes 5 ms apart end on the second.
    const Samples twice = device(0.f)->play(in, {{F, "mode", 1.f}, {F + 240, "mode", 2.f}});
    CHECK_ARRAY_EQUAL(slice(twice, F + 2 * kFade), slice(device(2.f)->play(in), F + 2 * kFade));
}

TEST_CASE("phaser-flanger: a change of Notches lands on the new number") {
    const Samples in = noise(kSampleRate, 7);
    constexpr int64_t F = 24000, kFade = 960;
    const auto notches = [](float n) { return std::make_unique<Phaser>(kSampleRate, still({{"notches", n}})); };
    for (const auto& [from, to] : {std::pair{4.f, 12.f}, std::pair{12.f, 2.f}}) {
        INFO(std::to_string(from) + " -> " + std::to_string(to));
        const Samples out = notches(from)->play(in, {{F, "notches", to}});
        const Samples want = notches(to)->play(in);
        CHECK_ALLCLOSE(slice(out, F + kFade + 9600), slice(want, F + kFade + 9600), 0.0, 1e-5);
    }
    // Fewer: once faded, exactly the stages kept (they never stopped).
    const Samples fewer = notches(12.f)->play(in, {{F, "notches", 2.f}});
    CHECK_ARRAY_EQUAL(slice(fewer, F + kFade), slice(notches(2.f)->play(in), F + kFade));
}

TEST_CASE("phaser-flanger: changing any control is click-free") {
    // A 220 Hz tone through the device as it opens (but a 30 % sine sweep at
    // 0.5 Hz), the transport playing at 120 BPM (so Sync switches between the
    // free phase and the song's), every control jumping in turn as
    // automation's steps make them, each where it is heard (the LFO's while
    // LFO 1 is, the envelope's while it follows, Spin's while it spins, LFO
    // 2's while it is mixed in); then the sweep run fast and deep into the
    // stages' top and bottom limits.
    const auto s = [](double seconds) { return static_cast<int64_t>(seconds * kSampleRate); };
    const std::vector<Change> changes = {
        {s(0.4), "notches", 12.f},    {s(0.6), "notches", 1.f},     {s(0.8), "notches", 42.f},
        {s(1.0), "notches", 4.f},     {s(1.2), "center", 300.f},    {s(1.4), "center", 3000.f},
        {s(1.6), "spread", 0.f},      {s(1.8), "spread", 100.f},    {s(2.0), "blend", 1.f},
        {s(2.2), "mode", 1.f},        {s(2.4), "flange_time", 0.5f}, {s(2.6), "flange_time", 10.f},
        {s(2.8), "feedback", 0.f},    {s(3.0), "feedback", 60.f},   {s(3.2), "fb_invert", 1.f},
        {s(3.4), "mode", 2.f},        {s(3.6), "doubler_time", 20.f}, {s(3.8), "doubler_time", 150.f},
        {s(4.0), "fb_invert", 0.f},   {s(4.2), "mode", 0.f},        {s(4.4), "mode", 2.f},
        {s(4.6), "mode", 1.f},        {s(4.8), "mode", 0.f},        {s(5.0), "wave", 1.f},
        {s(5.2), "wave", 2.f},        {s(5.4), "freq", 5.f},        {s(5.6), "wave", 0.f},
        {s(5.8), "sync", 1.f},        {s(6.0), "rate", 7.f},        {s(6.2), "sync", 0.f},
        {s(6.4), "phase", 270.f},     {s(6.6), "spin_on", 1.f},     {s(6.8), "spin_on", 0.f},
        {s(7.0), "lfo2_mix", 100.f},  {s(7.2), "env_on", 1.f},      {s(7.4), "env_on", 0.f},
        {s(7.6), "safe_bass", 500.f}, {s(7.8), "safe_bass", 5.f},   {s(8.0), "warmth", 100.f},
        {s(8.2), "warmth", 0.f},      {s(8.4), "output", -24.f},    {s(8.6), "output", 12.f},
        {s(8.8), "output", 0.f},      {s(9.0), "mix", 0.f},         {s(9.2), "mix", 100.f},
        {s(9.4), "mix", 50.f},        {s(9.6), "center", 500.f},    {s(9.6), "notches", 8.f},
        {s(9.6), "spread", 30.f},     {s(10.0), "lfo2_mix", 0.f},   {s(10.2), "blend", 0.5f},
        {s(10.4), "amount", 100.f},   {s(10.8), "amount", 0.f},     {s(11.0), "amount", 60.f},
        {s(11.2), "duty", 90.f},      {s(11.4), "duty", -90.f},     {s(11.6), "wave", 2.f},
        {s(11.8), "duty", 90.f},      {s(12.0), "duty", 0.f},       {s(12.2), "spin_on", 1.f},
        {s(12.4), "spin", 100.f},     {s(12.6), "spin", 0.f},       {s(12.8), "env_on", 1.f},
        {s(13.0), "env_amount", -100.f}, {s(13.2), "env_attack", 300.f}, {s(13.4), "env_attack", 0.1f},
        {s(13.6), "env_release", 1000.f}, {s(13.8), "env_release", 50.f}, {s(14.0), "env_on", 0.f},
        {s(14.2), "lfo2_mix", 50.f},  {s(14.4), "freq2", 0.2f},     {s(14.6), "sync2", 1.f},
        {s(14.8), "rate2", 13.f},     {s(15.0), "sync2", 0.f},      {s(15.2), "lfo2_mix", 0.f},
        {s(15.2), "blend", 0.f},      {s(15.2), "wave", 0.f},       {s(15.2), "freq", 5.f},
        {s(15.2), "amount", 100.f},   {s(15.2), "center", 5000.f},  {s(15.8), "center", 70.f},
    };
    const Values opening = {{"amount", 30.f}, {"wave", 0.f}, {"freq", 0.5f}, {"env_amount", 100.f}};
    const Samples tone = smoothSine(220.0, 16.4);
    Phaser p(kSampleRate, opening);
    p.playing(true, 120.0, 0.0);
    Samples l = tone, r = tone;
    p.run({&l, &r}, changes);
    CHECK(allFinite(l) && allFinite(r));
    const double input = clickiness(tone, s(0.2));
    const double output = std::max(clickiness(l, s(0.2)), clickiness(r, s(0.2)));
    std::string each;
    for (size_t i = 0; i < changes.size(); ++i) {
        if (i + 1 < changes.size() && changes[i + 1].frame == changes[i].frame) continue;
        const double c = std::max(clickiness(l, changes[i].frame, changes[i].frame + s(0.2)),
                                  clickiness(r, changes[i].frame, changes[i].frame + s(0.2)));
        char line[96];
        std::snprintf(line, sizeof line, "%s %.2g; ", changes[i].id.c_str(), c);
        each += line;
    }
    INFO("tone " + std::to_string(input) + ", through the changes " + std::to_string(output) + " (" + each + ")");
    CHECK(output < 1e-4);  // (the Disperser's; measured 3.4e-5 at Output's 36 dB jump, any other change 2e-5 at most)

    // What the measure makes of a click: Phaser's output switched to Flanger's at once, unfaded,
    Phaser phased(kSampleRate, opening), flanged(kSampleRate, opening);
    flanged.set("mode", 1.f);
    flanged.prepare(kSampleRate);
    Samples spliced = phased.play(tone);
    const Samples flange = flanged.play(tone);
    std::copy(flange.begin() + s(1.5), flange.end(), spliced.begin() + s(1.5));
    CHECK(clickiness(spliced, s(0.2)) > 50 * output);
    // and the dry tone switched to the device fully wet in one sample.
    Phaser wet(kSampleRate, {{"amount", 30.f}, {"wave", 0.f}, {"freq", 0.5f}, {"mix", 100.f}});
    Samples stepped = tone;
    const Samples allWet = wet.play(tone);
    std::copy(allWet.begin() + s(1.5), allWet.end(), stepped.begin() + s(1.5));
    CHECK(clickiness(stepped, s(0.2)) > 50 * output);
}

TEST_CASE("phaser-flanger: its automation plays through the engine sample-accurately") {
    sub::Engine engine;
    engine.setClipFadeMs(0);
    // (221.25 Hz: at its peak where the automation steps, so the first sample changed shows.)
    const Samples tone = smoothSine(221.25, 3.0);
    const std::string path = makeWav(stereo(tone), 2);
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, 0.0, 3.0, 0.0, 1.f)});
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "phaser", -1);
    setParam(engine, id, "mix", 0.f);
    setParam(engine, id, "feedback", 0.f);
    const Samples untouched = engine.renderOffline(0.0, 3 * kSampleRate);  // fully dry: the track as it is

    // Dry/Wet: dry until beat 2 (1 s), then all wet; Doubler from beat 4 (2 s).
    using Points = std::vector<sub::AutomationPoint>;
    const float doubler = paramInfo(engine, id, "mode").toNormalized(2.f);
    CHECK_EQ(paramInfo(engine, id, "mode").fromNormalized(doubler), 2.f);
    engine.setTrackAutomation(track, {{id, "mix", Points{{0.0, 0.f, 0.f}, {2.0, 0.f, 0.f}, {2.0, 1.f, 0.f}}},
                                      {id, "mode", Points{{0.0, 0.f, 0.f}, {4.0, 0.f, 0.f}, {4.0, doubler, 0.f}}}});
    const Samples out = engine.renderOffline(0.0, 3 * kSampleRate);
    CHECK(allFinite(out));
    int64_t first = -1;
    for (size_t i = 0; i < out.size() && first < 0; ++i)
        if (out[i] != untouched[i]) first = static_cast<int64_t>(i) / 2;
    CHECK_EQ(first, int64_t{kSampleRate});
    // A doubled copy, 30 ms late, once the mode has faded over.
    const int64_t doubled = 2 * kSampleRate + kSampleRate * 13 / 100;
    CHECK(!allclose(frames(out, doubled), frames(untouched, doubled), 0.0, 0.01));
    // And no click where they change: no more than the WAV's own 16-bit steps make.
    for (int c = 0; c < 2; ++c) {
        INFO(std::to_string(c));
        CHECK(clickiness(channel(out, c), kSampleRate / 5) < 2.0 * clickiness(channel(untouched, c), kSampleRate / 5));
    }
}

TEST_CASE("phaser-flanger: a synced LFO follows the song") {
    // Through the engine, playing at 120 BPM: a quarter note a cycle, 24 000 frames.
    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "phaser", -1);
    setParam(engine, id, "sync", 1.f);
    setParam(engine, id, "rate", 10.f);  // 1/4
    setParam(engine, id, "spin_on", 1.f);
    setParam(engine, id, "spin", 50.f);
    engine.renderOffline(0.0, 2 * kSampleRate);
    std::vector<float> phase, right;
    engine.readProcessorDisplay(id, 0, 0, phase);
    engine.readProcessorDisplay(id, 1, 0, right);
    REQUIRE(phase.size() >= 370);
    double phaseOff = 0.0, rightOff = 0.0;
    for (size_t k = 0; k < phase.size(); ++k) {
        const double position = 256.0 * static_cast<double>(k + 1) / 24000.0;
        phaseOff = std::max(phaseOff, std::abs(wrapHalf(phase[k] - position)));
        rightOff = std::max(rightOff, std::abs(wrapHalf(right[k] - 1.5 * position)));  // spinning half as fast again
    }
    CHECK(phaseOff < 1e-3);
    CHECK(rightOff < 1e-3);

    // Spin automated deep into a song: the right side speeds up step by step, never lurching.
    Phaser p(kSampleRate, {{"sync", 1.f}, {"rate", 10.f}, {"spin_on", 1.f}, {"spin", 0.f}});
    p.playing(true, 120.0, 1600.0);
    std::vector<Change> steps;
    for (int i = 1; i <= 100; ++i) steps.push_back({static_cast<int64_t>(i) * 960, "spin", static_cast<float>(i)});
    p.play(Samples(static_cast<size_t>(2.2 * kSampleRate), 0.f), steps);
    const std::vector<float> spun = p.display("phase_r");
    double least = 1.0, most = 0.0;
    for (size_t k = 1; k < spun.size(); ++k) {
        const double advance = frac(spun[k] - spun[k - 1]);
        least = std::min(least, advance);
        most = std::max(most, advance);
    }
    INFO("advances " + std::to_string(least) + " .. " + std::to_string(most));
    CHECK(least > 0.0106 - 1e-4);
    CHECK(most < 0.0214 + 1e-4);
}

TEST_CASE("phaser-flanger: the random shapes repeat") {
    const Values values = still({{"mode", 1.f}, {"amount", 100.f}, {"wave", 9.f}, {"freq", 5.f}});
    Phaser p(kSampleRate, values);
    const Samples in = noise(kSampleRate, 8);
    const Samples first = p.play(in);
    const std::vector<float> lfo = p.display("lfo");
    p.processor().reset();
    CHECK_ARRAY_EQUAL(p.play(in), first);
    // Random S&H holds a value a cycle (5 Hz: 9600 frames).
    size_t changes = 0;
    for (size_t k = 1; k < lfo.size(); ++k) {
        const auto cycle = [](size_t i) {
            return static_cast<int64_t>(5.0 * 256.0 * static_cast<double>(i + 1) / kSampleRate);
        };
        if (lfo[k] != lfo[k - 1]) {
            ++changes;
            CHECK(cycle(k) != cycle(k - 1));
        }
    }
    CHECK(changes >= 3);
}

TEST_CASE("phaser-flanger: reset and a new sample rate start it from silence") {
    const Values values = {{"mode", 1.f}, {"flange_time", 1.f}, {"amount", 0.f}};
    Phaser fresh(44100.0, values);
    const Samples want = fresh.play(impulse(8192));
    Phaser p(44100.0, values);
    p.play(noise(44100, 9));
    p.processor().reset();
    CHECK_ARRAY_EQUAL(p.play(impulse(8192)), want);

    // A new rate: 1 ms is 96 samples at 96 kHz.
    p.play(noise(44100, 10));
    p.prepare(96000.0);
    const Samples h = p.play(impulse(8192));
    CHECK_NEAR(h[96], 0.5, 1e-7);
    CHECK_NEAR(h[0], 0.5, 1e-7);
    CHECK_NEAR(h[95], 0.0, 1e-7);
    Phaser at96(96000.0, values);
    CHECK_ARRAY_EQUAL(h, at96.play(impulse(8192)));
}

TEST_CASE("phaser-flanger: silence rings out to exact zeros") {
    const std::vector<Values> settings = {
        {{"mode", 0.f}, {"notches", 12.f}, {"feedback", 70.f}},
        {{"mode", 1.f}, {"flange_time", 5.f}, {"feedback", 90.f}},
        {{"mode", 2.f}, {"doubler_time", 60.f}, {"feedback", 70.f}},
    };
    for (const Values& setting : settings) {
        INFO("mode " + std::to_string(setting[0].second));
        Values values = setting;
        values.insert(values.end(), {{"warmth", 50.f}, {"safe_bass", 100.f}, {"amount", 50.f}});
        Phaser p(kSampleRate, values);
        Samples l = noise(kSampleRate / 2, 11), r = noise(kSampleRate / 2, 12);
        l.resize(static_cast<size_t>(30.5 * kSampleRate), 0.f);
        r.resize(l.size(), 0.f);
        p.run({&l, &r});
        for (const Samples* out : {&l, &r}) {
            CHECK(allEqual(slice(*out, static_cast<int64_t>(25.5 * kSampleRate)), 0.0));
            size_t denormal = 0;
            for (const float v : *out) denormal += v != 0.f && std::abs(v) < std::numeric_limits<float>::min();
            CHECK_EQ(denormal, size_t{0});
        }
        // Denormal input passes as finite numbers, and dies away to zeros too.
        Samples tiny(static_cast<size_t>(2 * kSampleRate), 0.f);
        for (size_t i = 0; i < 1000; ++i) tiny[i] = (i % 2 ? 1e-40f : -1e-41f);
        Samples tl = tiny, tr = tiny;
        p.run({&tl, &tr});
        CHECK(allFinite(tl) && allFinite(tr));
        CHECK(allEqual(slice(tl, kSampleRate), 0.0));
        CHECK(allEqual(slice(tr, kSampleRate), 0.0));
    }
    // Gains gliding to 0 together: clicks through Warmth at Output -24 dB (22.05 kHz, where Warmth's
    // pole decays fastest), Dry/Wet and Warmth turned down at once. Their product, and what Warmth
    // still holds, pass through a float's denormal range on the way to 0: none of it comes out.
    {
        Phaser p(22050.0, still({{"warmth", 100.f}, {"mix", 100.f}, {"output", -24.f}}));
        Samples l(3 * 22050, 0.f);
        for (size_t i = 0; i < l.size(); i += 551) l[i] = 1.f;
        Samples r = l;
        p.run({&l, &r}, {{21760, "mix", 0.f}, {21760, "warmth", 0.f}});
        size_t denormal = 0;
        for (const Samples* out : {&l, &r})
            for (const float v : *out) denormal += v != 0.f && std::abs(v) < std::numeric_limits<float>::min();
        CHECK_EQ(denormal, size_t{0});
    }
}

TEST_CASE("phaser-flanger: its tail covers its ringing") {
    const std::vector<Values> settings = {
        {{"notches", 4.f}},
        {{"notches", 12.f}, {"center", 400.f}, {"spread", 30.f}, {"feedback", 80.f}},
        {{"mode", 1.f}, {"flange_time", 20.f}, {"feedback", 95.f}, {"amount", 100.f}},
        {{"mode", 2.f}, {"doubler_time", 150.f}, {"feedback", 95.f}},
        {{"safe_bass", 20.f}},
    };
    for (const Values& setting : settings) {
        Values values = {{"amount", 0.f}, {"feedback", 0.f}};
        values.insert(values.end(), setting.begin(), setting.end());
        std::string name;
        for (const auto& [id, value] : setting) name += id + " " + std::to_string(value) + " ";
        INFO(name);
        Phaser p(kSampleRate, values);
        const auto tail = static_cast<size_t>(tailOf(p));
        const Samples h = p.play(impulse(powerOfTwoAtLeast(2 * tail + 4096)));
        const Samples after = slice(h, static_cast<int64_t>(tail));
        INFO("tail " + std::to_string(tail) + ", what is left " + std::to_string(db(maxAbs(after) / maxAbs(h))) +
             " dB");
        CHECK(maxAbs(after) < 1e-3 * maxAbs(h));  // 60 dB down
        CHECK(energy(after) < 1e-5 * energy(h));
    }
    Phaser dry(kSampleRate, {{"mix", 0.f}, {"feedback", 95.f}});
    CHECK_EQ(tailOf(dry), 0);
    // At most a minute, however long it rings.
    Phaser longest(kSampleRate, {{"notches", 42.f}, {"center", 70.f}, {"spread", 0.f}, {"feedback", 100.f},
                                 {"amount", 100.f}});
    CHECK(tailOf(longest) > 10 * kSampleRate);
    CHECK(tailOf(longest) <= 60 * kSampleRate);
}

TEST_CASE("phaser-flanger: it stays stable at the extremes and at any sample rate") {
    const std::vector<Values> settings = {
        {{"mode", 0.f}, {"notches", 42.f}, {"center", 18500.f}, {"spread", 0.f}},
        {{"mode", 0.f}, {"notches", 42.f}, {"center", 70.f}, {"spread", 100.f}},
        {{"mode", 0.f}, {"notches", 42.f}, {"center", 70.f}, {"spread", 0.f}},
        {{"mode", 1.f}, {"flange_time", 0.1f}},
        {{"mode", 1.f}, {"flange_time", 20.f}},
        {{"mode", 2.f}, {"doubler_time", 150.f}},
    };
    for (const double rate : {22050.0, 44100.0, 96000.0, 192000.0}) {
        int which = 0;
        for (const Values& setting : settings) {
            for (const float invert : {0.f, 1.f}) {
                const bool synced = (which++ % 3) == 1;
                Values values = {{"feedback", 100.f}, {"fb_invert", invert}, {"amount", 100.f}, {"env_on", 1.f},
                                 {"env_amount", 100.f}, {"env_attack", 0.1f}, {"warmth", 30.f}};
                values.insert(values.end(), setting.begin(), setting.end());
                if (synced) {  // an 80 Hz LFO: 1/64 at 300 BPM
                    values.insert(values.end(), {{"sync", 1.f}, {"rate", 0.f}, {"wave", 2.f}});
                } else {
                    values.insert(values.end(), {{"wave", 9.f}, {"freq", 5.f}});
                }
                INFO(std::to_string(rate) + " Hz, mode " + std::to_string(setting[0].second) + ", setting " +
                     std::to_string(which) + (synced ? ", synced" : ", Random S&H"));
                Phaser p(rate, values);
                if (synced) p.playing(true, 300.0, 0.0);
                const auto half = static_cast<size_t>(1.5 * rate);
                Samples l = noise(half, 13, 0.9f), r = noise(half, 14, 0.9f);
                for (size_t i = 0; i < half; ++i) {  // then a full-scale square
                    l.push_back(((i / 100) % 2) ? 1.f : -1.f);
                    r.push_back(((i / 77) % 2) ? -1.f : 1.f);
                }
                p.run({&l, &r});
                CHECK(allFinite(l) && allFinite(r));
                CHECK(std::max(maxAbs(l), maxAbs(r)) < 40.0);
                for (const char* id : {"sweep_l", "lfo", "mod"}) CHECK(allFinite(p.display(id)));
            }
        }
    }
    // Kept below Nyquist: at 22.05 kHz, 18.5 kHz plays just under 0.45 of the rate (the limit's soft edge).
    Phaser high(22050.0, {{"amount", 0.f}, {"center", 18500.f}});
    high.play(Samples(22050, 0.f));
    const std::vector<float> top = high.display("sweep_l");
    CHECK(maxOfValues(top) < 9922.5);
    CHECK_APPROX_REL(minOf(top), 9922.5, 1e-4);
    CHECK_APPROX_REL(minOf(top), phaser::phaserCenterHz(18500.0, 0.0, 0.0, 22050.0), 1e-6);
    // And the flanger never reads nearer than 2 samples (0.1 ms swept down two octaves would be 0.55).
    Phaser shortest(22050.0, {{"mode", 1.f}, {"flange_time", 0.1f}, {"amount", 100.f}, {"wave", 0.f}, {"freq", 2.f}});
    shortest.play(Samples(22050, 0.f));
    CHECK_APPROX_REL(minOf(shortest.display("sweep_l")), 2000.0 / 22050.0, 1e-4);
    // Triangle Analog at 80 Hz: finite, within ±1, whatever the duty.
    for (const double duty : {-1.0, -0.5, 0.0, 0.7, 1.0}) {
        for (int i = 0; i < 100; ++i) {
            const float v = phaser::waveValue(phaser::Wave::TriangleAnalog, i / 100.0, 0, duty, 80.0);
            CHECK(std::isfinite(v));
            CHECK(std::abs(v) <= 1.f);
        }
    }
}

TEST_CASE("phaser-flanger: one channel plays as either of two") {
    const Samples in = noise(kSampleRate, 15);
    const Values values = {{"phase", 180.f}, {"amount", 100.f}, {"freq", 3.f}, {"safe_bass", 150.f},
                           {"warmth", 40.f}, {"env_on", 1.f}};
    for (const float mode : {0.f, 1.f, 2.f}) {
        INFO("mode " + std::to_string(mode));
        Values withMode = values;
        withMode.emplace_back("mode", mode);
        Phaser mono(kSampleRate, withMode), stereo(kSampleRate, withMode);
        const Samples one = mono.play(in, {{20000, "mode", mode == 0.f ? 1.f : 0.f}, {30000, "notches", 9.f}});
        Samples l = in, r = in;
        stereo.run({&l, &r}, {{20000, "mode", mode == 0.f ? 1.f : 0.f}, {30000, "notches", 9.f}});
        CHECK_ARRAY_EQUAL(one, l);
        CHECK(!allclose(l, r, 0.0, 1e-3));  // (the right sweeps the other way)
    }
}

TEST_CASE("phaser-flanger: its displays are what plays") {
    // A second at 48 kHz: 187 values each.
    Phaser p(kSampleRate, still({{"mix", 0.f}}));
    p.play(sine(440.0, 1.0));
    for (const char* id :
         {"phase", "phase_r", "lfo", "mod", "env", "sweep_l", "sweep_r", "q_l", "q_r", "input", "output"}) {
        INFO(id);
        const std::vector<float> values = p.display(id);
        CHECK_EQ(values.size(), size_t{187});
        if (std::string(id) == "input" || std::string(id) == "output") {
            for (size_t k = 1; k < values.size(); ++k) CHECK_NEAR(values[k], -6.02, 0.1);
        }
        if (std::string(id) == "env") CHECK(allEqual(values, 0.0));  // Env Follow off
        if (std::string(id) == "sweep_l") CHECK_ALLCLOSE(values, 1000.0, 1e-6, 0.0);
        if (std::string(id) == "q_l") CHECK_ALLCLOSE(values, phaser::qOfSpread(0.5), 1e-6, 0.0);
    }
    // Flanger: the delay in ms, and no Q.
    Phaser f(kSampleRate, still({{"mode", 1.f}, {"flange_time", 2.5f}}));
    f.play(Samples(kSampleRate, 0.f));
    for (const float v : f.display("sweep_l")) CHECK_APPROX_REL(v, 2.5, 1e-6);
    CHECK(allEqual(f.display("q_l"), 0.0));
    CHECK(allEqual(f.display("input"), -90.0));  // silence: the floor
}

TEST_CASE("phaser-flanger: it is cheap enough") {
#ifndef NDEBUG
    SKIP("timing needs an optimized build");
#else
    // Timed in the thread's CPU time, not the wall clock's: a busy machine (a parallel build, the
    // other tests) takes the core away, not the device's cost.
    const Samples left = noise(10 * kSampleRate, 21), right = noise(10 * kSampleRate, 22);
    const auto seconds = [&](const Values& values) {
        double best = 1e9;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Phaser p(kSampleRate, values);
            Samples l = left, r = right;
            const double start = threadSeconds();
            p.run({&l, &r});
            best = std::min(best, threadSeconds() - start);
        }
        return best;
    };
    const double defaults = seconds({});
    const double heavy = seconds({{"notches", 42.f}, {"feedback", 95.f}, {"amount", 100.f}, {"wave", 9.f}});
    INFO("10 s at the defaults: " + std::to_string(defaults) + " s; at the heaviest: " + std::to_string(heavy) + " s");
    CHECK(defaults < 0.1);
    CHECK(heavy < 0.4);
#endif
}

TEST_CASE("phaser-flanger: an LFO's jumps crossfade") {
    const Values values = still({{"notches", 1.f}, {"amount", 100.f}, {"wave", 0.f}, {"freq", 0.5f}, {"rate", 10.f}});
    const auto frameOf = [](size_t k) { return 256.0 * static_cast<double>(k + 1); };
    // Checks a jump at `at` (frame) from the old trajectory to the new: 3-5 values on the way, none
    // moving more than 0.6 of the jump, and from 30 ms on exactly the new.
    const auto crossfades = [&](const std::vector<float>& lfo, double at, const std::function<double(double)>& want) {
        size_t between = 0;
        double largest = 0.0;
        for (size_t k = 1; k < lfo.size(); ++k) {
            const double frame = frameOf(k);
            if (frame < at - 512.0 || frame > at + 0.05 * kSampleRate) continue;
            largest = std::max(largest, std::abs(static_cast<double>(lfo[k]) - lfo[k - 1]));
            if (frame > at && std::abs(lfo[k] - want(frame)) > 1e-5) ++between;
            if (frame >= at + 0.03 * kSampleRate) CHECK_NEAR(lfo[k], want(frame), 1e-5);
        }
        INFO(std::to_string(between) + " values on the way, the largest step " + std::to_string(largest));
        CHECK(between >= 3);
        CHECK(between <= 5);
        CHECK(largest < 0.6);
    };
    // Sync switched on at 0.5 s: the free phase 0.25 there (1), the song's frac(1.0) (0).
    {
        Phaser p(kSampleRate, values);
        p.playing(true, 120.0, 0.0);
        p.play(Samples(kSampleRate, 0.f), {{24000, "sync", 1.f}});
        crossfades(p.display("lfo"), 24000.0, [](double frame) { return std::sin(2.0 * kPi * frac(frame / 24000.0)); });
    }
    // A new waveform just after a cycle starts: the sine near 0, the rectangle at +1.
    {
        Phaser p(kSampleRate, values);
        p.play(Samples(3 * kSampleRate, 0.f), {{96016, "wave", 7.f}});
        crossfades(p.display("lfo"), 96016.0,
                   [](double frame) { return frac(0.5 * frame / kSampleRate) < 0.5 ? 1.0 : -1.0; });
    }
    // The transport looping back, synced: a block at beat 8.25 (1), then one from beat 0 (0).
    {
        Values synced = values;
        synced.emplace_back("sync", 1.f);
        Phaser p(kSampleRate, synced);
        p.playing(true, 120.0, 0.0);
        constexpr int64_t kLoop = 24064;
        p.beatAt = [](int64_t frame) {
            return frame < kLoop ? 8.25 - static_cast<double>(kLoop - 256 - frame) / 24000.0
                                 : static_cast<double>(frame - kLoop) / 24000.0;
        };
        p.play(Samples(kSampleRate, 0.f));
        crossfades(p.display("lfo"), static_cast<double>(kLoop),
                   [](double frame) { return std::sin(2.0 * kPi * frac((frame - kLoop) / 24000.0)); });
    }
    // Free, the transport changes nothing.
    {
        const Samples in = noise(kSampleRate, 16);
        Phaser still1(kSampleRate, {{"freq", 2.f}}), moving(kSampleRate, {{"freq", 2.f}});
        const Samples want = still1.play(in);
        Samples got;
        for (int part = 0; part < 4; ++part) {
            moving.playing(part % 2 == 0, 120.0, 37.0 * part);
            const Samples piece = moving.play(slice(in, part * 12000, (part + 1) * 12000));
            got.insert(got.end(), piece.begin(), piece.end());
        }
        CHECK_ARRAY_EQUAL(got, want);
    }
}

TEST_CASE("phaser-flanger: the editor's curve is the response, notches and combs drawn true") {
    phaser::Curve curve;
    // 4 notches: sparse everywhere, every point on the response, every notch among them.
    phaser::Response r;
    r.q = phaser::qOfSpread(0.5);
    phaser::curve(r, 20.0, 20000.0, 226, kSampleRate, curve);
    CHECK(std::none_of(curve.dense.begin(), curve.dense.end(), [](uint8_t d) { return d != 0; }));
    double worst = 0.0;
    for (size_t i = 0; i < curve.line.size(); ++i) {
        worst = std::max(worst, std::abs(curve.line[i].db - phaser::responseDb(r, curve.line[i].freqHz, kSampleRate)));
        if (i > 0) CHECK(curve.line[i].freqHz > curve.line[i - 1].freqHz);
    }
    CHECK(worst < 1e-12);
    for (const double notch : phaser::notchFrequencies(4, 1000.0, r.q, kSampleRate)) {
        const auto at = std::find_if(curve.line.begin(), curve.line.end(),
                                     [&](const phaser::CurvePoint& p) { return p.freqHz == notch; });
        REQUIRE(at != curve.line.end());
        CHECK(at->db < -60.0);
    }
    CHECK_APPROX(curve.line.back().freqHz, 20000.0);

    // A 30 ms doubler: a comb finer than the columns from about 1.1 kHz up, drawn as a band.
    phaser::Response d;
    d.mode = phaser::Mode::Doubler;
    d.delayMs = 30.0;
    phaser::curve(d, 20.0, 20000.0, 226, kSampleRate, curve);
    const auto columnAt = [](double f) {
        return static_cast<size_t>(std::floor(226.0 * std::log(f / 20.0) / std::log(1000.0)));
    };
    for (size_t c = 0; c < 226; ++c) {
        const double f = 20.0 * std::pow(1000.0, static_cast<double>(c) / 226.0);
        if (f < 1000.0) CHECK_EQ(curve.dense[c], 0);
        if (f > 1150.0) {
            CHECK_EQ(curve.dense[c], 1);
            CHECK_NEAR(curve.top[c], 0.0, 0.1);
            CHECK(curve.bottom[c] < -25.0);
        }
    }
    CHECK_EQ(curve.dense[columnAt(5000.0)], 1);
    // Fixed angles: the same band for a slightly different delay (no flicker as it sweeps).
    phaser::Curve moved;
    d.delayMs = 30.01;
    phaser::curve(d, 20.0, 20000.0, 226, kSampleRate, moved);
    for (size_t c = columnAt(1500.0); c < 226; ++c) {
        CHECK_EQ(moved.dense[c], 1);
        CHECK_EQ(moved.top[c], curve.top[c]);
        CHECK_EQ(moved.bottom[c], curve.bottom[c]);
    }

    // A 1 ms flanger: no dense column, its notches up to 20 kHz among the points.
    phaser::Response f;
    f.mode = phaser::Mode::Flanger;
    f.delayMs = 1.0;
    phaser::curve(f, 20.0, 20000.0, 226, kSampleRate, curve);
    CHECK(std::none_of(curve.dense.begin(), curve.dense.end(), [](uint8_t v) { return v != 0; }));
    const std::vector<double> comb = phaser::combNotchFrequencies(1.0, 20000.0, 100);
    CHECK_EQ(comb.size(), size_t{20});
    for (const double notch : comb) {
        INFO(std::to_string(notch));
        const auto at = std::find_if(curve.line.begin(), curve.line.end(),
                                     [&](const phaser::CurvePoint& p) { return std::abs(p.freqHz - notch) < 1e-9; });
        CHECK(at != curve.line.end());
    }

    // 42 tight notches: dense around 1 kHz, not below 300 Hz.
    phaser::Response many;
    many.notches = 42;
    many.q = phaser::qOfSpread(0.0);
    phaser::curve(many, 20.0, 20000.0, 226, kSampleRate, curve);
    CHECK_EQ(curve.dense[columnAt(1000.0)], 1);
    for (size_t c = 0; c < columnAt(300.0); ++c) CHECK_EQ(curve.dense[c], 0);

    // At 22.05 kHz the points above Nyquist hold its value.
    phaser::curve(r, 20.0, 20000.0, 226, 22050.0, curve);
    const double nyquist = phaser::responseDb(r, 0.499 * 22050.0, 22050.0);
    for (const phaser::CurvePoint& p : curve.line) {
        if (p.freqHz > 11025.0) CHECK_EQ(p.db, nyquist);
    }
}
