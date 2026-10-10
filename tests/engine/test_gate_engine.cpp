// The built-in Gate: its listing; opening at the threshold and closing below
// Return, after Hold, over Attack and Release, to the sample; Floor and Flip;
// lookahead as latency (standalone and through the engine); keying from a
// sidechain, its gain and blend, and an EQ on the key that plays as its editor
// draws it; listening to the key; every control changing without a click;
// automation to the sample, whatever the block size; reset and a new sample
// rate; the extremes and stability; one channel; and its displays.

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "Engine.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/DspBlocks.h"
#include "builtin/GateDesign.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;
namespace gate = sub::gate;

namespace {

constexpr int kBlock = 1024;  // the renderer's largest block (Renderer::kMaxBlock)

using Values = std::vector<std::pair<std::string, float>>;

// What every test starts from unless it says otherwise, so the arithmetic doesn't
// hang on the defaults: threshold -40 dB, Return 3 dB, attack 0.1 ms (4.8 samples),
// hold 10 ms (480), release 100 ms (4800), floor -40 dB (a gain of 0.01), no
// lookahead, not flipped, no EQ, not listening.
const Values kBase = {{"threshold", -40.f}, {"return", 3.f},    {"attack", 0.1f}, {"hold", 10.f}, {"release", 100.f},
                      {"floor", -40.f},     {"lookahead", 0.f}, {"flip", 0.f},    {"sc_eq", 0.f}, {"sc_listen", 0.f}};

// `base` with `more` merged in (as a Python dict update).
Values with(Values base, const Values& more) {
    for (const auto& [id, value] : more) {
        auto it = std::find_if(base.begin(), base.end(), [&](const auto& v) { return v.first == id; });
        if (it != base.end())
            it->second = value;
        else
            base.emplace_back(id, value);
    }
    return base;
}

// A parameter's change at a frame, as automation hands it over.
struct Change {
    int64_t frame;
    std::string id;
    float value;
};

// The sidechain: what it hears (left, and right or the left again), and the
// frames at whose blocks it is connected.
struct Key {
    const Samples* left = nullptr;
    const Samples* right = nullptr;
    int64_t from = 0;
    int64_t to = std::numeric_limits<int64_t>::max();
};

// A Gate on its own, outside an engine, at any sample rate: processed in blocks,
// its changes handed over as automation (so its blocks split there), its
// sidechain set for each block, as the renderer does.
class Gate {
public:
    explicit Gate(const Values& values = kBase, double rate = kSampleRate)
        : processor_(sub::BuiltinRegistry::instance().create("gate")), rate_(rate) {
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

    // Processes one or two channels of equal length in place, `block` frames at a time.
    void run(const std::vector<Samples*>& channels, const std::vector<Change>& changes = {}, int block = 256,
             const Key& key = {}) {
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
            const bool connected = key.left != nullptr && start >= key.from && start < key.to;
            processor_->setSidechainConnected(connected);
            if (connected) {
                const Samples* right = key.right != nullptr ? key.right : key.left;
                processor_->setSidechain(key.left->data() + start, right->data() + start);
            }
            ctx.samplePos = start;
            processor_->process(ctx, pointers, static_cast<int>(channels.size()), n);
            processor_->setSidechain(nullptr, nullptr);
            processor_->clearAutomation();
        }
    }
    // One channel: what comes out.
    Samples play(Samples mono, const std::vector<Change>& changes = {}, int block = 256, const Key& key = {}) {
        run({&mono}, changes, block, key);
        return mono;
    }
    // The same on both channels: the left that comes out (checking the right is the same).
    Samples playStereo(const Samples& mono, const std::vector<Change>& changes = {}, int block = 256,
                       const Key& key = {}) {
        Samples l = mono, r = mono;
        run({&l, &r}, changes, block, key);
        CHECK_ARRAY_EQUAL(r, l);
        return l;
    }

    // Display `index`'s values since the last read.
    std::vector<float> display(int index) {
        std::vector<float> values;
        positions_[static_cast<size_t>(index)] =
            processor_->readDisplay(index, positions_[static_cast<size_t>(index)], values);
        return values;
    }
    uint64_t position(int index) const { return positions_[static_cast<size_t>(index)]; }

private:
    std::shared_ptr<sub::Processor> processor_;
    double rate_;
    uint64_t positions_[4] = {};
};

// Constant stretches: {length, value}, one after another.
Samples levels(const std::vector<std::pair<int64_t, float>>& parts) {
    Samples x;
    for (const auto& [length, value] : parts) x.insert(x.end(), static_cast<size_t>(length), value);
    return x;
}

Samples tone(double freq, int64_t frames, double amplitude = 0.5, double rate = kSampleRate) {
    Samples x(static_cast<size_t>(frames));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate));
    return x;
}

Samples noise(int64_t frames, unsigned seed, float amplitude = 0.5f) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> uniform(-amplitude, amplitude);
    Samples x(static_cast<size_t>(frames));
    for (float& v : x) v = uniform(random);
    return x;
}

Samples plus(const Samples& a, const Samples& b) {
    Samples x(a.size());
    for (size_t i = 0; i < x.size(); ++i) x[i] = a[i] + b[i];
    return x;
}

// The gain applied at frame i (out / in).
double gainAt(const Samples& out, const Samples& in, int64_t i) {
    return static_cast<double>(out[static_cast<size_t>(i)]) / static_cast<double>(in[static_cast<size_t>(i)]);
}

// The largest sample-to-sample step over [from, to).
double largestStep(const Samples& x, int64_t from = 1, int64_t to = -1) {
    if (to < 0) to = static_cast<int64_t>(x.size());
    double worst = 0.0;
    for (int64_t i = std::max<int64_t>(from, 1); i < to; ++i)
        worst =
            std::max(worst, std::abs(static_cast<double>(x[static_cast<size_t>(i)]) - x[static_cast<size_t>(i - 1)]));
    return worst;
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
    for (int64_t i = std::max<int64_t>(from, 6); i < to; ++i)
        worst = std::max(worst, std::abs(d[static_cast<size_t>(i)]));
    return worst;
}

// RMS over the last `frames`.
double tailRms(const Samples& x, int64_t frames) { return rms(slice(x, static_cast<int64_t>(x.size()) - frames)); }

double db(double ratio) { return 20.0 * std::log10(ratio); }

// The gain the gate applies with its openness at `openness` (0..1), not flipped.
double expectedGain(double openness, float floorDb) {
    return gate::gain(gate::shape(static_cast<float>(openness)), gate::floorGain(floorDb));
}

// Whether |out| never exceeds |in| `delay` samples earlier (plus a hair).
bool neverLouder(const Samples& in, const Samples& out, int64_t delay = 0) {
    for (size_t i = 0; i < out.size(); ++i) {
        const double was = i >= static_cast<size_t>(delay) ? std::abs(in[i - static_cast<size_t>(delay)]) : 0.0;
        if (std::abs(out[i]) > was + 1e-6) return false;
    }
    return true;
}

// A 16-bit WAV's value for `v` (what a clip of it plays).
float quantized(float v) { return static_cast<float>(std::round(static_cast<double>(v) * 32768.0) / 32768.0); }

// --- Through the engine ------------------------------------------------------------------

struct GateEngine {
    sub::Engine engine;
    GateEngine() { engine.setClipFadeMs(0); }
};

uint32_t clipTrackOf(sub::Engine& engine, const Samples& mono, double seconds) {
    const std::string path = makeWav(stereo(mono), 2);
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, 0.0, seconds, 0.0, 1.f)});
    return track;
}

