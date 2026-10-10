// The built-in Limiter: a brick-wall lookahead limiter after Live 12.1's. Below
// the ceiling it is the input, its lookahead late; no sample comes out above
// the ceiling in any mode, routing, link, lookahead or release, and in True
// Peak mode no peak between samples either (by a reference meter of the
// test's own); a lone peak is caught exactly, the gain falling only over the
// attack before it; the manual release and Auto's two stages; True Peak's
// interpolator and its parabola; Soft Clip's knee; Maximize; L/R, M/S and Link;
// every control changing without a click, the glides straight in dB;
// automation through the engine; reset and new rates; the extremes and input
// that isn't finite; silence ringing out to exact zeros; one channel; latency
// through the engine; and its displays.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "Engine.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/LimiterDesign.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;
namespace limiter = sub::limiter;

namespace {

constexpr int kBlock = 1024;  // the renderer's largest block (Renderer::kMaxBlock)
constexpr int kL = 144;       // the lookahead at 48 kHz, 3 ms (the default): the latency
constexpr int kD = limiter::kDetectorDelay;
constexpr int kS = kL + 1 - kD;  // the attack's length: 133

using Values = std::vector<std::pair<std::string, float>>;

// A parameter's change at a frame, as automation hands it over.
struct Change {
    int64_t frame;
    std::string id;
    float value;
};

// A Limiter on its own, outside an engine, at any sample rate: processed in
// blocks, its changes handed over as automation (so its blocks split there) as
// the renderer does.
class Limiter {
public:
    explicit Limiter(const Values& values = {}, double rate = kSampleRate)
        : processor_(sub::BuiltinRegistry::instance().create("limiter")), rate_(rate) {
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
    void prepare(double rate) {
        rate_ = rate;
        processor_->prepare(rate, kBlock);
    }

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
            ctx.samplePos = samplePos_ + start;
            processor_->process(ctx, pointers, static_cast<int>(channels.size()), n);
            processor_->clearAutomation();
        }
        samplePos_ += frames;
    }
    // One channel: what comes out.
    Samples play(Samples mono, const std::vector<Change>& changes = {}) {
        run({&mono}, changes);
        return mono;
    }
    // Two channels: what comes out.
    std::pair<Samples, Samples> play(Samples left, Samples right, const std::vector<Change>& changes = {}) {
        run({&left, &right}, changes);
        return {std::move(left), std::move(right)};
    }

    // Display `id`'s values since the last read of it.
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
    int64_t samplePos_ = 0;
    std::map<size_t, uint64_t> positions_;
};

// --- Signals -------------------------------------------------------------------------------

Samples tone(double freq, double seconds, double amplitude, double phase = 0.0, double rate = kSampleRate) {
    Samples x(static_cast<size_t>(std::llround(seconds * rate)));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate + phase));
    return x;
}

Samples noise(size_t length, unsigned seed, float amplitude = 0.5f) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> uniform(-amplitude, amplitude);
    Samples x(length);
    for (float& v : x) v = uniform(random);
    return x;
}

// Single samples of ±height at random places, about every 400 samples, in silence.
Samples spikes(size_t length, unsigned seed, float height) {
    std::mt19937 random(seed);
    std::uniform_int_distribution<int> gap(100, 700);
    Samples x(length, 0.f);
    for (size_t at = static_cast<size_t>(gap(random)); at < length; at += static_cast<size_t>(gap(random)))
        x[at] = (at % 2 == 0 ? 1.f : -1.f) * height;
    return x;
}

Samples square(double freq, double seconds, double amplitude, double rate = kSampleRate) {
    Samples x(static_cast<size_t>(std::llround(seconds * rate)));
    for (size_t i = 0; i < x.size(); ++i) {
        const double phase = std::fmod(freq * static_cast<double>(i) / rate, 1.0);
        x[i] = static_cast<float>(phase < 0.5 ? amplitude : -amplitude);
    }
    return x;
}

// ±amplitude every other sample: all of it at Nyquist.
Samples alternating(size_t length, float amplitude) {
    Samples x(length);
    for (size_t i = 0; i < length; ++i) x[i] = i % 2 == 0 ? amplitude : -amplitude;
    return x;
}

// Gaussian noise through a 255-tap windowed-sinc low-pass (Kaiser β = 10) at `cutoff` Hz.
Samples lowpassNoise(size_t length, unsigned seed, double sigma, double cutoff) {
    std::mt19937 random(seed);
    std::normal_distribution<double> normal(0.0, sigma);
    constexpr int kTapsFir = 255;
    std::vector<double> h(kTapsFir);
    double sum = 0.0;
    const double fc = cutoff / kSampleRate;
    const auto i0 = [](double x) {
        double s = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            s += term;
        }
        return s;
    };
    for (int n = 0; n < kTapsFir; ++n) {
        const double m = n - (kTapsFir - 1) / 2.0;
        const double sinc = m == 0.0 ? 2.0 * fc : std::sin(2.0 * kPi * fc * m) / (kPi * m);
        const double r = m / ((kTapsFir - 1) / 2.0);
        h[static_cast<size_t>(n)] = sinc * i0(10.0 * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0(10.0);
        sum += h[static_cast<size_t>(n)];
    }
    std::vector<double> white(length + kTapsFir);
    for (double& v : white) v = normal(random);
    Samples x(length);
    for (size_t i = 0; i < length; ++i) {
        double acc = 0.0;
        for (int n = 0; n < kTapsFir; ++n) acc += h[static_cast<size_t>(n)] / sum * white[i + static_cast<size_t>(n)];
        x[i] = static_cast<float>(acc);
    }
    return x;
}

Samples scaled(Samples x, double gain) {
    for (float& v : x) v = static_cast<float>(v * gain);
    return x;
}

// --- Measures ------------------------------------------------------------------------------

double db(double gain) { return 20.0 * std::log10(gain); }

double peak(const Samples& x, int64_t from = 0, int64_t to = std::numeric_limits<int64_t>::max()) {
    return maxAbs(slice(x, from, to));
}

