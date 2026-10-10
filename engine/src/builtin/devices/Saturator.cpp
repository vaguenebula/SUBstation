// Built-in "Saturator" device, after Ableton Live 12's Saturator: waveshaping,
// from a gentle rounding of the peaks to clipping and folding. Drive sets how
// far up the curve the input reaches; Type picks the curve
// (builtin/SaturatorDesign.h: Analog Clip, Soft Sine, Bass Shaper with its
// Threshold, Medium Curve, Hard Curve, Sinoid Fold, Digital Clip, and the
// Waveshaper with six controls of its own). Color puts an EQ before the curve
// and its exact inverse after it, so it changes which frequencies saturate
// without colouring a clean sound. Dry/Wet blends the result with the input,
// Post Clip (soft or hard) holds that blend to full scale, and Output turns it
// down. DC takes an offset out of the input; Hi-Quality runs everything
// between the input and Output at 4x the rate, which keeps the harmonics the
// curve and Post Clip make from folding back below Nyquist.
//
// Per channel: x (the input, through the DC filter as DC says) is the dry
// signal; u = Color's emphasis of x times Drive; the curve; Color's
// de-emphasis; then Output × PostClip((1 - mix) x + mix wet). That runs at the
// base rate, or (Hi-Quality) on x brought up to 4x, and brought back down
// after Post Clip.
//
// - Continuous controls glide through two one-poles in a row (10 ms each;
//   Color's 20 ms), so a jump eases in and out. They move a chunk of up to 16
//   samples at a time, in closed form (exact at each chunk's end however
//   automation splits the block), and the gains and the curve's settings ramp
//   linearly across the chunk. While Color glides it is designed anew at each
//   chunk's end and its coefficients move there in a straight line, base
//   sample by base sample; the de-emphasis follows the same line and rescales
//   its states as it goes, so emphasis and de-emphasis still cancel exactly on
//   a clean signal (see ColorFilters::deemphasisAt()).
// - Type, Post Clip, DC and Hi-Quality crossfade over 10 ms (an S-curve)
//   between both sides. Hi-Quality's 4x path first runs unheard for 80 samples
//   (its filters' memory), so what fades in is what it would have played had it
//   run all along (but for Color's filters at that rate, which start from
//   silence: on a clean sound they still cancel exactly). A change during a
//   fade starts when it is done.
// - Color fades by its gains: switched off, Base and Depth glide to 0 dB (where
//   the sections are exactly 1), and once their states have died away the
//   filters are switched out (also when Color is on at 0 dB: it costs nothing).
//   Each rate has its own filters, designed for it; only the paths that play
//   run theirs.
// - Hi-Quality delays everything by the 4x path's filters (36 samples):
//   latencySamples() says so, and idle() asks the engine to realign the tracks
//   when it changes. Post Clip's ceiling then holds at 4x; brought back down
//   (band-limited), bright clipped sound can pass it a little (a hard-clipped
//   5 kHz tone by 15 %), as the clipping curves' own flat tops do with
//   Hi-Quality. That is the price of not folding the clip's harmonics back.
// - Displays: the peaks of the input (after DC) and of the output every 128
//   samples (linear, the louder channel), and both summed to mono per sample.

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/DspBlocks.h"
#include "builtin/SaturatorDesign.h"
#include "rt/RtUtils.h"

namespace sub {
namespace {

using saturator::Clip;
using saturator::Type;

constexpr int kChannels = 2;
constexpr int kChunk = 16;                         // samples a glide moves at once
constexpr int kHqFactorLog2 = 2;                   // Hi-Quality: 4x
constexpr int kHqChunk = kChunk << kHqFactorLog2;  // its samples in a chunk
constexpr int kHqPreroll = 80;                     // base samples the 4x path runs unheard first (its filters hold 74)
constexpr int kMeterSamples = 128;                 // samples per peak published
constexpr double kGlideSeconds = 0.010;            // each of a glide's two one-poles
constexpr double kColorGlideSeconds = 0.020;       // Color's
constexpr double kFadeSeconds = 0.010;             // the lists' and switches' crossfades
constexpr double kLandedDb = 1e-4;                 // a glide lands when this close (dB, %)
constexpr double kLandedFraction = 1e-6;           // (fractions, log Hz)
constexpr double kQuietState = 1e-6;               // Color's states this small (-120 dB): switched out
constexpr double kDcCutoffHz = 5.0;
constexpr double kTailTimeConstants = 8.0;  // the tail: until the slowest filter is this many time constants on
constexpr double kMaxTailSeconds = 5.0;
constexpr int kWsControls = 6;

// A one-pole's coefficient raised to each chunk length, and n (1 - c): what a
// glide needs to move n samples at once.
struct GlideTable {
    std::array<double, kChunk + 1> power{}, linear{};