// A Gate on a track's chain with `values`; the engine realigned to its latency.
uint32_t gateOn(sub::Engine& engine, uint32_t track, const Values& values = kBase) {
    const uint32_t device = engine.addBuiltinProcessor(engine.trackChain(track), "gate", -1);
    for (const auto& [id, value] : values) setParam(engine, device, id, value);
    engine.idle();  // (the lookahead is latency: the engine realigns the tracks)
    return device;
}

}  // namespace

TEST_CASE("the gate is listed with its parameters") {
    const sub::BuiltinInfo info = builtinInfo("gate");
    CHECK_EQ(info.name, std::string("Gate"));
    CHECK(!info.isInstrument());
    CHECK(paramIds(info.params) ==
          (std::vector<std::string>{"threshold", "return", "attack", "hold", "release", "floor", "lookahead", "flip",
                                    "sc_gain", "sc_mix", "sc_listen", "sc_eq", "sc_eq_type", "sc_eq_freq", "sc_eq_q",
                                    "sc_eq_gain"}));
    struct Expected {
        const char* unit;
        float min, max, value;
        bool log;
    };
    const std::vector<Expected> expected = {
        {"dB", -70.f, 6.f, -12.f, false}, {"dB", 0.f, 24.f, 3.f, false},     {"ms", 0.02f, 150.f, 3.5f, true},
        {"ms", 1.f, 1500.f, 10.f, true},  {"ms", 0.1f, 3000.f, 15.f, true},  {"dB", -75.f, 0.f, -40.f, false},
        {"", 0.f, 2.f, 1.f, false},       {"", 0.f, 1.f, 0.f, false},        {"dB", -70.f, 24.f, 0.f, false},
        {"%", 0.f, 100.f, 100.f, false},  {"", 0.f, 1.f, 0.f, false},        {"", 0.f, 1.f, 0.f, false},
        {"", 0.f, 5.f, 5.f, false},       {"Hz", 30.f, 15000.f, 80.f, true}, {"", 0.1f, 12.f, 0.71f, true},
        {"dB", -15.f, 15.f, 0.f, false},
    };
    REQUIRE(info.params.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        const sub::ParamInfo& p = info.params[i];
        const Expected& e = expected[i];
        INFO(p.id);
        CHECK_EQ(p.unit, std::string(e.unit));
        CHECK_APPROX(p.minValue, e.min);
        CHECK_APPROX(p.maxValue, e.max);
        CHECK_APPROX(p.defaultValue, e.value);
        CHECK_EQ(p.isLog(), e.log);
        CHECK_EQ(p.automatable, p.id != "lookahead" && p.id != "sc_listen");
    }
    CHECK_EQ(info.params[2].name, std::string("Attack"));
    CHECK_EQ(info.params[8].name, std::string("S/C Gain"));
    CHECK_EQ(info.params[12].name, std::string("S/C EQ Type"));
    CHECK(info.params[6].valueLabels == (std::vector<std::string>{"0 ms", "1 ms", "10 ms"}));
    for (const size_t i : {7u, 10u, 11u}) CHECK(info.params[i].valueLabels == (std::vector<std::string>{"Off", "On"}));
    CHECK(info.params[12].valueLabels ==
          (std::vector<std::string>{"Low Shelf", "Bell", "High Shelf", "Low-pass", "Band-pass", "High-pass"}));
    CHECK_EQ(info.params[12].valueLabels[3], std::string("Low-pass"));
    CHECK_EQ(info.params[12].valueLabels[5], std::string("High-pass"));

    // At its defaults: 1 ms of lookahead (Live's), so 48 samples of latency at 48 kHz.
    Gate defaults({});
    CHECK_EQ(defaults.processor().typeId(), std::string("builtin:gate"));
    CHECK_EQ(defaults.processor().name(), std::string("Gate"));
    CHECK(defaults.processor().hasSidechain());
    CHECK_EQ(defaults.processor().latencySamples(), 48);
    CHECK_EQ(defaults.processor().tailSamples(), 48);

    // The engine makes it.
    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "gate", -1);
    const sub::ProcessorInfo processor = engine.processorInfo(id);
    CHECK_EQ(processor.name, std::string("Gate"));
    CHECK_EQ(processor.latency, 48);
    CHECK(processor.hasSidechain);
}

TEST_CASE("the gate opens at the threshold and closes after the hold and release") {
    Gate g;
    const int64_t d = 24000;  // where the level drops
    const Samples in = levels({{d, 0.5f}, {24000, 0.001f}});
    const Samples out = g.playStereo(in);
    // Opens over the attack: 0.1 ms is 4.8 samples, so five steps (the first sample takes the first)...
    CHECK_APPROX_TOL(gainAt(out, in, 0), expectedGain(1.0 / 4.8, -40.f), 1e-5, 0.0);
    CHECK(out[3] < in[3]);
    // ...then passes the input bit for bit.
    CHECK_ARRAY_EQUAL(slice(out, 4, d), slice(in, 4, d));
    // Held for 10 ms (480 samples) after the level falls below the closing level...
    CHECK_ARRAY_EQUAL(slice(out, d, d + 480), slice(in, d, d + 480));
    CHECK(out[d + 480] < in[d + 480]);
    // ...then released over 100 ms (4800 samples) down to the floor, eased: halfway, halfway down.
    CHECK_NEAR(gainAt(out, in, d + 480 + 2399), 0.505, 1e-4);
    CHECK(gainAt(out, in, d + 480 + 4798) > 0.01 * (1.0 + 1e-5));
    for (int64_t n = d + 480 + 4799; n < static_cast<int64_t>(in.size()); ++n) {
        if (std::abs(gainAt(out, in, n) - 0.01) > 1e-8) {
            INFO(std::to_string(n));
            CHECK_APPROX_TOL(gainAt(out, in, n), 0.01, 1e-6, 0.0);
            break;
        }
    }
}

TEST_CASE("the gate opens over the attack, from wherever it is") {
    // Attack 10 ms (480 samples): halfway open halfway through, fully open at its end.
    Gate g(with(kBase, {{"attack", 10.f}}));
    const int64_t s = 4800;
    const Samples in = levels({{s, 0.001f}, {9600, 0.5f}});
    const Samples out = g.play(in);
    CHECK_APPROX_TOL(gainAt(out, in, s - 1), 0.01, 1e-6, 0.0);  // closed before
    CHECK_APPROX_TOL(gainAt(out, in, s), expectedGain(1.0 / 480, -40.f), 1e-5, 0.0);
    CHECK_NEAR(gainAt(out, in, s + 239), 0.505, 1e-4);
    CHECK(out[s + 478] < 0.5f);
    CHECK_ARRAY_EQUAL(slice(out, s + 479), slice(in, s + 479));

    // Opening again during the release ramps up from where it was: the gain never
    // moves faster than the attack's steepest (smoothstep's 1.5 per ramp length).
    Gate again(with(kBase, {{"attack", 10.f}}));
    const int64_t drop = 4800, back = drop + 480 + 1000;  // a fifth of the way through the release
    const Samples burst = levels({{drop, 0.5f}, {back - drop, 0.001f}, {9600, 0.5f}});
    const Samples gated = again.play(burst);
    std::vector<float> gains(gated.size());
    for (size_t i = 0; i < gains.size(); ++i)
        gains[i] = static_cast<float>(gainAt(gated, burst, static_cast<int64_t>(i)));
    const double steepest = 1.5 * 0.99 / 480;
    CHECK(largestStep(gains, drop + 480) <= steepest * 1.001);
    CHECK(gains[static_cast<size_t>(back)] > 0.85f);  // (it was still mostly open: 0.79 of the way)
    CHECK(gains[static_cast<size_t>(back)] > gains[static_cast<size_t>(back - 1)]);
    CHECK_EQ(gains.back(), 1.f);
}