// The output's post: the very float the device multiplies by at the end.
float postOf(const Values& values) {
    std::map<std::string, float> v = {{"gain", 0.f}, {"ceiling", -0.3f}, {"maximize", 0.f}, {"threshold", 0.f},
                                      {"output", -0.3f}};
    for (const auto& [id, value] : values) v[id] = value;
    return limiter::scales(v["maximize"] >= 0.5f, v["gain"], v["ceiling"], v["threshold"], v["output"]).post;
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

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 60; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

// A reference true-peak meter, independent of the device: 32 points per sample
// by a Kaiser-windowed sinc (β = 10) of 32 zero crossings each side,
// interpolated from the whole signal (a slice's own ends would ring); the most
// over [from, to).
double truePeak(const Samples& x, int64_t from, int64_t to) {
    constexpr int kPoints = 32, kHalf = 32;
    static const std::vector<std::vector<double>> kPhases = [] {
        std::vector<std::vector<double>> phases(kPoints - 1, std::vector<double>(2 * kHalf));
        const double i0Beta = besselI0(10.0);
        for (int k = 1; k < kPoints; ++k) {
            const double t = static_cast<double>(k) / kPoints;
            for (int i = 0; i < 2 * kHalf; ++i) {
                const double u = t - (i - (kHalf - 1));
                const double r = u / kHalf;
                phases[static_cast<size_t>(k - 1)][static_cast<size_t>(i)] =
                    std::sin(kPi * u) / (kPi * u) * besselI0(10.0 * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0Beta;
            }
        }
        return phases;
    }();
    const auto size = static_cast<int64_t>(x.size());
    to = std::min(to, size);
    double most = 0.0;
    for (int64_t n = std::max<int64_t>(0, from); n < to; ++n) {
        most = std::max(most, std::abs(static_cast<double>(x[static_cast<size_t>(n)])));
        for (const std::vector<double>& h : kPhases) {
            double sum = 0.0;
            for (int i = 0; i < 2 * kHalf; ++i) {
                const int64_t at = n + i - (kHalf - 1);
                if (at >= 0 && at < size) sum += h[static_cast<size_t>(i)] * x[static_cast<size_t>(at)];
            }
            most = std::max(most, std::abs(sum));
        }
    }
    return most;
}
double truePeakDb(const Samples& x, int64_t from, int64_t to) { return db(truePeak(x, from, to)); }

// The level of a frequency over [from, from + length): a Hann-windowed DFT bin, as an amplitude.
double levelAt(const Samples& x, double freq, int64_t from, int64_t length) {
    std::complex<double> sum = 0.0;
    double weights = 0.0;
    for (int64_t n = 0; n < length; ++n) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(n) / static_cast<double>(length));
        sum += w * static_cast<double>(x[static_cast<size_t>(from + n)]) *
               std::polar(1.0, -2.0 * kPi * freq * static_cast<double>(n) / kSampleRate);
        weights += w;
    }
    return 2.0 * std::abs(sum) / weights;
}

// x delayed by `frames` (zeros first), as long as x.
Samples delayed(const Samples& x, int64_t frames) {
    Samples out(x.size(), 0.f);
    for (size_t i = static_cast<size_t>(frames); i < x.size(); ++i) out[i] = x[i - static_cast<size_t>(frames)];
    return out;
}

float maxOfValues(const std::vector<float>& v, size_t from = 0) {
    float most = -std::numeric_limits<float>::infinity();
    for (size_t i = from; i < v.size(); ++i) most = std::max(most, v[i]);
    return most;
}

// A stereo test input: two channels and the gain they go in at.
struct Input {
    std::string name;
    Samples left, right;
    float gain = 0.f;
};

std::vector<Input> brickWallInputs() {
    const size_t n = kSampleRate / 4;
    return {
        {"sine 12 dB over", tone(100.0, 0.25, 4.0), tone(150.0, 0.25, 3.0, 1.0), 0.f},
        {"noise +20 dB", noise(n, 1), noise(n, 2), 20.f},
        {"spikes", spikes(n, 3, 8.f), spikes(n, 4, 8.f), 0.f},
        {"square", square(50.0, 0.25, 2.0), scaled(square(50.0, 0.25, 2.0), -0.7), 0.f},
        {"Nyquist +24 dB", alternating(n, 1.f), scaled(alternating(n, 1.f), 0.5), 24.f},
    };
}

// Whether a render of `input` with `values` stays at or under the ceiling, exactly.
bool holdsTheCeiling(const Input& input, Values values) {
    values.emplace_back("gain", input.gain);
    Limiter l(values);
    const auto [left, right] = l.play(input.left, input.right);
    const double ceiling = postOf(values);
    const double most = std::max(peak(left), peak(right));
    if (!allFinite(left) || !allFinite(right) || most > ceiling) {
        std::string settings;
        for (const auto& [id, value] : values) settings += " " + id + "=" + std::to_string(value);
        INFO(input.name + settings + ": peak " + std::to_string(most) + " over " + std::to_string(ceiling));
        CHECK(false);
        return false;
    }
    return true;
}

}  // namespace

TEST_CASE("the limiter is listed with its parameters") {
    const sub::BuiltinInfo info = builtinInfo("limiter");
    CHECK_EQ(info.name, std::string("Limiter"));
    CHECK(!info.isInstrument());
    CHECK(paramIds(info.params) == (std::vector<std::string>{"gain", "ceiling", "release", "auto_release", "lookahead",
                                                             "mode", "routing", "link", "maximize", "threshold",
                                                             "output"}));
    struct Expected {
        std::string unit;
        float min, max, def;
        bool log;
        std::vector<std::string> labels;
    };
    const std::vector<Expected> expected = {
        {"dB", -24.f, 24.f, 0.f, false, {}},
        {"dB", -24.f, 0.f, -0.3f, false, {}},
        {"ms", 0.1f, 3000.f, 300.f, true, {}},
        {"", 0.f, 1.f, 1.f, false, {"Off", "On"}},
        {"", 0.f, 2.f, 1.f, false, {"1.5 ms", "3 ms", "6 ms"}},
        {"", 0.f, 2.f, 0.f, false, {"Standard", "Soft Clip", "True Peak"}},
        {"", 0.f, 1.f, 0.f, false, {"L/R", "M/S"}},
        {"%", 0.f, 100.f, 100.f, false, {}},
        {"", 0.f, 1.f, 0.f, false, {"Off", "On"}},
        {"dB", -24.f, 0.f, 0.f, false, {}},
        {"dB", -24.f, 0.f, -0.3f, false, {}},
    };
    REQUIRE(info.params.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        const sub::ParamInfo& p = info.params[i];
        const Expected& e = expected[i];
        INFO(p.id);
        CHECK_EQ(p.unit, e.unit);
        CHECK_EQ(p.minValue, e.min);
        CHECK_EQ(p.maxValue, e.max);
        CHECK_EQ(p.defaultValue, e.def);
        CHECK_EQ(p.isLog(), e.log);
        CHECK(p.valueLabels == e.labels);
        CHECK_EQ(p.automatable, p.id != "lookahead");  // (the lookahead is the latency)
    }
    CHECK_EQ(info.params[2].name, std::string("Release"));
    CHECK_EQ(info.params[3].name, std::string("Auto Release"));

    Limiter l;
    const std::vector<sub::DisplayInfo> displays = l.processor().displays();
    const std::vector<std::string> ids = {"in_l", "in_r", "out_l", "out_r", "gr_a", "gr_b", "clip"};
    REQUIRE(displays.size() == ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        CHECK_EQ(displays[i].id, ids[i]);
        CHECK_EQ(displays[i].samplesPerValue, 128);
    }

    // Through an engine: its latency (and tail) is the lookahead.
    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "limiter", -1);
    const sub::ProcessorInfo processor = engine.processorInfo(id);
    CHECK_EQ(processor.name, std::string("Limiter"));
    CHECK_EQ(processor.latency, kL);
    CHECK_EQ(processor.tail, kL);
}