    void prepare(double seconds, double sampleRate) noexcept {
        const double c = onePoleCoefficient(seconds, sampleRate);
        for (int n = 0; n <= kChunk; ++n) {
            power[static_cast<size_t>(n)] = std::pow(c, n);
            linear[static_cast<size_t>(n)] = n * (1.0 - c);
        }
    }
};

// Two one-poles in a row gliding to a target (the first follows the target, the
// value the first), moved a chunk at a time: after n samples the errors are
// e1 c^n and c^n (e2 + n (1 - c) e1), what n steps of
// first += (1 - c)(target - first), value += (1 - c)(first - value) give. It
// lands on the target exactly once both are within `landed` of it.
struct Glide {
    double first = 0.0, value = 0.0;

    void snap(double target) noexcept { first = value = target; }
    bool settled(double target) const noexcept { return value == target && first == target; }
    // Moves `n` samples on; false if it was already there.
    bool advance(double target, const GlideTable& table, int n, double landed) noexcept {
        if (settled(target)) return false;
        const double e1 = first - target, e2 = value - target;
        const auto at = static_cast<size_t>(n);
        value = target + table.power[at] * (e2 + table.linear[at] * e1);
        first = target + table.power[at] * e1;
        if (std::abs(first - target) < landed && std::abs(value - target) < landed) snap(target);
        return true;
    }
};

// A list's or a switch's value, crossfading from one to the next: the weight of
// `to` follows an S-curve over the fade, after an optional pre-roll (at 0).
struct Switch {
    int from = 0, to = 0;
    int at = 0;       // samples into the fade
    int preroll = 0;  // samples before it starts

    bool fading() const noexcept { return from != to; }
    void snap(int value) noexcept {
        from = to = value;
        at = preroll = 0;
    }
    // A change starts a fade, unless one is running (then it waits for it).
    void request(int value, int prerollSamples = 0) noexcept {
        if (fading() || value == to) return;
        to = value;
        at = 0;
        preroll = prerollSamples;
    }
    // The weight of `to` for each of the next `n` samples (1 once the fade is done), moving on.
    void weights(float* w, int n, int length) noexcept {
        for (int i = 0; i < n; ++i) {
            if (!fading()) {
                w[i] = 1.f;
            } else if (preroll > 0) {
                --preroll;
                w[i] = 0.f;
            } else {
                w[i] = saturator::sCurve(static_cast<float>(at + 1) / static_cast<float>(length));
                if (++at >= length) snap(to);
            }
        }
    }
    // The weight of value 1 (a switch's "on") now.
    float onWeight(int length) const noexcept {
        if (!fading()) return static_cast<float>(to);
        const float w = preroll > 0 ? 0.f : saturator::sCurve(static_cast<float>(at) / static_cast<float>(length));
        return to == 1 ? w : 1.f - w;
    }
};

// A biquad section (transposed direct form II, states in double) whose two
// states are cleared together once both are tiny. dsp::Biquad clears each on
// its own, and a slowly decaying section (a low Color frequency, or any at 4x)
// can then ring at about 1e-19 for ever: what clearing one state takes out of
// the ringing, its partner's leftover puts back. Cleared together they reach
// exact zeros (at worst after 19 s: Depth ±36 dB at 30 Hz, Width 0).
struct Section {
    static constexpr double kTiny = 1e-20;
    double s1 = 0.0, s2 = 0.0;

    void reset() noexcept { s1 = s2 = 0.0; }
    float process(const dsp::BiquadCoefficients& c, float in) noexcept {
        const double x = in;
        const double y = c.b0 * x + s1;
        s1 = c.b1 * x - c.a1 * y + s2;
        s2 = c.b2 * x - c.a2 * y;
        if (std::abs(s1) < kTiny && std::abs(s2) < kTiny) s1 = s2 = 0.0;
        return static_cast<float>(y);
    }
};

// Color's filters at one rate (the base rate's, or the 4x path's): the
// emphasis before the curve, its exact inverse after it, and the straight line
// their coefficients follow across a chunk while Color glides (from the last
// chunk's design to this one's, landing on it). The de-emphasis takes the same
// line in step, at the same samples: the curve between them has no memory.
struct ColorFilters {
    double rate = 48000.0;
    saturator::ColorDesign from, to;    // the chunk's line
    saturator::ColorDesign emphasis;    // the emphasis's sections now
    saturator::ColorDesign deemphasis;  // what the de-emphasis inverts now
    dsp::BiquadCoefficients invShelf, invPeak;
    Section preShelf[kChannels], prePeak[kChannels], postPeak[kChannels], postShelf[kChannels];

