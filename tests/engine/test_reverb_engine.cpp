// The built-in Reverb, after Live's: mono in, stereo out. Its tail decays per
// band as the design says (the curve its editor draws), the reflections land
// where earlyTaps() puts them, Shape moves the diffuse onset, the input filter
// is the band it draws; Stereo, one channel, the levels; Freeze, Cut and Flat;
// the guard; each Density; no metallic ringing; every control and switch
// changing without a click; automation to the sample; reset and a new rate;
// extremes; silence ringing out to exact zeros and waking; its tail; its
// displays; what it costs.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "Engine.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/Dsp.h"
#include "builtin/ReverbDesign.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"
#include "rt/RtUtils.h"

using namespace subtest;
namespace reverb = sub::reverb;

namespace {

constexpr int kBlock = 1024;  // the renderer's largest block (Renderer::kMaxBlock)

using Values = std::vector<std::pair<std::string, float>>;

// A parameter's change at a frame, as automation hands it over.
struct Change {
    int64_t frame;
    std::string id;
    float value;
};

// What most cases start from: nothing moves by itself (Spin and Chorus off),
// the input unfiltered, the reflections at their least (-30 dB), all wet.
const Values kQuiet = {{"spin", 0.f}, {"chorus", 0.f}, {"lo_cut", 0.f}, {"hi_cut", 0.f}, {"reflect", -30.f},
                       {"mix", 100.f}};

// `values` with `more` set on top (a value set twice: the later).
Values with(Values values, const Values& more) {
    for (const auto& [id, value] : more) {
        auto at = std::find_if(values.begin(), values.end(), [&](const auto& v) { return v.first == id; });
        if (at != values.end()) {
            at->second = value;
        } else {
            values.emplace_back(id, value);
        }
    }
    return values;
}

int64_t frames(double seconds, double rate = kSampleRate) { return static_cast<int64_t>(std::lround(seconds * rate)); }

// A Reverb on its own, outside an engine: processed in blocks (the renderer's
// flush-to-zero on, as it renders), its changes handed over as automation.
class Reverb {
public:
    explicit Reverb(const Values& values = {}, double rate = kSampleRate)
        : processor_(sub::BuiltinRegistry::instance().create("reverb")), rate_(rate) {
        for (const auto& [id, value] : values) set(id, value);
        processor_->prepare(rate, kBlock);
    }