TEST_CASE("below the ceiling the limiter is the input, its lookahead late") {
    const Samples in = tone(1000.0, 0.5, 0.25);  // -12 dBFS
    Limiter l;
    const auto [left, right] = l.play(in, in);
    CHECK(allEqual(slice(left, 0, kL), 0.0));
    CHECK_ALLCLOSE(slice(left, kL), slice(in, 0, static_cast<int64_t>(in.size()) - kL), 1e-6, 1e-9);
    CHECK_ARRAY_EQUAL(right, left);
    for (const char* id : {"gr_a", "gr_b", "clip"}) {
        INFO(id);
        const std::vector<float> values = l.display(id);
        CHECK(!values.empty());
        CHECK(allEqual(values, 0.0));
    }
}

TEST_CASE("no sample comes out of the limiter above the ceiling, in any mode, routing or link") {
    const std::vector<Input> inputs = brickWallInputs();
    for (const Input& input : inputs) {
        for (const float routing : {0.f, 1.f}) {
            for (const float link : {0.f, 50.f, 100.f}) {
                for (const float mode : {0.f, 1.f, 2.f})
                    holdsTheCeiling(input, {{"routing", routing}, {"link", link}, {"mode", mode}});
            }
        }
    }
    // The other settings, one at a time.
    for (const Input& input : {inputs[1], inputs[2], inputs[4]}) {
        for (const float ceiling : {-0.3f, -6.f, -24.f}) holdsTheCeiling(input, {{"ceiling", ceiling}});
        for (const float lookahead : {0.f, 1.f, 2.f}) holdsTheCeiling(input, {{"lookahead", lookahead}});
        for (const float release : {0.1f, 300.f, 3000.f})
            holdsTheCeiling(input, {{"auto_release", 0.f}, {"release", release}});
        holdsTheCeiling(input, {{"auto_release", 1.f}});
    }

    // A sine 12 dB over settles with its peaks at the ceiling (True Peak's 0.02 dB under it).
    const float c = postOf({});
    const Samples loud = tone(1000.0, 0.5, 3.981 * c);
    for (const float mode : {0.f, 1.f, 2.f}) {
        INFO("mode " + std::to_string(mode));
        Limiter l({{"mode", mode}});
        const auto [left, right] = l.play(loud, loud);
        const double over = db(peak(left, kSampleRate / 4) / c);
        CHECK(over <= 0.0);
        CHECK(over >= -0.05);
    }
}

TEST_CASE("the limiter catches a lone peak exactly, the gain falling only over the attack before it") {
    constexpr int64_t m = 10000;
    Samples in(20000, 0.25f);
    in[m] = 4.f;
    Limiter l({{"ceiling", 0.f}});
    const Samples out = l.play(in);
    CHECK(out[m + kL] >= 0.99999f);
    CHECK(out[m + kL] <= 1.f);
    CHECK_ALLCLOSE(slice(out, kL, m + kD), 0.25, 0.0, 1e-6);
    // The attack: S - 1 samples of S-curve before the peak comes out, never rising.
    CHECK(out[m + kD] < 0.25f);
    CHECK_EQ(m + kL - (m + kD), int64_t{kS - 1});
    for (int64_t n = m + kD; n + 1 < m + kL; ++n) {
        INFO(std::to_string(n));
        CHECK(out[static_cast<size_t>(n + 1)] <= out[static_cast<size_t>(n)]);
    }
}

TEST_CASE("the limiter's manual release brings the gain back with its time constant") {
    // DC 0.5 with 100 ms of DC 2.0 (6 dB over), the ceiling at 0 dB, Release 100 ms.
    Samples in(kSampleRate * 3 / 2, 0.5f);
    std::fill(in.begin() + 24000, in.begin() + 28800, 2.f);
    Limiter l({{"auto_release", 0.f}, {"release", 100.f}, {"ceiling", 0.f}});
    const Samples out = l.play(in);
    CHECK_ALLCLOSE(slice(out, 24000 + kL, 28800 + kL), 1.0, 0.0, 1e-4);
    // The release starts as the burst's last sample leaves the hold; the boxes centre it (S - 1) / 2 late.
    const int64_t released = 28800 + kL + (kS - 1) / 2;
    CHECK_APPROX_REL(out[static_cast<size_t>(released + 4800)], 0.5 * (1.0 - 0.5 * std::exp(-1.0)), 0.02);
    CHECK_APPROX_REL(out[static_cast<size_t>(released + 5 * 4800)], 0.5, 0.005);
    CHECK(out[static_cast<size_t>(released + 5 * 4800)] < 0.5f);
}

TEST_CASE("the limiter's auto release is quick after a short peak and slow after sustained limiting") {
    const auto grAt = [](const std::vector<float>& gr, int64_t frame) { return gr[static_cast<size_t>(frame / 128)]; };
    // (a) A 5 ms burst 12 dB over a quiet tone: back within 150 ms (the fast stage).
    {
        Samples in = tone(1000.0, 1.0, 0.05);
        const Samples burst = tone(1000.0, 1.0, 4.0);
        std::copy(burst.begin() + 24000, burst.begin() + 24240, in.begin() + 24000);
        Limiter l;
        l.play(in, in);
        const std::vector<float> gr = l.display("gr_a");
        const int64_t out = 24240 + kL;
        CHECK(maxOfValues(gr) > 10.f);
        const float after = grAt(gr, out + 7200);
        INFO("150 ms after the burst: " + std::to_string(after) + " dB");
        CHECK(after < 0.5f);
        CHECK(after > 0.1f);
    }
    // (b) Two seconds of limiting by 6 dB: Auto's slow stage holds the gain down for longer.
    Samples in = tone(1000.0, 2.0, 2.0);
    in.resize(static_cast<size_t>(6 * kSampleRate), 0.f);
    const int64_t out = 2 * kSampleRate + kL;
    Limiter automatic;
    automatic.play(in, in);
    const std::vector<float> gr = automatic.display("gr_a");
    INFO("Auto: " + std::to_string(grAt(gr, out + 7200)) + " dB 150 ms after, " +
         std::to_string(grAt(gr, out + 3 * kSampleRate)) + " dB 3 s after");
    CHECK(grAt(gr, out - kSampleRate) > 6.f);
    CHECK(grAt(gr, out + 7200) > 2.f);
    CHECK(grAt(gr, out + 3 * kSampleRate) < 0.1f);
    Limiter manual({{"auto_release", 0.f}, {"release", 100.f}});
    manual.play(in, in);
    CHECK(grAt(manual.display("gr_a"), out + 600 * 48) < 0.5f);
}