    // Both sides at `design`, in step, nothing moving; the states as they are.
    void start(const saturator::ColorDesign& design) noexcept {
        from = to = emphasis = deemphasis = design;
        invShelf = saturator::inverse(design.shelf);
        invPeak = saturator::inverse(design.peak);
    }
    // A new line, from where the last one ended to `design` (the sections that move).
    void moveTo(const saturator::ColorDesign& design, bool shelf, bool peak) noexcept {
        from = to;
        if (shelf) to.shelf = design.shelf;
        if (peak) to.peak = design.peak;
    }
    // The emphasis at sample `k` of `length` along the line.
    void emphasisAt(int k, int length, bool shelf, bool peak) noexcept {
        if (shelf) emphasis.shelf = saturator::between(from.shelf, to.shelf, k, length);
        if (peak) emphasis.peak = saturator::between(from.peak, to.peak, k, length);
    }
    // The de-emphasis at sample `k`: the inverse of the emphasis there. A
    // section H and its inverse fed H's output (transposed direct form II) give
    // back H's input exactly while the inverse's states are -1/b0 times H's: a
    // new b0 breaks that, and a clean signal would pick up an error at every
    // change (a zipper while Color glides). Rescaling the inverse's states by
    // b0 old / b0 new keeps it.
    void deemphasisAt(int k, int length, bool shelf, bool peak) noexcept {
        if (shelf) {
            const dsp::BiquadCoefficients section = saturator::between(from.shelf, to.shelf, k, length);
            rescale(postShelf, deemphasis.shelf.b0 / section.b0);
            invShelf = saturator::inverse(section);
            deemphasis.shelf = section;
        }
        if (peak) {
            const dsp::BiquadCoefficients section = saturator::between(from.peak, to.peak, k, length);
            rescale(postPeak, deemphasis.peak.b0 / section.b0);
            invPeak = saturator::inverse(section);
            deemphasis.peak = section;
        }
    }
    float emphasize(int c, float x) noexcept {
        return prePeak[c].process(emphasis.peak, preShelf[c].process(emphasis.shelf, x));
    }
    float deemphasize(int c, float y) noexcept {
        return postShelf[c].process(invShelf, postPeak[c].process(invPeak, y));
    }

    void clear(int c) noexcept {
        preShelf[c].reset();
        prePeak[c].reset();
        postPeak[c].reset();
        postShelf[c].reset();
    }
    void clear() noexcept {
        for (int c = 0; c < kChannels; ++c) clear(c);
    }
    // Every state of the first `n` channels below `limit`.
    bool quiet(int n, double limit) const noexcept {
        const auto small = [limit](const Section& s) { return std::abs(s.s1) < limit && std::abs(s.s2) < limit; };
        for (int c = 0; c < n; ++c) {
            if (!small(preShelf[c]) || !small(prePeak[c]) || !small(postPeak[c]) || !small(postShelf[c])) return false;
        }
        return true;
    }

private:
    static void rescale(Section (&sections)[kChannels], double ratio) noexcept {
        if (ratio == 1.0) return;
        for (Section& s : sections) {
            s.s1 *= ratio;
            s.s2 *= ratio;
        }
    }
};

// Post Clip at base sample `i` (in the 4x path, for each of its four), both
// sides crossfading while it fades.
struct ClipFade {
    bool fading = false;
    Clip from = Clip::None, to = Clip::None;
    const float* weight = nullptr;

    float clip(int i, float y) const noexcept {
        if (!fading) return saturator::postClip(to, y);
        return (1.f - weight[i]) * saturator::postClip(from, y) + weight[i] * saturator::postClip(to, y);
    }
};

// The curve over a run of samples in place, its type fixed (so the compiler
// drops the switch), the settings for sample j at params[(j >> shift) * stride].
template <Type T>
void shapeRun(float* data, int count, const saturator::Params* params, int stride, int shift) noexcept {
    for (int j = 0; j < count; ++j) data[j] = saturator::curve(T, params[(j >> shift) * stride], data[j]);
}
using ShapeRun = void (*)(float*, int, const saturator::Params*, int, int);
constexpr std::array<ShapeRun, saturator::kTypes> kShapeRuns = {
    shapeRun<Type::AnalogClip>, shapeRun<Type::SoftSine>,   shapeRun<Type::BassShaper>,  shapeRun<Type::MediumCurve>,
    shapeRun<Type::HardCurve>,  shapeRun<Type::SinoidFold>, shapeRun<Type::DigitalClip>, shapeRun<Type::Waveshaper>,
};

class SaturatorProcessor final : public BuiltinProcessor {
public:
    enum Param {
        Drive = 0,
        TypeParam,
        Threshold,
        Output,
        Mix,
        ClipParam,
        Color,
        Base,
        Freq,
        Width,
        Depth,
        Dc,
        Hq,
        WsDrive,
        WsLin,
        WsCurve,
        WsDamp,
        WsDepth,
        WsPeriod,
        NumParams
    };
    enum Display { InPeak = 0, OutPeak, Input, OutputDisplay };