TEST_CASE("the gate's Return keeps it open until the level falls that far below the threshold") {
    // Threshold -20 dB (0.1), Return 6: it closes below -26 dB (0.0501).
    const Samples in = levels({{12000, 0.2f}, {24000, 0.07f}, {24000, 0.04f}});
    Gate g(with(kBase, {{"threshold", -20.f}, {"return", 6.f}}));
    const Samples out = g.play(in);
    CHECK_ARRAY_EQUAL(slice(out, 4, 36000), slice(in, 4, 36000));  // 0.07 is above -26 dB: open throughout
    CHECK_APPROX_TOL(gainAt(out, in, static_cast<int64_t>(in.size()) - 1), 0.01, 1e-6, 0.0);
    CHECK_APPROX_TOL(gainAt(out, in, 36000 + 480 + 4799), 0.01, 1e-6, 0.0);  // hold, release, closed
    // With no Return the same 0.07 closes it.
    Gate none(with(kBase, {{"threshold", -20.f}, {"return", 0.f}}));
    const Samples closed = none.play(in);
    CHECK_ARRAY_EQUAL(slice(closed, 4, 12480), slice(in, 4, 12480));
    CHECK_APPROX_TOL(gainAt(closed, in, 12000 + 480 + 4799), 0.01, 1e-6, 0.0);
    // And a level between the closing level and the threshold doesn't open a closed gate.
    Gate between(with(kBase, {{"threshold", -20.f}, {"return", 6.f}}));
    const Samples quiet = levels({{12000, 0.07f}});
    CHECK(allclose(between.play(quiet), 0.07 * 0.01, 1e-6, 0.0));
}

TEST_CASE("the gate's Hold keeps it open after the level falls, to the sample") {
    for (const auto& [ms, samples] : std::vector<std::pair<float, int64_t>>{{50.f, 2400}, {1.f, 48}, {1500.f, 72000}}) {
        INFO(std::to_string(ms) + " ms");
        Gate g(with(kBase, {{"threshold", -20.f}, {"hold", ms}, {"release", 1.f}}));  // release: 48 samples
        const int64_t d = 4800;
        const Samples in = levels({{d, 0.5f}, {samples + 4800, 0.001f}});
        const Samples out = g.play(in);
        CHECK_ARRAY_EQUAL(slice(out, 4, d + samples), slice(in, 4, d + samples));
        CHECK(out[static_cast<size_t>(d + samples)] < in[static_cast<size_t>(d + samples)]);
        CHECK(gainAt(out, in, d + samples + 46) > 0.01 * (1.0 + 1e-5));
        CHECK_APPROX_TOL(gainAt(out, in, d + samples + 47), 0.01, 1e-6, 0.0);
    }
}

TEST_CASE("the gate's Floor is how far a closed gate turns the sound down") {
    // At its bottom (-75 dB, shown as -inf): silence.
    Gate muted(with(kBase, {{"floor", -75.f}}));
    const Samples bursts = levels({{4800, 0.5f}, {24000, 0.001f}, {4800, 0.5f}, {24000, 0.002f}});
    const Samples out = muted.play(bursts);
    CHECK(allEqual(slice(out, 4800 + 480 + 4800, 4800 + 24000), 0.0));
    CHECK(allEqual(slice(out, 2 * 4800 + 24000 + 480 + 4800), 0.0));
    Gate shut(with(kBase, {{"floor", -75.f}, {"threshold", 6.f}}));
    CHECK(allEqual(shut.play(noise(9600, 1)), 0.0));
    // At 0 dB: no effect, whatever the key does.
    for (const float threshold : {-40.f, 6.f}) {
        Gate open(with(kBase, {{"floor", 0.f}, {"threshold", threshold}}));
        const Samples in = noise(24000, 2);
        CHECK_ARRAY_EQUAL(open.playStereo(in), in);
    }
    // At -20 dB: a tenth.
    Gate tenth(with(kBase, {{"floor", -20.f}, {"threshold", 6.f}}));
    const Samples in = noise(4800, 3);
    const Samples ducked = tenth.play(in);
    for (size_t i = 0; i < in.size(); i += 97) CHECK_APPROX_TOL(ducked[i], in[i] * 0.1, 1e-6, 1e-9);
    CHECK_APPROX(gate::floorGain(-20.f), 0.1);
    CHECK_EQ(gate::floorGain(-75.f), 0.f);
    CHECK(gate::floorGain(-74.f) > 0.f);
}

TEST_CASE("the gate flipped passes only what is below the threshold") {
    Gate loud(with(kBase, {{"flip", 1.f}}));
    const Samples above = levels({{9600, 0.5f}});
    const Samples out = loud.play(above);
    for (const int64_t n : {int64_t{960}, int64_t{5000}, int64_t{9599}})
        CHECK_APPROX_TOL(out[n], 0.5 * 0.01, 1e-6, 0.0);
    Gate quiet(with(kBase, {{"flip", 1.f}}));
    const Samples below = levels({{9600, 0.001f}});
    CHECK_ARRAY_EQUAL(quiet.play(below), below);
    // The state, Return and Hold work as unflipped: flipped, the release is the sound coming back.
    Gate back(with(kBase, {{"flip", 1.f}}));
    const Samples step = levels({{9600, 0.5f}, {9600, 0.001f}});
    const Samples returned = back.play(step);
    CHECK_APPROX_TOL(gainAt(returned, step, 9600 + 479), 0.01, 1e-6, 0.0);  // still held
    CHECK_NEAR(gainAt(returned, step, 9600 + 480 + 2399), 0.505, 1e-4);
    CHECK_EQ(returned.back(), step.back());

    // Switched on while the gate is open: the output fades to the floor over 10 ms.
    Gate g;
    const Samples dc = levels({{9600, 1.f}});
    const Samples flipped = g.play(dc, {{4800, "flip", 1.f}});
    CHECK_ARRAY_EQUAL(slice(flipped, 4, 4800), slice(dc, 4, 4800));
    CHECK(largestStep(flipped, 4800) <= 1.1 * 0.99 / 480);
    CHECK(flipped[4800 + 478] > 0.0101f);
    CHECK_APPROX_TOL(flipped[4800 + 479], 0.01, 1e-6, 0.0);
    CHECK_APPROX_TOL(flipped.back(), 0.01, 1e-6, 0.0);
}