TEST_CASE("the limiter's True Peak mode lets no peak between samples over the ceiling") {
    constexpr float kCeiling = -1.f;
    // A quarter of the rate at 45°: samples at ±0.7071, its true peak 1.0 between them.
    {
        Samples in = tone(kSampleRate / 4.0, 0.5, 1.0, kPi / 4.0);
        for (size_t i = 0; i < 480; ++i) in[i] *= static_cast<float>(i) / 480.f;  // (an abrupt start overshoots)
        Limiter standard({{"ceiling", kCeiling}});
        const Samples a = standard.play(in);
        CHECK_APPROX_TOL(db(peak(a, 4800)), -3.0103, 0.0, 0.01);  // untouched
        CHECK_APPROX_TOL(truePeakDb(a, 4800, 20000), 0.0, 0.0, 0.01);
        Limiter truePeak({{"ceiling", kCeiling}, {"mode", 2.f}});
        const Samples b = truePeak.play(in);
        const double tp = truePeakDb(b, 4800, 20000);
        INFO(std::to_string(tp));
        CHECK(tp <= kCeiling);
        CHECK(tp >= kCeiling - 0.05);
    }
    // Noise up to 16 kHz, 12 dB into it.
    {
        const Samples l = lowpassNoise(kSampleRate / 2, 5, 0.3, 16000.0);
        const Samples r = lowpassNoise(kSampleRate / 2, 6, 0.3, 16000.0);
        Limiter lim({{"ceiling", kCeiling}, {"mode", 2.f}, {"gain", 12.f}});
        const auto [left, right] = lim.play(l, r);
        const double tp = std::max(truePeakDb(left, 4800, 20000), truePeakDb(right, 4800, 20000));
        INFO("noise: true peak " + std::to_string(tp));
        CHECK(tp <= kCeiling + 0.01);
        CHECK(std::max(peak(left), peak(right)) <= postOf({{"ceiling", kCeiling}}));
        // A 4x meter after BS.1770, from the device's own phases (1/4, 2/4, 3/4), reads at most the ceiling.
        const limiter::Phases& phases = limiter::truePeakPhases();
        double most = 0.0;
        for (const Samples* x : {&left, &right}) {
            for (size_t n = 4800; n < 20000; ++n) {
                most = std::max(most, std::abs(static_cast<double>((*x)[n])));
                for (const size_t k : {1u, 3u, 5u}) {
                    const float point = limiter::interpolate(x->data() + n - 11, phases[k]);
                    most = std::max(most, std::abs(static_cast<double>(point)));
                }
            }
        }
        CHECK(db(most) <= kCeiling);
    }
    // Lone peaks, which an 8x grid alone misses: short bursts at high frequencies, at 12 offsets.
    for (const double freq : {8000.0, 12000.0, 16000.0, 18000.0}) {
        double worstTruePeak = -100.0, worstStandard = -100.0, worstSample = -100.0;
        for (int offset = 0; offset < 12; ++offset) {
            constexpr int64_t kCentreAt = 600;
            Samples in(1200 + kL, 0.f);
            for (int64_t i = -12; i <= 12; ++i) {
                const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i + 12) / 24.0);
                const double t = static_cast<double>(i) - offset / 12.0;
                in[static_cast<size_t>(kCentreAt + i)] =
                    static_cast<float>(3.0 * hann * std::cos(2.0 * kPi * freq * t / kSampleRate));
            }
            Limiter tp({{"ceiling", kCeiling}, {"mode", 2.f}});
            const Samples a = tp.play(in);
            Limiter standard({{"ceiling", kCeiling}});
            const Samples b = standard.play(in);
            const int64_t from = kCentreAt + kL - 100, to = kCentreAt + kL + 100;
            worstTruePeak = std::max(worstTruePeak, truePeakDb(a, from, to));
            worstStandard = std::max(worstStandard, truePeakDb(b, from, to));
            worstSample = std::max({worstSample, db(peak(a)), db(peak(b))});
        }
        INFO(std::to_string(freq) + " Hz: True Peak " + std::to_string(worstTruePeak) + ", Standard " +
             std::to_string(worstStandard) + " dB at most");
        CHECK(worstTruePeak <= kCeiling + 0.01);
        CHECK(worstSample <= db(postOf({{"ceiling", kCeiling}})));
        if (freq >= 16000.0) CHECK(worstStandard > kCeiling + 0.05);
    }
}

TEST_CASE("the limiter's true-peak interpolator is flat and its parabola finds the peak") {
    const limiter::Phases& phases = limiter::truePeakPhases();
    for (size_t k = 0; k < phases.size(); ++k) {
        INFO("phase " + std::to_string(k + 1));
        double sum = 0.0;
        for (const float h : phases[k]) sum += h;
        CHECK_APPROX_TOL(sum, 1.0, 0.0, 1e-6);
        for (int step = 1; step <= 21; ++step) {
            const double f = 0.02 * step;  // of the rate: 0.02 .. 0.42
            std::complex<double> response = 0.0;
            for (int i = 0; i < limiter::kTaps; ++i)
                response += static_cast<double>(phases[k][static_cast<size_t>(i)]) *
                            std::polar(1.0, 2.0 * kPi * f * (i - 11));
            INFO(std::to_string(f));
            CHECK(std::abs(db(std::abs(response))) <= 0.01);
        }
    }
    // A sine through the window: the interpolated value is the sine's, between samples.
    std::vector<float> window(limiter::kTaps);
    for (int i = 0; i < limiter::kTaps; ++i)
        window[static_cast<size_t>(i)] = static_cast<float>(std::sin(0.7 * (i - 11) + 0.3));
    for (size_t k = 0; k < phases.size(); ++k)
        CHECK_APPROX_TOL(limiter::interpolate(window.data(), phases[k]), std::sin(0.7 * (k + 1) / 8.0 + 0.3), 0.0,
                         2e-3);

    // The parabola through three points of the 8x grid near a sine's crest at 0.4 of the rate.
    const double phi = 2.0 * kPi * 0.4 / 8.0;
    for (int i = -10; i <= 10; ++i) {
        const double delta = 0.5 * phi * i / 10.0;
        const auto ym = static_cast<float>(std::cos(delta - phi)), y0 = static_cast<float>(std::cos(delta)),
                   yp = static_cast<float>(std::cos(delta + phi));
        const float p = limiter::refinedPeak(ym, y0, yp);
        INFO(std::to_string(delta));
        CHECK_APPROX_TOL(p, 1.0, 0.0, 2.5e-4);  // (worst midway between two points: 0.99977, -0.002 dB)
        CHECK(p >= y0);
    }
    CHECK_EQ(limiter::refinedPeak(0.9f, 0.8f, 0.5f), 0.8f);  // a neighbour is larger: its own interval has the peak
    CHECK_EQ(limiter::refinedPeak(0.5f, 0.8f, 0.9f), 0.8f);
}