    SaturatorProcessor()
        : BuiltinProcessor(infos(),
                           {{"in_peak", kMeterSamples}, {"out_peak", kMeterSamples}, {"input", 1}, {"output", 1}}),
          hqLatency_(dsp::Oversampler::latencyFor(kHqFactorLog2)) {}

    std::string typeId() const override { return "builtin:saturator"; }
    std::string name() const override { return "Saturator"; }

    // Hi-Quality's filters delay everything (as the parameter is: a change fades in).
    int latencySamples() const override { return isOn(Hq) ? hqLatency_ : 0; }

    // Hi-Quality changed: the engine must realign the tracks.
    bool idle() override {
        const int latency = latencySamples();
        if (latency == reportedLatency_) return false;
        reportedLatency_ = latency;
        return true;
    }

    // What the curve leaves ringing in Color's filters (it doesn't cancel once
    // the curve bends) or the DC filter: 8 time constants of the slowest (69 dB
    // down); with Hi-Quality, its filters' memory too. At most 5 s.
    int tailSamples() const override {
        double seconds = 0.0;
        if (isOn(Color))
            seconds = kTailTimeConstants *
                      saturator::colorTimeConstant(param(Base), param(Freq), param(Width), param(Depth), sampleRate_);
        if (isOn(Dc)) seconds = std::max(seconds, kTailTimeConstants / (2.0 * dsp::kPi * kDcCutoffHz));
        const double samples = (isOn(Hq) ? kHqPreroll : 0) + std::ceil(seconds * sampleRate_);
        return static_cast<int>(std::min(samples, kMaxTailSeconds * sampleRate_));
    }

    void prepare(double sampleRate, int) override {
        sampleRate_ = sampleRate;
        fast_.prepare(kGlideSeconds, sampleRate);
        slow_.prepare(kColorGlideSeconds, sampleRate);
        fadeLength_ = std::max(1, static_cast<int>(std::lround(kFadeSeconds * sampleRate)));
        for (int c = 0; c < kChannels; ++c) {
            oversampler_[c].prepare(kChunk, kHqFactorLog2);
            oversampler_[c].setFactorLog2(kHqFactorLog2);
            dcBlocker_[c].prepare(sampleRate, kDcCutoffHz);
        }
        color_[0].rate = sampleRate;
        color_[1].rate = sampleRate * (1 << kHqFactorLog2);
        reportedLatency_ = latencySamples();
        reset();
    }

    // Silent, every glide and fade where the parameters are.
    void reset() override {
        for (int c = 0; c < kChannels; ++c) clearChannel(c);
        const Targets t = targets();
        drive_.snap(t.driveDb);
        output_.snap(t.outputDb);
        mix_.snap(t.mix);
        threshold_.snap(t.thresholdDb);
        for (int k = 0; k < kWsControls; ++k) ws_[k].snap(t.ws[static_cast<size_t>(k)]);
        driveGain_ = expDbToGain(static_cast<float>(drive_.value));
        outGain_ = expDbToGain(static_cast<float>(output_.value));
        mixNow_ = static_cast<float>(mix_.value);
        thresholdNow_ = saturator::thresholdGain(static_cast<float>(threshold_.value));
        for (int k = 0; k < kWsControls; ++k) wsNow_[k] = static_cast<float>(ws_[k].value);
        curve_ = saturator::params(thresholdNow_, waveshaper());
        type_.snap(t.type);
        clip_.snap(t.clip);
        dc_.snap(t.dc);
        hq_.snap(t.hq);

        const double base = t.color ? t.baseDb : 0.0, depth = t.color ? t.depthDb : 0.0;
        base_.snap(base);
        depth_.snap(depth);
        freq_.snap(t.logFreq);
        width_.snap(t.width);
        colorRunning_ = base != 0.0 || depth != 0.0;
        for (ColorFilters& f : color_) f.start(colorDesign(f.rate));
        shelfMoved_ = peakMoved_ = false;

        meterCount_ = 0;
        inPeak_ = outPeak_ = 0.f;
    }

protected:
    void render(const ProcessContext&, float* const* ch, int numChannels, int numFrames) override {
        const int n = std::min(numChannels, kChannels);
        if (n <= 0 || numFrames <= 0) return;
        if (n != channels_) {  // a channel that wasn't processed has no history to go on from
            clearChannel(1);
            channels_ = n;
        }
        const Targets t = targets();
        float* part[kChannels] = {};
        for (int at = 0; at < numFrames; at += kChunk) {
            const int len = std::min(kChunk, numFrames - at);
            for (int c = 0; c < n; ++c) part[c] = ch[c] + at;
            processChunk(part, n, len, t);
        }
    }

private:
    // The parameters as the glides and fades aim for them, read once per stretch.
    struct Targets {
        double driveDb = 0.0, outputDb = 0.0, mix = 1.0, thresholdDb = -18.0;
        std::array<double, kWsControls> ws{};
        double baseDb = 0.0, depthDb = 0.0, logFreq = 0.0, width = 50.0;
        int type = 0, clip = 0, dc = 0, hq = 0;
        bool color = false;
    };