TEST_CASE("the gate's lookahead is latency: it opens before a transient reaches the output") {
    // 0, 1 or 10 ms, as whole samples.
    for (const auto& [rate, want] :
         std::vector<std::pair<double, std::vector<int>>>{{48000.0, {0, 48, 480}}, {44100.0, {0, 44, 441}}}) {
        Gate g(kBase, rate);
        for (int i = 0; i < 3; ++i) {
            g.set("lookahead", static_cast<float>(i));
            CHECK_EQ(g.processor().latencySamples(), want[static_cast<size_t>(i)]);
            CHECK_EQ(g.processor().tailSamples(), want[static_cast<size_t>(i)]);
            CHECK_EQ(gate::lookaheadSamples(i, rate), want[static_cast<size_t>(i)]);
        }
    }
    // idle() says once when it changed (the engine realigns the tracks).
    Gate g;
    CHECK(!g.processor().idle());
    g.set("lookahead", 2.f);
    CHECK(g.processor().idle());
    CHECK(!g.processor().idle());
    g.set("lookahead", 2.f);
    CHECK(!g.processor().idle());
    g.set("lookahead", 0.f);
    CHECK(g.processor().idle());

    // 10 ms and an instant attack: open (fully) as the transient's lookahead begins.
    Gate ahead(with(kBase, {{"lookahead", 2.f}, {"attack", 0.02f}}));
    const int64_t s = 4800, d = 9600;
    const Samples in = levels({{s, 0.001f}, {d - s, 0.5f}, {9600, 0.001f}});
    const Samples out = ahead.play(in);
    CHECK(allEqual(slice(out, 0, 480), 0.0));                              // (the delay starts empty)
    CHECK_APPROX_TOL(out[s - 1], 0.001 * 0.01, 1e-5, 0.0);                 // closed before
    CHECK(allEqual(slice(out, s, s + 480), static_cast<double>(0.001f)));  // opened early
    CHECK_EQ(out[s + 480], 0.5f);
    // The hold counts from when the drop reaches the output.
    CHECK_ARRAY_EQUAL(slice(out, s + 480, d + 960), slice(in, s, d + 480));
    CHECK(out[d + 960] < 0.001f);

    // A new lookahead's window holds what the key did before it was chosen: a burst on
    // the sidechain 200 samples back (outside 1 ms's window, inside 10 ms's) opens the
    // gate the moment 10 ms is chosen, and keeps it open until the burst leaves the
    // window, then the hold (1 ms), then closed (floor -inf, so the gain shows as
    // silence or the input). Back to 1 ms, the burst is out of the window at once.
    {
        const int64_t burst = 2000, change = burst + 200;
        const Samples quiet = levels({{9600, 0.001f}});
        const Samples key = levels({{burst, 0.f}, {10, 0.5f}, {9600 - burst - 10, 0.f}});
        const Values values =
            with(kBase, {{"lookahead", 1.f}, {"attack", 0.02f}, {"hold", 1.f}, {"release", 0.1f}, {"floor", -75.f}});
        Gate longer(values);
        const Samples out = longer.play(quiet, {{change, "lookahead", 2.f}}, 256, Key{&key});
        CHECK_EQ(out[change - 1], 0.f);
        CHECK_EQ(out[change], 0.001f);
        const int64_t leaves = burst + 9 + 481;  // the first frame whose window doesn't hold the burst
        CHECK_EQ(out[leaves + 47], 0.001f);
        CHECK(out[leaves + 48] < 0.001f);
        CHECK_EQ(out[leaves + 48 + 4], 0.f);
        Gate shorter(with(values, {{"lookahead", 2.f}}));
        const Samples back = shorter.play(quiet, {{change, "lookahead", 1.f}}, 256, Key{&key});
        CHECK_EQ(back[change - 1], 0.001f);
        CHECK_EQ(back[change + 47], 0.001f);  // (the hold)
        CHECK(back[change + 48] < 0.001f);
        CHECK_EQ(back[change + 48 + 4], 0.f);
    }

    // Through the engine, the output is lined up with the timeline: open a sample before the step.
    GateEngine e;
    auto& engine = e.engine;
    const int64_t step = 12000;
    const uint32_t track = clipTrackOf(engine, levels({{step, 0.001f}, {36000, 0.5f}}), 1.0);
    const uint32_t device = gateOn(engine, track, with(kBase, {{"lookahead", 2.f}, {"attack", 0.02f}}));
    CHECK_EQ(engine.processorInfo(device).latency, 480);
    CHECK_EQ(engine.processorInfo(device).tail, 480);
    const Samples left = channel(engine.renderOffline(0.0, 24000), 0);
    CHECK_EQ(left[step], 0.5f);
    CHECK_EQ(left[step - 1], quantized(0.001f));                              // open already
    CHECK_EQ(left[step - 480], quantized(0.001f));                            // the lookahead before it
    CHECK_APPROX_TOL(left[step - 481], quantized(0.001f) * 0.01, 1e-5, 0.0);  // closed until then
}

TEST_CASE("a sidechain keys the gate, after its gain and blended by its mix") {
    GateEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrackOf(engine, levels({{48000, 0.5f}}), 1.0);
    const uint32_t device = gateOn(engine, track);
    const auto keyTrack = [&](float level) {
        const uint32_t key = clipTrackOf(engine, levels({{48000, level}}), 1.0);
        engine.setTrackGain(key, 0.f);  // heard only through the sidechain, taken before its fader
        return key;
    };
    const auto last = [&] { return at(engine.renderOffline(0.0, kSampleRate / 2), -1, 0); };
    const uint32_t silent = engine.addTrack();
    const uint32_t half = keyTrack(0.5f), small = keyTrack(0.05f);

    // A silent key keeps it closed (it doesn't fall back to its own input); a loud one opens it.
    engine.setProcessorSidechain(device, silent, sub::SidechainTap::PreFader);
    CHECK_APPROX_TOL(last(), 0.5 * 0.01, 1e-5, 0.0);
    engine.setProcessorSidechain(device, half, sub::SidechainTap::PreFader);
    CHECK_EQ(last(), 0.5f);
    // S/C Mix 0: its own input keys it again; 50 %: half each (a silent key: 0.25 of the input, -12 dB).
    engine.setProcessorSidechain(device, silent, sub::SidechainTap::PreFader);
    setParam(engine, device, "sc_mix", 0.f);
    CHECK_EQ(last(), 0.5f);
    setParam(engine, device, "sc_mix", 50.f);
    setParam(engine, device, "threshold", -10.f);
    CHECK_APPROX_TOL(last(), 0.5 * 0.01, 1e-5, 0.0);
    setParam(engine, device, "threshold", -14.f);
    CHECK_EQ(last(), 0.5f);
    // S/C Gain: a key at 0.05 doesn't reach -20 dB; 12 dB louder it does.
    setParam(engine, device, "sc_mix", 100.f);
    setParam(engine, device, "threshold", -20.f);
    engine.setProcessorSidechain(device, small, sub::SidechainTap::PreFader);
    CHECK_APPROX_TOL(last(), 0.5 * 0.01, 1e-5, 0.0);
    setParam(engine, device, "sc_gain", 12.f);
    CHECK_EQ(last(), 0.5f);

    // Without a sidechain, its gain and mix change nothing: its own input keys it.
    engine.clearProcessorSidechain(device);
    setParam(engine, device, "threshold", -40.f);
    const Samples plain = channel(engine.renderOffline(0.0, kSampleRate / 2), 0);
    setParam(engine, device, "sc_gain", -70.f);
    CHECK_ARRAY_EQUAL(channel(engine.renderOffline(0.0, kSampleRate / 2), 0), plain);
    CHECK_EQ(plain.back(), 0.5f);

    // Standalone, on a stretch of the key: bit-equal to the gate keyed by it at full mix.
    const Samples quiet = levels({{9600, 0.001f}});
    const Samples key = levels({{2400, 0.f}, {4800, 0.5f}, {2400, 0.f}});
    Gate keyed;
    const Samples out = keyed.play(quiet, {}, 256, Key{&key});
    CHECK_APPROX_TOL(out[2399], 0.001 * 0.01, 1e-5, 0.0);
    CHECK_EQ(out[2404], 0.001f);
    CHECK_EQ(out[7200 + 479], 0.001f);  // held after the key falls
    CHECK(out[7200 + 480] < 0.001f);
}