    sub::Processor& processor() { return *processor_; }
    double rate() const { return rate_; }

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
        const sub::ScopedNoDenormals noDenormals;
        const auto length = static_cast<int64_t>(channels[0]->size());
        sub::ProcessContext ctx;
        ctx.sampleRate = rate_;
        ctx.offline = true;
        float* pointers[2] = {};
        size_t next = 0;
        for (int64_t start = 0; start < length; start += block) {
            const int n = static_cast<int>(std::min<int64_t>(block, length - start));
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
    // Two channels: what comes out of each.
    std::pair<Samples, Samples> play(Samples left, Samples right, const std::vector<Change>& changes = {},
                                     int block = 256) {
        run({&left, &right}, changes, block);
        return {std::move(left), std::move(right)};
    }
    // A display's values published since the last call.
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
    std::map<size_t, uint64_t> positions_;
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

// A sine that fades in over 100 ms (no click of its own to start with).
Samples smoothSine(double freq, double seconds, double rate = kSampleRate, double amplitude = 0.5) {
    Samples x(static_cast<size_t>(seconds * rate));
    const double fadeIn = 0.1 * rate;
    for (size_t i = 0; i < x.size(); ++i) {
        const double t = std::min(1.0, static_cast<double>(i) / fadeIn);
        x[i] = static_cast<float>(t * t * (3.0 - 2.0 * t) * amplitude * std::sin(2.0 * kPi * freq * i / rate));
    }
    return x;
}

Samples silence(Samples x, double seconds, double rate = kSampleRate) {
    x.resize(x.size() + static_cast<size_t>(seconds * rate), 0.f);
    return x;
}

double energy(const Samples& x) {
    double sum = 0.0;
    for (const float v : x) sum += static_cast<double>(v) * v;
    return sum;
}
double db(double ratio) { return 10.0 * std::log10(std::max(ratio, 1e-300)); }
double rmsDb(const Samples& x) { return 20.0 * std::log10(std::max(rms(x), 1e-300)); }

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
        const double level = db(curve[i] / curve[0]);
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

std::string show(const Values& values) {
    std::string text;
    for (const auto& [id, value] : values) text += (text.empty() ? "" : ", ") + id + " " + std::to_string(value);
    return text;
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
        {"stereo", "Stereo Image", "%", 0.f, 120.f, 100.f, false, {}},
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
                INFO(show(values) + " at " + std::to_string(band) + " Hz: " + std::to_string(got) + " s, designed " +
                     std::to_string(want));
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
        // The first sound: the first tap, after the predelay (within the interpolation's reach).
        const std::vector<int64_t> heard = above(a, 1e-4);
        REQUIRE(!heard.empty());
        CHECK_NEAR(static_cast<double>(heard.front()), predelay + 3.1 * s * kSampleRate / 1000.0, 2.0);
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
        CHECK_NEAR(late - early, 0.8 * 56.7 * s, 2.0);
    }
    // Shape high: the reflections fade faster (their last third against their first).
    const auto fall = [](float shape) {
        const Values values = with(kQuiet, {{"shape", shape}, {"reflect", 0.f}, {"diffuse", -30.f}});
        Reverb r(values);
        const Samples h = r.play(impulse(static_cast<size_t>(0.1 * kSampleRate)));
        const double predelay = 2.5, first = 3.1, last = 56.7, third = (last - first) / 3.0;
        const auto at = [&](double ms) { return frames((predelay + ms) / 1000.0); };
        return db(energy(slice(h, at(last - third), at(last + 0.5))) /
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

TEST_CASE("the reverb's stereo image goes from mono to wider than its own") {
    const auto render = [](float stereo) {
        Reverb r(with(kQuiet, {{"stereo", stereo}}));
        const size_t length = static_cast<size_t>(1.5 * kSampleRate);
        return r.play(impulse(length), impulse(length));
    };
    const auto [monoL, monoR] = render(0.f);
    CHECK_ARRAY_EQUAL(monoL, monoR);
    const auto late = [](const Samples& x) { return slice(x, frames(0.2), frames(1.2)); };
    const auto [l, r] = render(100.f);
    const Samples lateL = late(l), lateR = late(r);
    const double rho = correlation(std::vector<double>(lateL.begin(), lateL.end()),
                                   std::vector<double>(lateR.begin(), lateR.end()));
    INFO("correlation " + std::to_string(rho));
    CHECK(std::abs(rho) < 0.3);
    const auto sideOverMid = [&](const Samples& a, const Samples& b) {
        double side = 0.0, mid = 0.0;
        const Samples la = late(a), lb = late(b);
        for (size_t i = 0; i < la.size(); ++i) {
            mid += 0.25 * (la[i] + lb[i]) * (la[i] + lb[i]);
            side += 0.25 * (la[i] - lb[i]) * (la[i] - lb[i]);
        }
        return side / mid;
    };
    const auto [wl, wr] = render(120.f);
    CHECK_APPROX_TOL(sideOverMid(wl, wr) / sideOverMid(l, r), 1.44, 0.05, 0.0);
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
    CHECK_NEAR(db(window(-30.f, 0.f, 0.0025, 0.0225) / window(0.f, 0.f, 0.0025, 0.0225)), -30.0, 0.5);
    CHECK_NEAR(db(window(-30.f, -30.f, 0.5, 1.5) / window(-30.f, 0.f, 0.5, 1.5)), -30.0, 0.5);

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
    // Noise, then frozen (Cut and Flat, by automation) while the noise goes on:
    // the tail holds.
    const Samples x = noise(static_cast<size_t>(7 * kSampleRate), 6);
    const int64_t at = frames(0.5);
    {
        Reverb r(kQuiet);
        const Samples out = r.play(x, {{at, "freeze", 1.f}});
        const double early = rmsDb(slice(out, at + frames(1.0), at + frames(2.0)));
        const double late = rmsDb(slice(out, at + frames(5.0), at + frames(6.0)));
        INFO(std::to_string(early) + " dB, then " + std::to_string(late) + " dB");
        CHECK_NEAR(late, early, 1.5);
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
        return db(band(4.0, 4.5) / band(1.0, 1.5));
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
        INFO(show(values));
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
    // Gaussian's from 150 ms on, and early on the richer networks are denser.
    const Samples high = impulseResponse(kQuiet, 1.0).left;
    for (const double at : {0.15, 0.2, 0.3, 0.4, 0.6}) {
        INFO("at " + std::to_string(at) + " s: " + std::to_string(echoDensity(high, at)));
        CHECK(echoDensity(high, at) >= 0.85);
    }
    const Samples sparse = impulseResponse(with(kQuiet, {{"density", 0.f}}), 1.0).left;
    INFO("at 60 ms: High " + std::to_string(echoDensity(high, 0.06)) + ", Sparse " +
         std::to_string(echoDensity(sparse, 0.06)));
    CHECK(echoDensity(high, 0.06) > echoDensity(sparse, 0.06));
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
                worst = std::max(worst, db(power[k] / (sum / static_cast<double>(to - from + 1))));
            }
            INFO(show(values) + ": a bin " + std::to_string(worst) + " dB over its third-octave");
            CHECK(worst < 14.0);
        }
    }
}

TEST_CASE("the reverb changes every control without a click") {
    // A 220 Hz tone through the defaults (Spin and Chorus on), every control and
    // switch jumping in turn as automation's steps make them: the largest 6th
    // difference stays within three times the steady render's, or 1e-4 (a step of
    // 5e-6, -106 dB) where that is more. Size at Smooth's Fast is the one control
    // allowed more: a jump from 100 to 300 sweeps the whole tail's pitch by 15 %
    // within 0.1 s (every delay growing at up to 0.15 samples a sample), and the
    // measure then sees what is left of the lines' reads' error in the sweep, -85
    // dB under the tone (a step of 5e-5, -86 dB, would score as much), not a click.
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

    // A new rate: tuned to it, from silence.
    r.play(noise(kSampleRate, 10));
    r.processor().prepare(96000.0, kBlock);
    Reverb at96(values, 96000.0);
    // (the helper's rate is the one it was made at: render the new rate by hand)
    Samples h(96000, 0.f), h96(96000, 0.f);
    h[0] = h96[0] = 1.f;
    {
        const sub::ScopedNoDenormals noDenormals;
        sub::ProcessContext ctx;
        ctx.sampleRate = 96000.0;
        for (Samples* x : {&h, &h96}) {
            sub::Processor& p = x == &h ? r.processor() : at96.processor();
            for (size_t start = 0; start < x->size(); start += 256) {
                float* channels[1] = {x->data() + start};
                p.process(ctx, channels, 1, static_cast<int>(std::min<size_t>(256, x->size() - start)));
            }
        }
    }
    CHECK_ARRAY_EQUAL(h, h96);
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
        INFO(show(values));
        Reverb r(with(values, {{"mix", 100.f}}));
        const auto [l, rr] = r.play(x, x);
        REQUIRE(allFinite(l) && allFinite(rr));
        CHECK(std::max(maxAbs(l), maxAbs(rr)) < 20.0);
        const double first = energy(slice(l, frames(2.0), frames(3.0))), last = energy(slice(l, frames(5.0)));
        CHECK(last < first);
    }
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
        INFO(show(values));
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
    // (in pieces: a reader keeps up with the latest 8192 values of a display)
    std::vector<float> signal, tail;
    for (int64_t at = 0; at < static_cast<int64_t>(tone.size()); at += 4800) {
        const Samples piece = slice(tone, at, at + 4800);
        r.play(piece, piece);
        const std::vector<float> s = r.display("signal"), t = r.display("tail");
        signal.insert(signal.end(), s.begin(), s.end());
        tail.insert(tail.end(), t.begin(), t.end());
    }
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
    const auto length = static_cast<size_t>(10 * kSampleRate);
    const Samples l = noise(length, 18), r = noise(length, 19);
    Reverb reverb;
    const auto start = std::chrono::steady_clock::now();
    reverb.play(l, r, {}, 256);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    INFO("10 s took " + std::to_string(seconds) + " s");
    CHECK(seconds < 1.0);
}
#endif