    Targets targets() const noexcept {
        Targets t;
        t.driveDb = param(Drive);
        t.outputDb = param(Output);
        t.mix = std::clamp(param(Mix), 0.f, 100.f) / 100.f;
        t.thresholdDb = param(Threshold);
        for (int k = 0; k < kWsControls; ++k) t.ws[static_cast<size_t>(k)] = param(WsDrive + k) / 100.f;
        t.baseDb = param(Base);
        t.depthDb = param(Depth);
        t.logFreq = std::log(saturator::colorFrequency(param(Freq), sampleRate_));
        t.width = param(Width);
        t.type = std::clamp(choiceIndex(TypeParam), 0, saturator::kTypes - 1);
        t.clip = std::clamp(choiceIndex(ClipParam), 0, saturator::kClips - 1);
        t.color = isOn(Color);
        t.dc = isOn(Dc) ? 1 : 0;
        t.hq = isOn(Hq) ? 1 : 0;
        return t;
    }

    saturator::Waveshaper waveshaper() const noexcept {
        return {wsNow_[0], wsNow_[1], wsNow_[2], wsNow_[3], wsNow_[4], wsNow_[5]};
    }

    void processChunk(float* const* ch, int n, int len, const Targets& t) noexcept {
        const float step = 1.f / static_cast<float>(len);  // a ramp's share per sample

        // Lists and switches. The path Hi-Quality goes to starts from silence (its Color's filters too);
        // towards 4x it runs unheard first.
        type_.request(t.type);
        clip_.request(t.clip);
        dc_.request(t.dc);
        if (!hq_.fading() && t.hq != hq_.to) {
            if (t.hq == 1)
                for (auto& o : oversampler_) o.reset();
            ColorFilters& incoming = color_[t.hq];
            incoming.clear();
            incoming.start(colorDesign(incoming.rate));
            hq_.request(t.hq, t.hq == 1 ? kHqPreroll : 0);
        }
        const Type typeFrom = saturator::typeAt(type_.from), typeTo = saturator::typeAt(type_.to);
        const bool typeFading = type_.fading(), dcFading = dc_.fading(), hqFading = hq_.fading();
        const int hqTo = hq_.to;
        const float dcOn = static_cast<float>(dc_.to);
        float wType[kChunk], wClip[kChunk], wDc[kChunk], wHq[kChunk];  // each fade's weight of `to`
        const ClipFade clip = {clip_.fading(), saturator::clipAt(clip_.from), saturator::clipAt(clip_.to), wClip};
        if (typeFading) type_.weights(wType, len, fadeLength_);
        if (clip.fading) clip_.weights(wClip, len, fadeLength_);
        if (dcFading) {
            const bool on = dc_.to == 1;
            dc_.weights(wDc, len, fadeLength_);
            if (!on)
                for (int i = 0; i < len; ++i) wDc[i] = 1.f - wDc[i];
        }
        if (hqFading) {  // (as the 4x path's weight)
            hq_.weights(wHq, len, fadeLength_);
            if (hqTo == 0)
                for (int i = 0; i < len; ++i) wHq[i] = 1.f - wHq[i];
        }
        const bool run1x = hqFading || hqTo == 0, run4x = hqFading || hqTo == 1;

        // Glides, to the chunk's end; the gains ramp from where the last chunk ended.
        const float drive0 = driveGain_, out0 = outGain_, mix0 = mixNow_;
        if (drive_.advance(t.driveDb, fast_, len, kLandedDb))
            driveGain_ = expDbToGain(static_cast<float>(drive_.value));
        if (output_.advance(t.outputDb, fast_, len, kLandedDb))
            outGain_ = expDbToGain(static_cast<float>(output_.value));
        if (mix_.advance(t.mix, fast_, len, kLandedFraction)) mixNow_ = static_cast<float>(mix_.value);
        const Ramp drive = {drive0, (driveGain_ - drive0) * step}, mix = {mix0, (mixNow_ - mix0) * step};
        const float outStep = (outGain_ - out0) * step;

        // The curve's settings: ramped per sample while they glide (held, the curve would step every chunk).
        const float threshold0 = thresholdNow_;
        std::array<float, kWsControls> ws0{};
        std::copy(std::begin(wsNow_), std::end(wsNow_), ws0.begin());
        bool curveMoved = false;
        if (threshold_.advance(t.thresholdDb, fast_, len, kLandedDb)) {
            thresholdNow_ = saturator::thresholdGain(static_cast<float>(threshold_.value));
            curveMoved = true;
        }
        for (int k = 0; k < kWsControls; ++k) {
            if (ws_[k].advance(t.ws[static_cast<size_t>(k)], fast_, len, kLandedFraction)) {
                wsNow_[k] = static_cast<float>(ws_[k].value);
                curveMoved = true;
            }
        }
        const saturator::Params* curves = &curve_;
        int stride = 0;
        if (curveMoved) {
            curve_ = saturator::params(thresholdNow_, waveshaper());
            if (saturator::usesParams(typeFrom) || saturator::usesParams(typeTo)) {
                for (int i = 0; i + 1 < len; ++i) {
                    const float f = static_cast<float>(i + 1) * step;
                    const auto at = [&](int k) {
                        return ws0[static_cast<size_t>(k)] + (wsNow_[k] - ws0[static_cast<size_t>(k)]) * f;
                    };
                    ramp_[i] = saturator::params(threshold0 + (thresholdNow_ - threshold0) * f,
                                                 {at(0), at(1), at(2), at(3), at(4), at(5)});
                }
                ramp_[len - 1] = curve_;
                curves = ramp_;
                stride = 1;
            }
        }
        const Shaping shaping = {typeFading, typeFrom, typeTo, wType, curves, stride};

        updateColor(t, len, n, run1x, run4x);

        // The input: DC; the meters' and displays' share of it.
        float x[kChannels][kChunk], inMono[kChunk], inAbs[kChunk];
        const float share = 1.f / static_cast<float>(n);
        for (int i = 0; i < len; ++i) {
            const float wdc = dcFading ? wDc[i] : dcOn;
            float sum = 0.f, peak = 0.f;
            for (int c = 0; c < n; ++c) {
                const float raw = ch[c][i];
                const float blocked = dcBlocker_[c].process(raw);  // (always, so it is warm when switched on)
                const float v = wdc == 0.f ? raw : (wdc == 1.f ? blocked : raw + wdc * (blocked - raw));
                x[c][i] = v;
                sum += v;
                peak = std::max(peak, std::abs(v));
            }
            inMono[i] = sum * share;
            inAbs[i] = peak;
        }

        // The wet sound and Post Clip, at 1x and/or 4x.
        float y1[kChannels][kChunk], y4[kChannels][kChunk];
        if (run1x) {
            float* io[kChannels] = {y1[0], y1[1]};
            const float* dry[kChannels] = {x[0], x[1]};
            for (int c = 0; c < n; ++c) std::copy_n(x[c], len, y1[c]);
            path<0>(io, dry, n, len, drive, mix, shaping, clip);
        }
        if (run4x) {
            float* io[kChannels] = {};
            float dry4[kChannels][kHqChunk];
            const float* dry[kChannels] = {dry4[0], dry4[1]};
            for (int c = 0; c < n; ++c) {
                io[c] = oversampler_[c].up(x[c], len);
                std::copy_n(io[c], len << kHqFactorLog2, dry4[c]);
            }
            path<kHqFactorLog2>(io, dry, n, len, drive, mix, shaping, clip);
            for (int c = 0; c < n; ++c) oversampler_[c].down(io[c], len, y4[c]);
        }

        // Output; meters and displays.
        for (int i = 0; i < len; ++i) {
            const float og = out0 + outStep * static_cast<float>(i + 1);
            float sum = 0.f, peak = 0.f;
            for (int c = 0; c < n; ++c) {
                const float y =
                    hqFading ? (1.f - wHq[i]) * y1[c][i] + wHq[i] * y4[c][i] : (hqTo == 1 ? y4[c][i] : y1[c][i]);
                const float out = og * y;
                ch[c][i] = out;
                sum += out;
                peak = std::max(peak, std::abs(out));
            }
            publish(Input, inMono[i]);
            publish(OutputDisplay, sum * share);
            inPeak_ = std::max(inPeak_, inAbs[i]);
            outPeak_ = std::max(outPeak_, peak);
            if (++meterCount_ >= kMeterSamples) {
                publish(InPeak, inPeak_);
                publish(OutPeak, outPeak_);
                inPeak_ = outPeak_ = 0.f;
                meterCount_ = 0;
            }
        }
    }