TEST_CASE("the limiter's Soft Clip rounds peaks off near the ceiling") {
    const float c = postOf({});
    const Samples atCeiling = tone(100.0, 1.5, c);
    const int64_t from = kL + kSampleRate / 4, length = kSampleRate;  // a second of 100 whole cycles
    for (const float mode : {0.f, 1.f}) {
        INFO("mode " + std::to_string(mode));
        Limiter l({{"mode", mode}});
        const auto [out, unused] = l.play(atCeiling, atCeiling);
        const double fundamental = levelAt(out, 100.0, from, length);
        const double third = levelAt(out, 300.0, from, length);
        CHECK(maxOfValues(l.display("gr_a")) < 0.001f);
        const std::vector<float> clip = l.display("clip");
        if (mode == 0.f) {
            CHECK_APPROX_TOL(peak(out), c, 0.0, 1e-6);
            CHECK(peak(out) <= c);
            CHECK(allEqual(clip, 0.0));
            CHECK(db(third / fundamental) < -100.0);
        } else {
            CHECK_APPROX_TOL(db(peak(out) / c), db(0.875), 0.0, 0.01);  // -1.16 dB
            CHECK_APPROX_TOL(maxOfValues(clip, 4), -db(0.875), 0.0, 0.05);
            CHECK_APPROX_TOL(db(third / fundamental), -27.0, 0.0, 2.0);
        }
    }
    // Peaks up to 3.52 dB over are rounded off to the ceiling, not turned down; beyond, limited.
    {
        const Samples over = tone(100.0, 0.5, c * std::pow(10.0, 3.52 / 20.0));
        Limiter l({{"mode", 1.f}});
        const auto [out, unused] = l.play(over, over);
        CHECK_APPROX_TOL(db(peak(out) / c), 0.0, 0.0, 0.01);
        CHECK(maxOfValues(l.display("gr_a")) < 0.01f);
    }
    for (const float mode : {0.f, 1.f}) {
        const Samples over = tone(100.0, 0.5, c * std::pow(10.0, 12.0 / 20.0));
        Limiter l({{"mode", mode}});
        const auto [out, unused] = l.play(over, over);
        CHECK(peak(out) <= c);
        CHECK_APPROX_TOL(maxOfValues(l.display("gr_a"), 20), mode == 0.f ? 12.0 : 8.48, 0.0, 0.2);
    }
    // Sample by sample, it is the knee's curve.
    Samples ramp(24000 + kL, 0.f);
    for (size_t i = 0; i < 24000; ++i) ramp[i] = static_cast<float>(1.5 * c * static_cast<double>(i) / 24000.0);
    Limiter l({{"mode", 1.f}});
    const Samples out = l.play(ramp);
    const limiter::Scales s = limiter::scales(false, 0.f, -0.3f, 0.f, -0.3f);
    Samples want(ramp.size(), 0.f);
    for (size_t i = 0; i + kL < ramp.size(); ++i)
        want[i + kL] = limiter::shape(ramp[i] * s.pre, limiter::knee(1.f)) * s.post;
    CHECK_ALLCLOSE(out, want, 0.0, 1e-5);
}

TEST_CASE("the limiter's Maximize turns the gain into Output - Threshold") {
    const Samples in = tone(1000.0, 0.5, 0.25);  // -12.04 dBFS
    {
        Limiter l({{"maximize", 1.f}, {"output", -1.f}, {"threshold", -12.f}});
        const auto [out, unused] = l.play(in, in);
        CHECK_APPROX_TOL(db(peak(out, kL + 4800)), -1.04, 0.0, 0.01);
        CHECK(allEqual(l.display("gr_a"), 0.0));
    }
    Limiter l({{"maximize", 1.f}, {"output", -1.f}, {"threshold", -18.f}});
    const auto [out, unused] = l.play(in, in);
    CHECK_APPROX_TOL(db(peak(out, kL + 4800)), -1.0, 0.0, 0.02);
    CHECK_APPROX_TOL(maxOfValues(l.display("gr_a"), 4), 5.96, 0.0, 0.1);
    // The same as Standard with 17 dB of gain and the ceiling at -1 dB.
    Limiter standard({{"gain", 17.f}, {"ceiling", -1.f}});
    const auto [same, unused2] = standard.play(in, in);
    CHECK_ALLCLOSE(out, same, 1e-5, 1e-9);
}

TEST_CASE("the limiter's L/R, M/S and Link") {
    const Samples loud = tone(1000.0, 0.5, 2.0), quiet = tone(500.0, 0.5, 0.25);
    const auto values = [](float link, float routing = 0.f) {
        return Values{{"ceiling", 0.f}, {"link", link}, {"routing", routing}};
    };
    {
        Limiter l(values(100.f));
        l.play(loud, quiet);
        CHECK_APPROX_TOL(maxOfValues(l.display("gr_a"), 4), 6.02, 0.0, 0.1);
        CHECK_APPROX_TOL(maxOfValues(l.display("gr_b"), 4), 6.02, 0.0, 0.1);
    }
    {
        Limiter l(values(0.f));
        const auto [left, right] = l.play(loud, quiet);
        CHECK_APPROX_TOL(maxOfValues(l.display("gr_a"), 4), 6.02, 0.0, 0.1);
        CHECK(allEqual(l.display("gr_b"), 0.0));
        CHECK_ALLCLOSE(right, delayed(quiet, kL), 1e-6, 1e-9);
    }
    {
        Limiter l(values(50.f));
        l.play(loud, quiet);
        CHECK_APPROX_TOL(maxOfValues(l.display("gr_b"), 4), -db(0.75), 0.0, 0.2);  // half the depth: 2.5 dB
    }
    // M/S, unlinked: a loud centre is limited and the side keeps its level.
    const Samples mid = tone(1000.0, 0.5, 1.6), side = tone(3000.0, 0.5, 0.1);
    Samples left(mid.size()), right(mid.size());
    for (size_t i = 0; i < mid.size(); ++i) {
        left[i] = mid[i] + side[i];
        right[i] = mid[i] - side[i];
    }
    {
        Limiter l(values(0.f, 1.f));
        const auto [a, b] = l.play(left, right);
        Samples sideOut(a.size()), midOut(a.size());
        for (size_t i = 0; i < a.size(); ++i) {
            sideOut[i] = 0.5f * (a[i] - b[i]);
            midOut[i] = 0.5f * (a[i] + b[i]);
        }
        CHECK_ALLCLOSE(sideOut, delayed(side, kL), 0.0, 1e-5);
        CHECK(peak(midOut, kL + 4800) < 0.95);
        CHECK(allEqual(l.display("gr_b"), 0.0));
        CHECK(maxOfValues(l.display("gr_a"), 4) > 3.f);
        CHECK(std::max(peak(a), peak(b)) <= 1.0);
    }
    // Linked, M/S is L/R linked: |left| and |right| at most |mid| + |side|, both turned down alike.
    for (const auto& [l1, r1] : {std::pair{left, right}, std::pair{loud, quiet}}) {
        Limiter ms(values(100.f, 1.f)), lr(values(100.f, 0.f));
        const auto [a, b] = ms.play(l1, r1);
        const auto [c, d] = lr.play(l1, r1);
        CHECK_ALLCLOSE(a, c, 1e-5, 1e-7);
        CHECK_ALLCLOSE(b, d, 1e-5, 1e-7);
    }
}