TEST_CASE("the gate's key EQ opens it from a band only, as its editor draws it") {
    const auto ratio = [](const Samples& out, const Samples& in) { return tailRms(out, 12000) / tailRms(in, 12000); };
    const Samples low = tone(100.0, 24000, 0.5);
    Gate open(with(kBase, {{"threshold", -20.f}}));
    CHECK(ratio(open.play(low), low) > 0.999);
    // High-pass at 2 kHz: the 100 Hz key is about 52 dB down, under the threshold.
    Gate highPass(
        with(kBase,
             {{"threshold", -20.f}, {"sc_eq", 1.f}, {"sc_eq_type", 5.f}, {"sc_eq_freq", 2000.f}, {"sc_eq_q", 0.71f}}));
    CHECK_APPROX_TOL(ratio(highPass.play(low), low), 0.01, 1e-3, 0.0);
    Gate lowPass(with(kBase, {{"threshold", -20.f}, {"sc_eq", 1.f}, {"sc_eq_type", 3.f}, {"sc_eq_freq", 2000.f}}));
    CHECK(ratio(lowPass.play(low), low) > 0.999);
    // A bell lifting the key 15 dB opens it where the input alone wouldn't.
    const Samples soft = tone(100.0, 24000, 0.2);
    Gate bell(with(kBase, {{"threshold", -10.f},
                           {"sc_eq", 1.f},
                           {"sc_eq_type", 1.f},
                           {"sc_eq_freq", 100.f},
                           {"sc_eq_q", 1.f},
                           {"sc_eq_gain", 15.f}}));
    CHECK(ratio(bell.play(soft), soft) > 0.999);
    Gate flat(with(kBase, {{"threshold", -10.f}}));
    CHECK_APPROX_TOL(ratio(flat.play(soft), soft), 0.01, 1e-3, 0.0);

    // Each type plays as drawn: listening, the key's level is the input's times the
    // response gate::keyFilterDb gives (what the editor's curve is made of).
    for (int type = 0; type < gate::kKeyFilters; ++type) {
        for (const double freq : {100.0, 1000.0, 8000.0}) {
            INFO("type " + std::to_string(type) + " at " + std::to_string(freq) + " Hz");
            Gate g(with(kBase, {{"sc_eq", 1.f},
                                {"sc_eq_type", static_cast<float>(type)},
                                {"sc_eq_freq", 1000.f},
                                {"sc_eq_q", 2.f},
                                {"sc_eq_gain", 9.f},
                                {"sc_listen", 1.f}}));
            const Samples in = tone(freq, 24000, 0.25);
            const double heard = db(ratio(g.play(in), in));
            const double drawn =
                gate::keyFilterDb(static_cast<gate::KeyFilter>(type), 1000.0, 2.0, 9.0, kSampleRate, freq);
            CHECK_NEAR(heard, drawn, 0.1);
        }
    }
    // Which types use Q and which gain (the editor dims the others).
    CHECK(gate::keyFilterUsesGain(gate::KeyFilter::Bell) && !gate::keyFilterUsesGain(gate::KeyFilter::LowPass));
    CHECK(!gate::keyFilterUsesQ(gate::KeyFilter::LowShelf) && gate::keyFilterUsesQ(gate::KeyFilter::BandPass));
    // Kept below Nyquist: 15 kHz at 22.05 kHz is designed at 0.45 of the rate.
    CHECK_APPROX(gate::keyFilterDb(gate::KeyFilter::Bell, 15000.0, 2.0, 9.0, 22050.0, 9922.5), 9.0);
}

TEST_CASE("the gate listening puts out the key instead") {
    // The key, delayed by the lookahead as the audio is, whatever the gate does.
    const Samples in = levels({{9600, 0.5f}}), key = levels({{9600, 0.3f}});
    Gate g(with(kBase, {{"sc_listen", 1.f}, {"lookahead", 1.f}}));
    const Samples out = g.playStereo(in, {}, 256, Key{&key});
    CHECK(allEqual(slice(out, 0, 48), 0.0));
    CHECK(allclose(slice(out, 48), 0.3, 0.0, 1e-6));

    // Through the EQ: exactly the key through gate::keyFilter, delayed by the lookahead.
    const Samples mixed = plus(tone(5000.0, 9600, 0.3), tone(100.0, 9600, 0.3));
    Gate eq(with(kBase, {{"sc_listen", 1.f},
                         {"lookahead", 1.f},
                         {"sc_eq", 1.f},
                         {"sc_eq_type", 3.f},
                         {"sc_eq_freq", 500.f},
                         {"sc_eq_q", 0.71f}}));
    const Samples heard = eq.playStereo(in, {}, 256, Key{&mixed});
    const sub::dsp::BiquadCoefficients lowPass =
        gate::keyFilter(gate::KeyFilter::LowPass, 500.0, 0.71, 0.0, kSampleRate);
    sub::dsp::Biquad state;
    Samples want(mixed.size());
    for (size_t i = 0; i < want.size(); ++i) want[i] = state.process(lowPass, mixed[i]);
    CHECK_ALLCLOSE(slice(heard, 48), slice(want, 0, static_cast<int64_t>(want.size()) - 48), 0.0, 1e-5);
    CHECK(rms(slice(heard, 4800)) < 0.25);  // (the 5 kHz is gone)

    // On one channel: the key's two channels' mean.
    const Samples left = levels({{4800, 0.3f}}), right = levels({{4800, 0.1f}});
    Gate mono(with(kBase, {{"sc_listen", 1.f}}));
    const Samples one = mono.play(levels({{4800, 0.5f}}), {}, 256, Key{&left, &right});
    CHECK(allclose(one, 0.2, 0.0, 1e-6));
}