    // A gain moving in a straight line across a chunk: `step` per base sample
    // from `start` (where the last chunk ended).
    struct Ramp {
        float start = 0.f, step = 0.f;
        // At oversampled sample j (2^shift a base sample): the line at its end.
        template <int Shift>
        float at(int j) const noexcept {
            return start + step * (static_cast<float>(j + 1) * (1.f / static_cast<float>(1 << Shift)));
        }
    };

    // The curve's type (crossfading while Type fades) and its settings per sample.
    struct Shaping {
        bool fading = false;
        Type from = Type::AnalogClip, to = Type::AnalogClip;
        const float* weight = nullptr;
        const saturator::Params* curves = nullptr;
        int stride = 0;
    };

    // One rate's path over a chunk, in place: `io` holds x (at 2^Shift the base
    // rate, `dry` a copy) and comes out as PostClip((1 - mix) x + mix wet),
    // wet = de-emphasis(curve(emphasis(Drive x))). Color's filters are that
    // rate's; while they glide they move base sample by base sample, the
    // emphasis over the chunk first, the de-emphasis after the curve.
    template <int Shift>
    void path(float* const* io, const float* const* dry, int n, int len, const Ramp& drive, const Ramp& mix,
              const Shaping& shaping, const ClipFade& clip) noexcept {
        constexpr int kFactor = 1 << Shift;
        ColorFilters& f = color_[Shift == 0 ? 0 : 1];
        const bool color = colorRunning_, moving = color && (shelfMoved_ || peakMoved_);
        for (int i = 0; i < len; ++i) {
            if (moving) f.emphasisAt(i, len, shelfMoved_, peakMoved_);
            for (int j = i * kFactor; j < (i + 1) * kFactor; ++j) {
                const float g = drive.at<Shift>(j);
                for (int c = 0; c < n; ++c) {  // (the channels' filters side by side: independent, they overlap)
                    const float u = g * io[c][j];
                    io[c][j] = color ? f.emphasize(c, u) : u;
                }
            }
        }
        for (int c = 0; c < n; ++c)
            shape(io[c], len * kFactor, Shift, shaping.fading, shaping.from, shaping.to, shaping.weight, shaping.curves,
                  shaping.stride);
        for (int i = 0; i < len; ++i) {
            if (moving) f.deemphasisAt(i, len, shelfMoved_, peakMoved_);
            for (int j = i * kFactor; j < (i + 1) * kFactor; ++j) {
                const float m = mix.at<Shift>(j);
                for (int c = 0; c < n; ++c) {
                    const float wet = color ? f.deemphasize(c, io[c][j]) : io[c][j];
                    io[c][j] = clip.clip(i, (1.f - m) * dry[c][j] + m * wet);
                }
            }
        }
    }