TEST_CASE("changing any of the limiter's controls is click-free") {
    // A 200 Hz tone 6 dB into the limiter (L/R differ where it matters), one change by
    // automation at frame 24000 each; its clickiness around it against the steady renders'.
    const Samples a = tone(200.0, 0.75, 2.0), b = tone(300.0, 0.75, 1.0, 0.0);
    struct Case {
        Values base;
        std::string id;
        float to;
        bool wide = false;  // left and right differ
    };
    const std::vector<Case> cases = {
        {{}, "gain", 6.f},
        {{}, "ceiling", -6.f},
        {{}, "link", 0.f, true},
        {{{"mode", 0.f}}, "mode", 1.f},
        {{{"mode", 1.f}}, "mode", 2.f},
        {{{"mode", 2.f}}, "mode", 0.f},
        {{}, "routing", 1.f, true},
        {{{"link", 50.f}}, "routing", 1.f, true},
        {{{"threshold", -6.f}, {"output", -1.f}}, "maximize", 1.f},
        {{}, "auto_release", 0.f},
        {{{"auto_release", 0.f}}, "release", 1.f},
        {{{"maximize", 1.f}, {"threshold", -6.f}, {"output", -1.f}}, "threshold", -12.f},
        {{{"maximize", 1.f}, {"threshold", -6.f}, {"output", -1.f}}, "output", -3.f},
    };
    constexpr int64_t kAt = 24000, kFrom = 22000, kTo = 30000;
    for (const Case& c : cases) {
        INFO(c.id + " to " + std::to_string(c.to) + (c.wide ? " (wide)" : ""));
        const Samples& right = c.wide ? b : a;
        Values after = c.base;
        after.emplace_back(c.id, c.to);
        Limiter before(c.base), steady(after), changing(c.base);
        const auto [b0, b1] = before.play(a, right);
        const auto [s0, s1] = steady.play(a, right);
        const auto [o0, o1] = changing.play(a, right, {{kAt, c.id, c.to}});
        const double bound = 3.0 * std::max({clickiness(b0, kFrom, kTo), clickiness(b1, kFrom, kTo),
                                             clickiness(s0, kFrom, kTo), clickiness(s1, kFrom, kTo)}) + 1e-3;
        const double measured = std::max(clickiness(o0, kFrom, kTo), clickiness(o1, kFrom, kTo));
        INFO("clickiness " + std::to_string(measured) + " against " + std::to_string(bound));
        CHECK(measured <= bound);
        CHECK(std::max(peak(o0), peak(o1)) <= std::max(postOf(c.base), postOf(after)));
        // The control: the steady render with its level stepped by 6 dB there.
        if (c.id == "gain") {
            Samples stepped = b0;
            for (size_t i = kAt; i < stepped.size(); ++i) stepped[i] *= 0.5f;
            CHECK(clickiness(stepped, kFrom, kTo) > 10.0 * bound);
        }
    }

    // Lookahead, between blocks: a dip of a few milliseconds, never a click or an overshoot.
    Limiter l;
    Samples left = a, right = a;
    constexpr int64_t kSwitch = 24064;  // (a block's start)
    Samples l1 = slice(left, 0, kSwitch), r1 = slice(right, 0, kSwitch);
    l.run({&l1, &r1});
    l.set("lookahead", 2.f);
    CHECK_EQ(l.processor().latencySamples(), 288);
    CHECK(l.processor().idle());
    CHECK(!l.processor().idle());
    Samples l2 = slice(left, kSwitch), r2 = slice(right, kSwitch);
    l.run({&l2, &r2});
    Samples out = l1;
    out.insert(out.end(), l2.begin(), l2.end());
    Limiter three, six({{"lookahead", 2.f}});
    const auto [steady3, unused3] = three.play(a, a);
    const auto [steady6, unused6] = six.play(a, a);
    const double bound = 3.0 * std::max(clickiness(steady3, kFrom, kTo), clickiness(steady6, kFrom, kTo)) + 1e-3;
    CHECK(clickiness(out, kFrom, kTo) <= bound);
    CHECK(peak(out) <= postOf({}));
    int64_t quiet = 0;
    for (int64_t i = kSwitch - 480; i < kSwitch + 960; ++i)
        quiet += std::abs(out[static_cast<size_t>(i)]) < 1e-3f ? 1 : 0;
    INFO("quiet for " + std::to_string(quiet) + " samples");
    CHECK(quiet <= 480);
    CHECK(quiet > 288);
    CHECK_ALLCLOSE(slice(out, kSwitch + 2400), slice(steady6, kSwitch + 2400), 1e-5, 1e-7);
}

TEST_CASE("the limiter's glides are straight in dB, and a moving ceiling leaves quiet material alone") {
    const Samples in = tone(1000.0, 0.75, 0.1);  // -20 dBFS
    {
        Limiter l;
        const auto [out, unused] = l.play(in, in, {{24000, "gain", 6.f}});
        // Each cycle's peak (48 samples) on the line from -20 to -14 dB over the 960 samples after it comes out.
        for (int64_t cycle = 0; cycle < 24; ++cycle) {
            const int64_t start = 24000 + kL + 48 * cycle;
            const size_t at = static_cast<size_t>(start) + argmax(slice(out, start, start + 48));
            const double t = std::clamp(static_cast<double>(static_cast<int64_t>(at) - 24000 - kL) / 960.0, 0.0, 1.0);
            INFO("cycle " + std::to_string(cycle));
            CHECK_APPROX_TOL(db(std::abs(out[at])), -20.0 + 6.0 * t, 0.0, 0.05);
        }
        CHECK_APPROX_TOL(db(peak(out, 24000 + kL + 960, 30000)), -20.0 + 6.0, 0.0, 0.01);
    }
    // The ceiling from -0.3 to -6 dB over a sine that reaches neither: nothing changes.
    Limiter l;
    const auto [out, unused] = l.play(in, in, {{24000, "ceiling", -6.f}});
    CHECK_ALLCLOSE(out, delayed(in, kL), 1e-5, 1e-9);
}