TEST_CASE("every gate control moves without a jump") {
    // Floor, from -inf to 0 dB while closed: a 20 ms ramp.
    {
        Gate g(with(kBase, {{"threshold", 6.f}, {"floor", -75.f}}));
        const Samples out = g.play(levels({{9600, 1.f}}), {{4800, "floor", 0.f}});
        CHECK(largestStep(out) <= 1.1 / 960);
        CHECK_EQ(out[4799], 0.f);
        CHECK_EQ(out.back(), 1.f);
    }
    // Listen on and off: a 10 ms crossfade between the output and the key.
    {
        const Samples key = levels({{14400, 0.2f}});
        Gate g;
        const Samples out =
            g.play(levels({{14400, 1.f}}), {{4800, "sc_listen", 1.f}, {9600, "sc_listen", 0.f}}, 256, Key{&key});
        CHECK(largestStep(out, 100) <= 1.1 * 0.8 / 480);
        CHECK_APPROX(out[9599], 0.2);
        CHECK_EQ(out.back(), 1.f);
    }
    // Lookahead 0 to 10 ms on a 70 Hz tone (floor 0 dB): a crossfade between the delay's taps.
    {
        Gate g(with(kBase, {{"floor", 0.f}}));
        const Samples in = tone(70.0, 24000, 0.5);
        const int64_t change = 9154;  // (where the old and new taps are furthest apart)
        const Samples out = g.play(in, {{change, "lookahead", 2.f}});
        CHECK(largestStep(out) < 0.01);
        Samples hard = in;  // (what a switch without the crossfade would do: about 0.8)
        for (size_t i = change; i < hard.size(); ++i) hard[i] = in[i - 480];
        CHECK(largestStep(hard) > 0.6);
        CHECK_ARRAY_EQUAL(slice(out, change + 480), slice(hard, change + 480));
    }
    // The key EQ while listening to it (a 1 kHz tone): its type, its frequency jumping, on and off.
    const Samples sine = tone(1000.0, 24000, 0.5);
    const double bound = 1.5 * largestStep(sine) + 0.01;
    const Values listening =
        with(kBase, {{"sc_listen", 1.f}, {"sc_eq", 1.f}, {"sc_eq_freq", 1000.f}, {"sc_eq_q", 1.f}});
    for (const auto& [name, changes] : std::vector<std::pair<std::string, std::vector<Change>>>{
             {"type",
              {{4800, "sc_eq_type", 1.f},
               {9000, "sc_eq_type", 3.f},
               {9100, "sc_eq_type", 0.f},
               {14000, "sc_eq_type", 4.f},
               {19000, "sc_eq_type", 5.f}}},
             {"freq", {{4800, "sc_eq_freq", 100.f}, {9600, "sc_eq_freq", 5000.f}, {14400, "sc_eq_freq", 30.f}}},
             {"on and off",
              {{4800, "sc_eq", 0.f}, {9600, "sc_eq", 1.f}, {9700, "sc_eq", 0.f}, {14400, "sc_eq", 1.f}}}}) {
        INFO(name);
        Gate g(listening);
        const Samples out = g.play(sine, changes);
        CHECK(largestStep(out) < bound);
    }
    // A sidechain chosen while listening (DC 0.5 in, 0.1 on the sidechain): the key
    // glides to it over 20 ms. Removed, its sound is gone at once (as when its source
    // stops), and the key glides back to the input from silence.
    {
        const Samples key = levels({{14336, 0.1f}});
        Gate g(with(kBase, {{"sc_listen", 1.f}}));
        const Samples out = g.play(levels({{14336, 0.5f}}), {}, 256, Key{&key, nullptr, 4864, 9728});
        CHECK(largestStep(out, 1, 9728) <= 1.1 * 0.4 / 960);
        CHECK_APPROX(out[9727], 0.1);
        CHECK(std::abs(out[9728]) <= 1.1 * 0.5 / 960);
        CHECK(largestStep(out, 9729) <= 1.1 * 0.5 / 960);
        CHECK_APPROX(out.back(), 0.5);
    }
}

TEST_CASE("changing any gate control is click-free") {
    // Each control jumping as automation's steps make it, against the same change
    // made at once (the output with the old setting spliced to the output with the
    // new one): the 6th difference (a steep high-pass) of what comes out must be
    // far smaller. On a 220 Hz tone (at a peak where the change comes), sometimes
    // keyed by a 660 Hz sidechain and listening to it.
    const int64_t at = 12055, length = 36000;
    const Samples sine = tone(220.0, length, 0.5);
    const Samples key = tone(660.0, length, 0.4);
    struct Case {
        std::string name;
        Values base;
        std::vector<Change> before;  // what sets it moving first
        std::string id;
        float value;
        bool keyed = false;
    };
    const Values open = with(kBase, {{"threshold", -60.f}, {"attack", 3.5f}, {"release", 50.f}});
    const Values shut = with(open, {{"threshold", 6.f}});
    const Values listen = with(open, {{"sc_listen", 1.f}});
    const Values eq = with(listen, {{"sc_eq", 1.f}, {"sc_eq_type", 1.f}, {"sc_eq_freq", 400.f}, {"sc_eq_gain", 9.f}});
    const std::vector<Case> cases = {
        {"floor", shut, {}, "floor", -10.f},
        {"flip", open, {}, "flip", 1.f},
        {"threshold (closing it)", open, {}, "threshold", 6.f},
        {"threshold (opening it)", shut, {}, "threshold", -60.f},
        {"return (closing it)",
         with(open, {{"threshold", -8.f}, {"return", 24.f}}),
         {{at - 2000, "threshold", -1.f}},
         "return",
         0.f},
        {"attack (while opening)", with(shut, {{"attack", 150.f}}), {{at - 2000, "threshold", -60.f}}, "attack", 3.f},
        {"release (while closing)", open, {{at - 2000, "threshold", 6.f}}, "release", 3000.f},
        {"hold (while held)", with(open, {{"hold", 100.f}}), {{at - 2000, "threshold", 6.f}}, "hold", 1.f},
        {"lookahead", open, {}, "lookahead", 2.f},
        {"listen", open, {}, "sc_listen", 1.f, true},
        {"sc_gain (listening)", listen, {}, "sc_gain", -12.f, true},
        {"sc_mix (listening)", listen, {}, "sc_mix", 30.f, true},
        {"sc_eq on (listening)", with(listen, {{"sc_eq_type", 3.f}, {"sc_eq_freq", 400.f}}), {}, "sc_eq", 1.f, true},
        {"sc_eq off (listening)", with(eq, {{"sc_eq_type", 3.f}}), {}, "sc_eq", 0.f, true},
        {"sc_eq_type (listening)", eq, {}, "sc_eq_type", 4.f, true},
        {"sc_eq_freq (listening)", eq, {}, "sc_eq_freq", 3000.f, true},
        {"sc_eq_q (listening)", eq, {}, "sc_eq_q", 8.f, true},
        {"sc_eq_gain (listening)", eq, {}, "sc_eq_gain", -15.f, true},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        const Key sidechain = c.keyed ? Key{&key} : Key{};
        std::vector<Change> changes = c.before;
        changes.push_back({at, c.id, c.value});
        Gate moved(c.base), old(c.base), now(with(c.base, {{c.id, c.value}}));
        const Samples out = moved.play(sine, changes, 256, sidechain);
        Samples spliced = old.play(sine, c.before, 256, sidechain);
        const Samples after = now.play(sine, c.before, 256, sidechain);
        std::copy(after.begin() + at, after.end(), spliced.begin() + at);
        const double moving = clickiness(out, at - 100, length);
        const double atOnce = clickiness(spliced, at - 100, at + 100);
        INFO("moving " + std::to_string(moving) + ", at once " + std::to_string(atOnce));
        CHECK(allFinite(out));
        CHECK(moving * 50.0 < atOnce);
    }
}

TEST_CASE("the gate's automation plays to the sample, whatever the block size") {
    // Threshold from -60 to 0 dB at frame 24000 on DC 0.1: open before, the hold
    // starts at the change, the release after it.
    const int64_t at = 24000;
    const Samples in = levels({{48000, 0.1f}});
    for (const int block : {64, 128, 1000, 1024}) {
        INFO(std::to_string(block));
        Gate g(with(kBase, {{"threshold", -60.f}}));
        const Samples out = g.play(in, {{at, "threshold", 0.f}}, block);
        CHECK_ARRAY_EQUAL(slice(out, 4, at + 480), slice(in, 4, at + 480));
        CHECK(out[at + 480] < in[at + 480]);
        CHECK(gainAt(out, in, at + 480 + 4798) > 0.01 * (1.0 + 1e-5));
        CHECK_APPROX_TOL(gainAt(out, in, at + 480 + 4799), 0.01, 1e-6, 0.0);
    }

    // The EQ's glide keeps its pace however the stretches split: listening to an EQ'd
    // key of noise, its frequency jumping and its gain changing every 37 samples.
    const Samples hiss = noise(24000, 4);
    std::vector<Change> changes = {{1000, "sc_eq_freq", 2000.f}};
    for (int64_t n = 37; n < 24000; n += 37)
        changes.push_back({n, "sc_eq_gain", static_cast<float>(12.0 * std::sin(static_cast<double>(n) / 900.0))});
    std::sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return a.frame < b.frame; });
    const Values eq = with(kBase, {{"sc_listen", 1.f},
                                   {"sc_eq", 1.f},
                                   {"sc_eq_type", 1.f},
                                   {"sc_eq_freq", 200.f},
                                   {"sc_eq_q", 2.f},
                                   {"sc_eq_gain", 12.f}});
    Gate first(eq);
    const Samples want = first.play(hiss, changes, 64);
    for (const int block : {256, 1024}) {
        INFO(std::to_string(block));
        Gate g(eq);
        CHECK_ARRAY_EQUAL(g.play(hiss, changes, block), want);
    }

    // Through the engine: the hold starts at the automation's step, to the sample.
    GateEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrackOf(engine, levels({{96000, 0.1f}}), 2.0);
    const uint32_t device = gateOn(engine, track, with(kBase, {{"threshold", -60.f}}));
    const sub::ParamInfo threshold = paramInfo(engine, device, "threshold");
    using Points = std::vector<sub::AutomationPoint>;
    const float low = threshold.toNormalized(-60.f), high = threshold.toNormalized(0.f);
    engine.setTrackAutomation(track,
                              {{device, "threshold", Points{{0.0, low, 0.f}, {2.0, low, 0.f}, {2.0, high, 0.f}}}});
    const Samples left = channel(engine.renderOffline(0.0, 72000), 0);
    const float level = quantized(0.1f);
    const int64_t step = kSampleRate;  // beat 2 at 120 BPM
    CHECK(allEqual(slice(left, 4, step + 480), level));
    CHECK(left[step + 480] < level);
    CHECK_APPROX_TOL(left[step + 480 + 4799], level * 0.01, 1e-5, 0.0);
}