    // The curve over `count` samples in place (oversampled by 2^shift), crossfading types while Type fades.
    static void shape(float* data, int count, int shift, bool fading, Type from, Type to, const float* weight,
                      const saturator::Params* curves, int stride) noexcept {
        if (!fading) {
            kShapeRuns[static_cast<size_t>(to)](data, count, curves, stride, shift);
            return;
        }
        for (int j = 0; j < count; ++j) {
            const int i = j >> shift;
            const saturator::Params& p = curves[i * stride];
            const float w = weight[i];
            data[j] = (1.f - w) * saturator::curve(from, p, data[j]) + w * saturator::curve(to, p, data[j]);
        }
    }

    // --- Color ------------------------------------------------------------------------------

    // Color designed at the glides' values for `rate`.
    saturator::ColorDesign colorDesign(double rate) const noexcept {
        return saturator::colorDesign(base_.value, std::exp(freq_.value), width_.value, depth_.value, rate);
    }

    // Color's glides for a chunk: switched in when its gains leave 0 dB, the
    // paths that run redesigned at the chunk's end while they move, switched
    // out once back at 0 dB and quiet.
    void updateColor(const Targets& t, int len, int n, bool run1x, bool run4x) noexcept {
        shelfMoved_ = peakMoved_ = false;
        const double base = t.color ? t.baseDb : 0.0, depth = t.color ? t.depthDb : 0.0;
        if (!colorRunning_) {
            freq_.snap(t.logFreq);  // (nothing to hear while switched out)
            width_.snap(t.width);
            if (base == 0.0 && depth == 0.0) return;
            colorRunning_ = true;  // from silent states at 0 dB, gliding up
            for (ColorFilters& f : color_) {
                f.clear();
                f.start(colorDesign(f.rate));
            }
        }
        shelfMoved_ = base_.advance(base, slow_, len, kLandedDb);
        peakMoved_ = depth_.advance(depth, slow_, len, kLandedDb);
        peakMoved_ = freq_.advance(t.logFreq, slow_, len, kLandedFraction) || peakMoved_;
        peakMoved_ = width_.advance(t.width, slow_, len, kLandedDb) || peakMoved_;
        const bool runs[2] = {run1x, run4x};
        if (shelfMoved_ || peakMoved_) {
            for (int r = 0; r < 2; ++r)
                if (runs[r]) color_[r].moveTo(colorDesign(color_[r].rate), shelfMoved_, peakMoved_);
        } else if (base == 0.0 && depth == 0.0 && base_.settled(0.0) && depth_.settled(0.0) &&
                   (!run1x || color_[0].quiet(n, kQuietState)) && (!run4x || color_[1].quiet(n, kQuietState))) {
            colorRunning_ = false;
            for (ColorFilters& f : color_) f.clear();
        }
    }