TEST_CASE("the limiter's automation plays through the engine sample-accurately") {
    sub::Engine engine;
    engine.setClipFadeMs(0);
    const Samples in = tone(1000.0, 2.0, 0.5);
    const std::string path = makeWav(stereo(in), 2);
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, 0.0, 2.0, 0.0, 1.f)});
    const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(track), "limiter", -1);
    setParam(engine, id, "ceiling", 0.f);
    const Samples untouched = channel(engine.renderOffline(0.0, 2 * kSampleRate), 0);

    // The ceiling at 0 dB until beat 2 (1 s), then -12 dB.
    const sub::ParamInfo ceiling = paramInfo(engine, id, "ceiling");
    using Points = std::vector<sub::AutomationPoint>;
    const float zero = ceiling.toNormalized(0.f), lower = ceiling.toNormalized(-12.f);
    engine.setTrackAutomation(track, {{id, "ceiling", Points{{0.0, zero, 0.f}, {2.0, zero, 0.f}, {2.0, lower, 0.f}}}});
    const Samples out = channel(engine.renderOffline(0.0, 2 * kSampleRate), 0);
    CHECK(allFinite(out));
    // (The engine plays the track its latency early, so what comes out is in time.) Untouched up to the step.
    CHECK_ARRAY_EQUAL(slice(out, 0, kSampleRate), slice(untouched, 0, kSampleRate));
    CHECK_APPROX_TOL(db(peak(out, 480, kSampleRate)), -6.02, 0.0, 0.01);
    const double after = db(peak(out, kSampleRate + 960 + kL));
    INFO(std::to_string(after));
    CHECK(after <= -12.0);
    CHECK(after >= -12.05);
}

TEST_CASE("reset and a new sample rate start the limiter cleanly") {
    const Samples loud = noise(kSampleRate / 2, 7, 1.f);
    Limiter l({{"gain", 12.f}});
    l.play(loud, loud);
    l.processor().reset();
    l.display("gr_a");
    l.display("gr_b");
    const auto [left, right] = l.play(Samples(kSampleRate / 4, 0.f), Samples(kSampleRate / 4, 0.f));
    CHECK(allEqual(left, 0.0));
    CHECK(allEqual(right, 0.0));
    CHECK(allEqual(l.display("gr_a"), 0.0));
    CHECK(allEqual(l.display("gr_b"), 0.0));

    // A new rate: the lookahead's length follows it, and the ceiling holds.
    for (const auto& [rate, lookahead, samples] : {std::tuple{96000.0, 1.f, 288}, std::tuple{44100.0, 1.f, 132},
                                                   std::tuple{44100.0, 0.f, 66}, std::tuple{44100.0, 2.f, 265}}) {
        INFO(std::to_string(rate) + " Hz, lookahead " + std::to_string(lookahead));
        l.set("lookahead", lookahead);
        l.prepare(rate);
        CHECK_EQ(l.processor().latencySamples(), samples);
        CHECK_EQ(l.processor().tailSamples(), samples);
        const auto [a, b] = l.play(noise(static_cast<size_t>(rate / 4), 1), noise(static_cast<size_t>(rate / 4), 2));
        CHECK(std::max(peak(a), peak(b)) <= postOf({{"gain", 12.f}}));
        // Below the ceiling, the input that many samples late.
        const Samples quiet = tone(440.0, 0.1, 0.1, 0.0, rate);
        l.processor().reset();
        const Samples out = l.play(quiet);
        CHECK_ALLCLOSE(out, scaled(delayed(quiet, samples), std::pow(10.0, 12.0 / 20.0)), 1e-5, 1e-8);
    }
}

TEST_CASE("the limiter stays finite and under the ceiling at the extremes") {
    {
        Limiter l({{"gain", 24.f}, {"ceiling", -24.f}, {"release", 0.1f}, {"auto_release", 0.f}, {"lookahead", 0.f}},
                  44100.0);
        const auto [a, b] = l.play(noise(44100, 1, 1.f), noise(44100, 2, 1.f));
        CHECK(allFinite(a) && allFinite(b));
        CHECK(std::max(peak(a), peak(b)) <= postOf({{"gain", 24.f}, {"ceiling", -24.f}}));
        CHECK(std::max(peak(a), peak(b)) > 0.99 * postOf({{"ceiling", -24.f}}));
    }
    {
        Limiter l({{"gain", 12.f}, {"lookahead", 2.f}}, 192000.0);
        CHECK_EQ(l.processor().latencySamples(), 1152);
        const auto [a, b] = l.play(noise(96000, 3, 1.f), spikes(96000, 4, 8.f));
        CHECK(std::max(peak(a), peak(b)) <= postOf({}));
    }
    {
        const Samples faint = tone(1000.0, 0.25, 0.001);  // -60 dBFS
        Limiter l({{"gain", -24.f}});
        const Samples out = l.play(faint);
        CHECK_ALLCLOSE(out, scaled(delayed(faint, kL), std::pow(10.0, -24.0 / 20.0)), 1e-5, 1e-12);
    }
    for (const float mode : {0.f, 1.f, 2.f}) {
        for (const float routing : {0.f, 1.f}) {
            Limiter l({{"mode", mode}, {"routing", routing}, {"link", 30.f}, {"gain", 24.f}});
            const auto [a, b] = l.play(noise(2 * kSampleRate, 5, 1.f), noise(2 * kSampleRate, 6, 1.f));
            CHECK(allFinite(a) && allFinite(b));
        }
    }
    // Input that isn't finite is taken as silence, and the limiter carries on.
    Samples in = tone(440.0, 0.25, 0.5);
    Samples want = delayed(in, kL);
    in[1000] = std::numeric_limits<float>::quiet_NaN();
    in[2000] = std::numeric_limits<float>::infinity();
    want[1000 + kL] = 0.f;
    want[2000 + kL] = 0.f;
    for (const float mode : {0.f, 2.f}) {
        INFO("mode " + std::to_string(mode));
        Limiter l({{"mode", mode}});
        const auto [a, b] = l.play(in, in);
        CHECK(allFinite(a) && allFinite(b));
        CHECK_EQ(a[1000 + kL], 0.f);
        CHECK_EQ(a[2000 + kL], 0.f);
        if (mode == 0.f) CHECK_ALLCLOSE(a, want, 1e-6, 1e-9);
        CHECK(peak(a, 3000 + kL) > 0.49);
    }
}