TEST_CASE("reset and a new sample rate start the gate closed and silent, at its settings") {
    Gate g(with(kBase, {{"lookahead", 2.f}}));
    g.play(levels({{9600, 0.8f}}));  // loud: open, the delay full
    g.processor().reset();
    CHECK(allEqual(g.play(Samples(4800, 0.f)), 0.0));
    g.play(levels({{9600, 0.8f}}));
    g.processor().reset();
    // The delay holds nothing old, and the gate is closed from the first sample.
    const Samples quiet = levels({{4800, 0.001f}});
    const Samples out = g.play(quiet);
    CHECK(allEqual(slice(out, 0, 480), 0.0));
    CHECK(allclose(slice(out, 480), 0.001 * 0.01, 1e-5, 0.0));

    // A sidechain connected at full mix: a key transient at the first sample after a
    // reset opens it at once (the blend starts where it is set, not ramping from the input).
    const Samples key = levels({{4800, 0.5f}});
    Gate keyed(with(kBase, {{"attack", 0.02f}}));
    keyed.play(quiet);  // (no sidechain: its blend rests on the input)
    keyed.processor().reset();
    const Samples opened = keyed.play(quiet, {}, 256, Key{&key});
    CHECK_EQ(opened[0], 0.001f);
    // And without a reset the blend glides in over 20 ms.
    Gate gliding(with(kBase, {{"attack", 0.02f}}));
    gliding.play(quiet);
    const Samples glided = gliding.play(quiet, {}, 256, Key{&key});
    CHECK(glided[0] < 0.001f);

    // A new rate: as a gate made at that rate, from silence.
    Gate moved(with(kBase, {{"lookahead", 2.f}}));
    moved.play(noise(9600, 5));
    moved.processor().prepare(96000.0, kBlock);
    CHECK_EQ(moved.processor().latencySamples(), 960);
    Gate at96(with(kBase, {{"lookahead", 2.f}}), 96000.0);
    const Samples bursts = plus(noise(19200, 6, 0.1f), levels({{4800, 0.f}, {4800, 0.4f}, {9600, 0.f}}));
    CHECK_ARRAY_EQUAL(moved.play(bursts), at96.play(bursts));
}