    void clearChannel(int c) noexcept {
        dcBlocker_[c].reset();
        oversampler_[c].reset();
        for (ColorFilters& f : color_) f.clear(c);
    }

    static const std::vector<ParamInfo>& infos() {
        static const std::vector<ParamInfo> kInfos = [] {
            std::vector<ParamInfo> list = {
                {"drive", "Drive", "dB", -36.f, 36.f, 0.f},
                {"type", "Type", "", 0.f, static_cast<float>(saturator::kTypes - 1), 0.f, false,
                 saturator::typeLabels()},
                {"threshold", "Threshold", "dB", -50.f, 0.f, -18.f},
                {"output", "Output", "dB", -36.f, 0.f, 0.f},
                {"mix", "Dry/Wet", "%", 0.f, 100.f, 100.f},
                {"clip", "Post Clip", "", 0.f, static_cast<float>(saturator::kClips - 1), 0.f, false,
                 saturator::clipLabels()},
                {"color", "Color", "", 0.f, 1.f, 0.f, false, offOnLabels()},
                {"base", "Base", "dB", -36.f, 36.f, 0.f},
                {"freq", "Frequency", "Hz", 30.f, 18500.f, 1000.f, true},
                {"width", "Width", "%", 0.f, 100.f, 50.f},
                {"depth", "Depth", "dB", -36.f, 36.f, 0.f},
                {"dc", "DC", "", 0.f, 1.f, 0.f, false, offOnLabels()},
                {"hq", "Hi-Quality", "", 0.f, 1.f, 0.f, false, offOnLabels()},
                {"ws_drive", "WS Drive", "%", 0.f, 100.f, 50.f},
                {"ws_lin", "WS Lin", "%", 0.f, 100.f, 50.f},
                {"ws_curve", "WS Curve", "%", 0.f, 100.f, 50.f},
                {"ws_damp", "WS Damp", "%", 0.f, 100.f, 0.f},
                {"ws_depth", "WS Depth", "%", 0.f, 100.f, 0.f},
                {"ws_period", "WS Period", "%", 0.f, 100.f, 0.f},
            };
            list[Hq].automatable = false;  // (it is the device's latency)
            return list;
        }();
        return kInfos;
    }

    double sampleRate_ = 48000.0;
    GlideTable fast_, slow_;
    int fadeLength_ = 480;
    const int hqLatency_;
    int reportedLatency_ = 0;
    int channels_ = kChannels;

    Glide drive_, output_, mix_, threshold_;
    Glide ws_[kWsControls];
    float driveGain_ = 1.f, outGain_ = 1.f, mixNow_ = 1.f;  // at the last chunk's end
    float thresholdNow_ = 0.125893f;
    float wsNow_[kWsControls] = {};
    saturator::Params curve_;         // the curve's settings at the last chunk's end
    saturator::Params ramp_[kChunk];  // and per sample across a chunk while they glide
    Switch type_, clip_, dc_, hq_;

    // Color: emphasis (shelf, then peak) and de-emphasis (the peak's inverse, then the shelf's), for the
    // base rate and for the 4x path.
    bool colorRunning_ = false;
    Glide base_, depth_, freq_, width_;            // dB, dB, log Hz, %
    bool shelfMoved_ = false, peakMoved_ = false;  // which sections move in this chunk
    ColorFilters color_[2];

    dsp::Oversampler oversampler_[kChannels];
    dsp::DcBlocker dcBlocker_[kChannels];

    int meterCount_ = 0;
    float inPeak_ = 0.f, outPeak_ = 0.f;
};

}  // namespace

SUB_REGISTER_BUILTIN(SaturatorProcessor, AudioEffect);

}  // namespace sub