TEST_CASE("silence rings out of the limiter to exact zeros") {
    Samples in = noise(kSampleRate / 4, 8, 1.f);
    const int64_t last = static_cast<int64_t>(in.size()) - 1;
    in.resize(kSampleRate, 0.f);
    Limiter l({{"gain", 12.f}, {"auto_release", 0.f}, {"release", 10.f}});
    const auto [a, b] = l.play(in, in);
    CHECK(peak(a, last + kL - 10, last + kL + 1) > 0.0);
    CHECK(allEqual(slice(a, last + kL + 1), 0.0));
    CHECK(allEqual(slice(b, last + kL + 1), 0.0));
    for (const char* id : {"gr_a", "gr_b"}) {
        INFO(id);
        const std::vector<float> gr = l.display(id);
        const auto from = static_cast<size_t>((last + kL + kSampleRate * 3 / 10) / 128 + 1);
        CHECK(gr.size() > from);
        CHECK(allEqual(std::vector<float>(gr.begin() + static_cast<int64_t>(from), gr.end()), 0.0));
        CHECK(gr[static_cast<size_t>((last + kL) / 128)] > 1.f);
    }
}

TEST_CASE("the limiter on one channel: the ceiling holds, and Routing and Link change nothing") {
    for (const Input& input : brickWallInputs()) {
        for (const float mode : {0.f, 1.f, 2.f}) {
            INFO(input.name + ", mode " + std::to_string(mode));
            const Values values = {{"mode", mode}, {"gain", input.gain}};
            Limiter l(values);
            const Samples out = l.play(input.left);
            CHECK(allFinite(out));
            CHECK(peak(out) <= postOf(values));
        }
    }
    const Samples in = noise(kSampleRate / 4, 9, 1.f);
    Limiter plain({{"gain", 12.f}}), other({{"gain", 12.f}, {"routing", 1.f}, {"link", 0.f}});
    const Samples a = plain.play(in);
    CHECK_ARRAY_EQUAL(other.play(in), a);
    CHECK(plain.display("in_r") == plain.display("in_l"));
    CHECK(plain.display("out_r") == plain.display("out_l"));
    const std::vector<float> grA = plain.display("gr_a"), grB = plain.display("gr_b");
    CHECK(grA == grB);
    CHECK(maxOfValues(grA) > 6.f);
}

TEST_CASE("the engine delays the other tracks by the limiter's lookahead") {
    for (const auto& [lookahead, latency] : {std::pair{1.f, 144}, std::pair{2.f, 288}}) {
        INFO(std::to_string(latency));
        sub::Engine engine;
        engine.setClipFadeMs(0);
        constexpr int64_t kClick = 1000;
        stereoClickTrack(engine, 0.5f, 0.f, kClick, 1.0);
        const uint32_t wet = stereoClickTrack(engine, 0.f, 0.5f, kClick, 1.0);
        const uint32_t id = engine.addBuiltinProcessor(engine.trackChain(wet), "limiter", -1);
        setParam(engine, id, "lookahead", lookahead);
        engine.idle();
        CHECK_EQ(engine.processorInfo(id).latency, latency);
        const Samples out = engine.renderOffline(0.0, kSampleRate / 2);
        const std::vector<int64_t> left = nonzero(channel(out, 0)), right = nonzero(channel(out, 1));
        REQUIRE(left.size() == 1);
        REQUIRE(right.size() == 1);
        CHECK_EQ(right[0], left[0]);
        CHECK_EQ(left[0], kClick);  // (the engine plays the limited track its latency early)
        CHECK_APPROX_REL(at(out, right[0], 1), at(out, left[0], 0), 1e-6);
    }
}

TEST_CASE("the limiter's displays") {
    const Samples loud = tone(1000.0, 128.0 * 100 / kSampleRate, 2.0);
    {
        Limiter l({{"ceiling", 0.f}});
        l.play(loud, loud);
        for (const char* id : {"in_l", "in_r", "out_l", "out_r", "gr_a", "gr_b", "clip"}) {
            INFO(id);
            CHECK_EQ(l.display(id).size(), size_t{100});
        }
    }
    Limiter l({{"ceiling", 0.f}});
    l.play(loud, loud);
    const std::vector<float> in = l.display("in_l"), out = l.display("out_l"), gr = l.display("gr_a"),
                             clip = l.display("clip");
    CHECK_EQ(in[0], limiter::kFloorDb);  // (the lookahead's silence)
    for (size_t i = 4; i < 100; ++i) {
        INFO(std::to_string(i));
        CHECK_APPROX_TOL(in[i], 6.02, 0.0, 0.05);
        CHECK_APPROX_TOL(out[i], 0.0, 0.0, 0.05);
        CHECK(out[i] <= 0.f);
        CHECK_APPROX_TOL(gr[i], 6.02, 0.0, 0.1);
    }
    CHECK(allEqual(clip, 0.0));
    // In the line's domain: after Gain without Maximize, the input as it is with it.
    {
        Limiter g({{"ceiling", 0.f}, {"gain", 6.f}});
        g.play(loud, loud);
        CHECK_APPROX_TOL(g.display("in_l")[50], 12.04, 0.0, 0.05);
        Limiter m({{"maximize", 1.f}, {"threshold", -6.f}, {"output", -1.f}});
        m.play(loud, loud);
        CHECK_APPROX_TOL(m.display("in_l")[50], 6.02, 0.0, 0.05);
        CHECK_APPROX_TOL(m.display("out_l")[50], -1.0, 0.0, 0.05);
        CHECK_APPROX_TOL(m.display("gr_a")[50], 12.02, 0.0, 0.1);
    }
    // M/S on a mono signal, unlinked: the reduction is all the mid's.
    {
        Limiter ms({{"ceiling", 0.f}, {"routing", 1.f}, {"link", 0.f}});
        ms.play(loud, loud);
        CHECK_APPROX_TOL(ms.display("gr_a")[50], 6.02, 0.0, 0.1);
        CHECK(allEqual(ms.display("gr_b"), 0.0));
    }
    // Reading on: only what is new; falling behind skips to the latest 8192 values.
    l.play(loud, loud);
    CHECK_EQ(l.display("gr_a").size(), size_t{100});
    CHECK(l.display("gr_a").empty());
    const Samples longer = tone(1000.0, 128.0 * 9000 / kSampleRate, 0.5);
    l.play(longer, longer);
    CHECK_EQ(l.display("gr_a").size(), size_t{8192});
}