TEST_CASE("at the extremes the gate stays finite and never louder than its input") {
    const Samples low = tone(50.0, 48000, 0.9);
    const std::vector<std::pair<std::string, Values>> settings = {
        {"fastest", with(kBase, {{"attack", 0.02f}, {"hold", 1.f}, {"release", 0.1f}})},
        {"slowest", with(kBase, {{"attack", 150.f}, {"hold", 1500.f}, {"release", 3000.f}})},
        {"highest threshold", with(kBase, {{"threshold", 6.f}})},
        {"lowest threshold", with(kBase, {{"threshold", -70.f}})},
        {"widest return", with(kBase, {{"return", 24.f}, {"threshold", -6.f}})},
        {"flipped", with(kBase, {{"flip", 1.f}, {"floor", -75.f}})},
    };
    for (const auto& [name, values] : settings) {
        INFO(name);
        Gate g(values);
        const Samples out = g.playStereo(low);
        CHECK(allFinite(out));
        CHECK(neverLouder(low, out));
    }
    // DC at +6 dB's threshold (closed) and at -60 dB with the threshold at -70 (open).
    Gate shut(with(kBase, {{"threshold", 6.f}}));
    CHECK(allclose(shut.play(levels({{4800, 1.f}})), 0.01, 1e-6, 0.0));
    Gate open(with(kBase, {{"threshold", -70.f}}));
    const Samples faint = levels({{4800, 0.001f}});
    CHECK_ARRAY_EQUAL(slice(open.play(faint), 4), slice(faint, 4));

    // Every parameter at its minimum, then at its maximum, at every rate.
    for (const double rate : {8000.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
        for (const bool top : {false, true}) {
            INFO(std::to_string(rate) + (top ? " max" : " min"));
            Values values;
            for (const sub::ParamInfo& p : builtinInfo("gate").params)
                if (p.id != "sc_listen") values.emplace_back(p.id, top ? p.maxValue : p.minValue);
            Gate g(values, rate);
            const Samples in =
                plus(noise(static_cast<int64_t>(rate), 7, 0.5f), tone(50.0, static_cast<int64_t>(rate), 0.4, rate));
            const Samples key = noise(static_cast<int64_t>(rate), 8, 1.f);
            Samples l = in, r = in;
            g.run({&l, &r}, {}, 256, Key{&key});
            CHECK(allFinite(l) && allFinite(r));
            const int64_t delay = gate::lookaheadSamples(top ? 2 : 0, rate);
            CHECK(neverLouder(in, l, delay));
        }
    }
}

TEST_CASE("the gate stays stable, and silence rings out to exact zeros") {
    // Noise in and on the sidechain (24 dB up), the EQ on each type at its narrowest
    // and widest, cut and boosted, at both ends of its range; listening, so the
    // filtered key is heard. Then silence: the EQ's states die away to exact zeros,
    // as soon as its slowest pole takes them below 1e-15 (where they are zeroed):
    // within a few milliseconds high up, 4 s for a bell 15 dB down at 30 Hz and
    // Q 0.1 (whose slowest pole is a real one near 1 Hz), 8.5 s for one 15 dB up
    // at Q 12.
    const Samples in = noise(24000, 9, 0.5f), key = noise(24000, 10, 1.f);
    for (int type = 0; type < gate::kKeyFilters; ++type) {
        for (const float q : {0.1f, 12.f}) {
            for (const float gain : {-15.f, 15.f}) {
                for (const float freq : {30.f, 15000.f}) {
                    INFO("type " + std::to_string(type) + ", Q " + std::to_string(q) + ", " + std::to_string(gain) +
                         " dB, " + std::to_string(freq) + " Hz");
                    const sub::dsp::BiquadCoefficients c =
                        gate::keyFilter(static_cast<gate::KeyFilter>(type), freq, q, gain, kSampleRate);
                    // The poles' largest radius, and how long from about 1e4 (the most the states hold) to 1e-20.
                    const double disc = c.a1 * c.a1 - 4.0 * c.a2;
                    const double radius =
                        disc < 0.0
                            ? std::sqrt(c.a2)
                            : std::max(std::abs(-c.a1 + std::sqrt(disc)), std::abs(-c.a1 - std::sqrt(disc))) / 2.0;
                    REQUIRE(radius < 1.0);
                    const auto ringOut = static_cast<int64_t>(std::log(1e-24) / std::log(radius)) + 4800;
                    Gate g(with(kBase, {{"sc_listen", 1.f},
                                        {"sc_eq", 1.f},
                                        {"sc_eq_type", static_cast<float>(type)},
                                        {"sc_eq_q", q},
                                        {"sc_eq_gain", gain},
                                        {"sc_eq_freq", freq},
                                        {"sc_gain", 24.f}}));
                    Samples x = in, k = key;
                    x.resize(static_cast<size_t>(24000 + ringOut + 4800), 0.f);
                    k.resize(x.size(), 0.f);
                    const Samples out = g.play(x, {}, 256, Key{&k});
                    CHECK(allFinite(out));
                    const std::vector<int64_t> sounding = nonzero(out);
                    REQUIRE(!sounding.empty());
                    INFO("rang until " + std::to_string(sounding.back()) + ", bound " +
                         std::to_string(24000 + ringOut));
                    CHECK(sounding.back() < 24000 + ringOut);
                    bool noDenormals = true;
                    for (const float v : out)
                        noDenormals = noDenormals && (v == 0.f || std::abs(v) >= std::numeric_limits<float>::min());
                    CHECK(noDenormals);
                }
            }
        }
    }
    // The settings it is usually at ring out within a fifth of a second.
    Gate usual(with(kBase, {{"sc_listen", 1.f}, {"sc_eq", 1.f}}));  // high-pass at 80 Hz, Q 0.71
    Samples x = in;
    x.resize(48000, 0.f);
    const std::vector<int64_t> sounding = nonzero(usual.play(x));
    REQUIRE(!sounding.empty());
    CHECK(sounding.back() < 24000 + 9600);
    // Ten seconds of noise through the gate with an EQ'd sidechain, not listening.
    Gate g(with(kBase, {{"sc_eq", 1.f},
                        {"sc_eq_type", 1.f},
                        {"sc_eq_q", 12.f},
                        {"sc_eq_gain", 15.f},
                        {"sc_gain", 24.f},
                        {"threshold", -20.f}}));
    const Samples long_ = noise(10 * kSampleRate, 11, 0.5f), longKey = noise(10 * kSampleRate, 12, 1.f);
    Samples l = long_, r = long_;
    g.run({&l, &r}, {}, 256, Key{&longKey});
    CHECK(allFinite(l));
    CHECK(neverLouder(long_, l));
    // Without the EQ, silence after sound is silence at once (no lookahead) or after it.
    Gate plain(with(kBase, {{"lookahead", 2.f}}));
    Samples burst = noise(4800, 13);
    burst.resize(9600, 0.f);
    const Samples out = plain.play(burst);
    CHECK(allEqual(slice(out, 4800 + 480), 0.0));
}

TEST_CASE("on one channel the gate gates as two equal channels do") {
    for (const Samples& in : {levels({{9600, 0.5f}, {9600, 0.001f}}), noise(19200, 14, 0.3f)}) {
        for (const Values& values : {kBase, with(kBase, {{"lookahead", 1.f}, {"threshold", -12.f}}),
                                     with(kBase, {{"sc_eq", 1.f}, {"sc_eq_type", 1.f}, {"sc_eq_gain", 12.f}})}) {
            Gate one(values), two(values);
            CHECK_ARRAY_EQUAL(one.play(in), two.playStereo(in));
        }
    }
    // A sidechain still keys it.
    const Samples quiet = levels({{9600, 0.001f}}), key = levels({{4800, 0.f}, {4800, 0.5f}});
    Gate keyed;
    const Samples out = keyed.play(quiet, {}, 256, Key{&key});
    CHECK_APPROX_TOL(out[4799], 0.001 * 0.01, 1e-5, 0.0);
    CHECK_EQ(out.back(), 0.001f);
    // And it goes back to two channels without the right's old delay.
    Gate both(with(kBase, {{"lookahead", 2.f}, {"threshold", -70.f}}));
    both.play(levels({{4800, 0.5f}}));
    Samples l = levels({{4800, 0.2f}}), r = l;
    both.run({&l, &r});
    CHECK(allEqual(slice(r, 0, 480), 0.0));
    CHECK(allclose(slice(r, 480), 0.2, 0.0, 1e-7));
}

TEST_CASE("the gate's displays: the levels in, out and of the key, and how much passed") {
    Gate g({});  // the defaults: threshold -12 dB, 1 ms of lookahead, 3.5 ms attack
    std::vector<std::pair<std::string, int>> displays;
    for (const auto& d : g.processor().displays()) displays.emplace_back(d.id, d.samplesPerValue);
    CHECK(displays ==
          (std::vector<std::pair<std::string, int>>{{"input", 256}, {"output", 256}, {"key", 256}, {"open", 256}}));
    g.playStereo(levels({{256 * 100, 0.5f}}));
    const std::vector<float> input = g.display(0), output = g.display(1), key = g.display(2), open = g.display(3);
    REQUIRE(input.size() == 100);
    CHECK_EQ(output.size(), size_t{100});
    CHECK_EQ(key.size(), size_t{100});
    CHECK_EQ(open.size(), size_t{100});
    const double level = 20.0 * std::log10(0.5);
    CHECK_NEAR(input.back(), level, 0.01);
    CHECK_NEAR(output.back(), level, 0.01);
    CHECK_NEAR(key.back(), level, 0.01);
    CHECK(open[0] < 1.f);  // (opening over the 3.5 ms attack)
    for (size_t i = 1; i < open.size(); ++i) CHECK_EQ(open[i], 1.f);

    // Closed: the output 40 dB down (floored at -90), nothing passing.
    g.playStereo(levels({{256 * 40, 0.001f}}));
    const std::vector<float> in2 = g.display(0), out2 = g.display(1), open2 = g.display(3);
    REQUIRE(in2.size() == 40);
    CHECK_NEAR(in2.back(), -60.0, 0.01);
    CHECK_EQ(out2.back(), -90.f);
    CHECK_EQ(open2.back(), 0.f);

    // The four streams always hold the same count, whatever the block sizes.
    for (const int block : {1, 7, 100, 1024}) {
        Samples x = noise(3001, 15), y = x;
        g.run({&x, &y}, {}, block);
        std::vector<float> ignored;
        const uint64_t count = g.processor().readDisplay(0, 0, ignored);
        for (int i = 1; i < 4; ++i) CHECK_EQ(g.processor().readDisplay(i, 0, ignored), count);
    }

    // With a sidechain the key stream shows the sidechain's level, the input stream the input's.
    Gate keyed;
    const Samples sidechain = levels({{2560, 0.25f}});
    keyed.play(levels({{2560, 0.5f}}), {}, 256, Key{&sidechain});
    CHECK_NEAR(keyed.display(2).back(), 20.0 * std::log10(0.25), 0.01);
    CHECK_NEAR(keyed.display(0).back(), level, 0.01);
    // Flipped, "open" is what passes: nothing, with the key above the threshold.
    Gate flipped(with(kBase, {{"flip", 1.f}}));
    flipped.play(levels({{2560, 0.5f}}));
    CHECK_EQ(flipped.display(3).back(), 0.f);
}
