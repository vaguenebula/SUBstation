// Built-in "Spectral Compressor" device: a compressor per frequency. It splits
// the sound into about a thousand bins (a short-time Fourier transform),
// measures each bin's level (smoothed across neighbouring frequencies and over
// time), compares it with a threshold curve that follows a pink spectrum
// (turned about 1 kHz by Tilt), turns each bin down above the curve (Ratio)
// and, optionally, up below a second curve (Below and Upward), and puts the
// sound back together. Resonances and harsh regions are tamed where they
// occur, the rest left alone; with Upward too the spectrum is pulled towards
// the curve. Keyed by a sidechain, the key's spectrum sets each bin's gain
// (one track's frequencies ducked where another's clash with them). The maths
// the editor shares (frame size, latency, calibration, curves, gain computer)
// is in builtin/SpectralDesign.h.
//
// - The STFT: a periodic Hann window of N samples (frameSize(): 2048 at 44.1
//   and 48 kHz, scaled with the rate so a bin is about 23 Hz wide), a hop of
//   H = N/4, the same window again on the way out (Hann² at 4x overlap sums
//   to 1.5). Input goes into a ring; every hop the last N samples are a
//   frame, transformed, gained, transformed back and added into an
//   accumulator read (and cleared) as the output.
// - The work is spread: a frame's transforms and per-bin loops are cut into
//   small steps (a transform in about ten, a loop over the bins in parts of
//   about 256) and run evenly through the hop after the frame went in, so an
//   audio callback carries its share of the frame, not all of it (at 192 kHz
//   a whole frame takes longer than a 32-sample callback lasts). The cost is
//   that hop: the latency is N + H, exactly (latencySamples()). The dry path
//   reads the input ring that far back, so Dry/Wet blends in time with no
//   delay line of its own, and at 0 % passes the input, delayed, bit for bit.
// - Levels: each bin's power, its mean over a band constant in octaves
//   (Smoothing: 0 to 2 octaves; prefix sums), then an envelope on its
//   magnitude per bin and hop (Attack and Release as one-poles at the hop
//   rate, so they mean what the Compressor's do), then dB calibrated and
//   pink-referenced (calibrationDb(), pinkDb()). Stereo Link blends each
//   channel's level towards the louder one's in dB.
// - Gains: the shared gain computer (the Compressor's quadratic knee, down
//   above the threshold, up under Below and faded out from -90 dB, within
//   ±Range), times the Focus band's weight, smoothed once across bins ([1 2 1]
//   / 4, so a jagged gain smears less in time), then applied to the bins.
// - Changes never click: each frame's gains are faded in and out by the
//   synthesis window and overlap four ways, so a jump becomes a crossfade over
//   about N samples. Threshold, Below, Tilt, Knee, Range, the ratios' slopes,
//   Stereo Link and the Focus edges also glide per hop (30 ms) so a sweep
//   doesn't step; Dry/Wet, Output and Delta ramp per sample (20 ms).
// - Automation stays with the audio. The engine hands the device its
//   parameters in step with its input; a frame works with those of two hops
//   before it, when its centre went in, and Dry/Wet, Output and Delta reach
//   the output with the input they came with, the latency on (rings, as the
//   dry path's).
// - A frame whose input (and key) is all exact zeros skips its transforms: its
//   envelopes release, and nothing is added to the output. So silence costs
//   little and comes out as exact zeros 2N + H after the last sound.
// - Displays: per hop, 128 values (log-spaced 20 Hz..20 kHz) of the input
//   spectrum, the levels compared, the output spectrum and the gains, published
//   three hops late so they are in step with what is heard; and the In and
//   Out peaks over the hop.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/SpectralDesign.h"
#include "rt/RtUtils.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "signalsmith-linear/fft.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace sub {
namespace {

using Complex = std::complex<float>;

constexpr int kChannels = 2;  // a sidechain is always stereo; the main input up to two
// The envelopes' floor: a magnitude of 1e-6, -120 dB uncalibrated (about -154 dB pink-referenced at 1 kHz).
constexpr float kTinyMagnitude = 1e-6f;
constexpr float kMeterFloorDb = -90.f;  // in_level and out_level
// What the displays read at the most: far above any audio (a full-scale tone reads +20 dB in the spectra), so they
// stay finite when an absurd input sample (within BuiltinProcessor::kMaxInput) overflows a bin's power.
constexpr float kTopDb = 300.f;
constexpr int kPoints = spectral::kDisplayPoints;
constexpr int kSpectralStreams = 4;  // input, key, output, gain
constexpr int kSlots = 3;            // parameter snapshots and display frames kept: this frame's and two more
constexpr int kBinsPerPart = 256;    // a loop over the bins is run in parts of about this many
constexpr int kMaxSteps = 256;       // a frame's steps at the most (222: 192 kHz, keyed, unlinked, all moving)
constexpr float kFloorDb = static_cast<float>(spectral::kFloorDb);
constexpr float kUpwardFloorDb = static_cast<float>(spectral::kUpwardFloorDb);
constexpr float kInvUpwardFadeDb = static_cast<float>(1.0 / spectral::kUpwardFadeDb);
constexpr float kInvFocusEdge = static_cast<float>(1.0 / spectral::kFocusEdgeOctaves);
constexpr double kLinkedFully = 0.9999;  // Stereo Link from here: one set of gains for both channels
constexpr double kLanded = 1e-7;         // a glide this close to its target lands on it

// A key sample as the input's are taken (BuiltinProcessor::process()): NaN, infinity and absurd levels as silence.
inline float audioOrSilence(float x) noexcept { return std::abs(x) <= BuiltinProcessor::kMaxInput ? x : 0.f; }

// How far past a threshold a level counts: spectral::kneed() in float, with the knee's halves worked out once.
struct KneeCurve {
    float half = 0.f, invTwice = 0.f;  // knee / 2, 1 / (2 knee)

    explicit KneeCurve(float knee) noexcept : half(0.5f * knee), invTwice(knee > 0.f ? 0.5f / knee : 0.f) {}
    float operator()(float over) const noexcept {
        if (over <= -half) return 0.f;
        if (over < half) {
            const float x = over + half;
            return x * x * invTwice;
        }
        return over;
    }
};

// Where display point j finds its value in the bins.
struct DisplayPoint {
    enum Kind { Range, Interpolate, AboveNyquist };
    Kind kind = AboveNyquist;
    int k0 = 0, k1 = 0;    // Range: bins k0..k1 (inclusive); Interpolate: between k0 and k1 = k0 + 1
    float t = 0.f;         // Interpolate: how far towards k1
    float invCount = 0.f;  // Range: 1 / its number of bins
    float pinkDb = 0.f;    // pinkDb() at the point's frequency, plus the calibration
};

class SpectralProcessor final : public BuiltinProcessor {
public:
    enum Param {
        Threshold = 0,
        Ratio,
        Below,
        Upward,
        Tilt,
        Knee,
        Range,
        Smooth,
        FocusLo,
        FocusHi,
        Attack,
        Release,
        Link,
        Mix,
        Output,
        Delta,
        NumParams
    };
    enum Display { InputDisplay = 0, KeyDisplay, OutputDisplay, GainDisplay, InLevel, OutLevel };
    using Params = std::array<float, NumParams>;

    SpectralProcessor()
        : BuiltinProcessor(
              infos(), {{"input", 4}, {"key", 4}, {"output", 4}, {"gain", 4}, {"in_level", 512}, {"out_level", 512}}) {}

    std::string typeId() const override { return "builtin:spectral"; }
    std::string name() const override { return "Spectral Compressor"; }
    bool hasSidechain() const override { return true; }

    // What goes in comes out N + H samples later; the last frames to hear a sound end N after that.
    int latencySamples() const override { return latency_; }
    int tailSamples() const override { return frame_; }

    // The latency changes only with the sample rate (in prepare(), with audio stopped).
    bool idle() override {
        if (latency_ == reportedLatency_) return false;
        reportedLatency_ = latency_;
        return true;
    }

    void prepare(double sampleRate, int) override {
        sampleRate_ = sampleRate;
        frame_ = spectral::frameSize(sampleRate);
        hop_ = frame_ / spectral::kOverlap;
        latency_ = spectral::latencySamples(sampleRate);
        bins_ = frame_ / 2 + 1;
        ring_ = 2 * frame_;  // holds a frame while its steps read it, and the dry path the latency back
        ringMask_ = ring_ - 1;
        parts_ = std::max(1, bins_ / kBinsPerPart);
        const auto n = static_cast<size_t>(frame_), b = static_cast<size_t>(bins_), r = static_cast<size_t>(ring_);

        fft_.resize(n);
        fftSteps_ = static_cast<int>(fft_.steps());
        window_.assign(n, 0.f);
        synthesis_.assign(n, 0.f);
        for (size_t i = 0; i < n; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / frame_);
            window_[i] = static_cast<float>(w);
            synthesis_[i] = static_cast<float>(w / (1.5 * frame_));  // Hann² sums to 1.5; the inverse gives N times
        }
        frameBuffer_.assign(n, 0.f);
        keySpec_.assign(n / 2, Complex{});
        mixRing_.assign(r, 0.f);
        outputRing_.assign(r, 1.f);
        deltaRing_.assign(r, 0.f);
        for (int c = 0; c < kChannels; ++c) {
            inRing_[c].assign(r, 0.f);
            keyRing_[c].assign(r, 0.f);
            accum_[c].assign(r, 0.f);
            spec_[c].assign(n / 2, Complex{});
            power_[c].assign(b, 0.f);
            keyPower_[c].assign(b, 0.f);
            env_[c].assign(b, kTinyMagnitude);
            level_[c].assign(b, 0.f);
            gainDb_[c].assign(b, 0.f);
            gainLin_[c].assign(b, 1.f);
        }
        prefix_.assign(b + 1, 0.0);
        boxLo_.assign(b, 0);
        boxHi_.assign(b, 0);
        boxInv_.assign(b, 1.f);
        scratch_.assign(b, 0.f);
        shownInput_.assign(b, 0.f);
        shownKey_.assign(b, 0.f);
        shownOutput_.assign(b, 0.f);
        shownGain_.assign(b, 0.f);

        calibration_ = static_cast<float>(spectral::calibrationDb(sampleRate, frame_));
        oct_.assign(b, 0.f);
        pinkCal_.assign(b, 0.f);
        focusOct_.assign(b, 0.f);
        focus_.assign(b, 1.f);
        floorLevel_.assign(b, 0.f);
        const float tinyDb = 20.f * std::log10(kTinyMagnitude);
        for (int k = 0; k < bins_; ++k) {
            const double freq = k * sampleRate / frame_;
            oct_[static_cast<size_t>(k)] = static_cast<float>(spectral::octavesFromPivot(freq));
            pinkCal_[static_cast<size_t>(k)] = static_cast<float>(spectral::pinkDb(freq)) + calibration_;
            focusOct_[static_cast<size_t>(k)] = static_cast<float>(spectral::focusOctaves(freq));
            floorLevel_[static_cast<size_t>(k)] = tinyDb + pinkCal_[static_cast<size_t>(k)];
        }
        buildDisplayTable();

        glide_ = 1.0 - onePoleCoefficient(spectral::kGlideSeconds, sampleRate, hop_);
        mix_.reset(sampleRate, 0.02);
        output_.reset(sampleRate, 0.02);
        delta_.reset(sampleRate, 0.02);
        reportedLatency_ = latency_;
        reset();
    }

    // Real-time: silent, every glide where the parameters are.
    void reset() override {
        if (frame_ == 0) return;
        for (int c = 0; c < kChannels; ++c) {
            std::fill(inRing_[c].begin(), inRing_[c].end(), 0.f);
            std::fill(keyRing_[c].begin(), keyRing_[c].end(), 0.f);
            std::fill(accum_[c].begin(), accum_[c].end(), 0.f);
            std::fill(spec_[c].begin(), spec_[c].end(), Complex{});
            std::fill(power_[c].begin(), power_[c].end(), 0.f);
            std::fill(keyPower_[c].begin(), keyPower_[c].end(), 0.f);
            std::fill(env_[c].begin(), env_[c].end(), kTinyMagnitude);
            std::fill(level_[c].begin(), level_[c].end(), 0.f);
            std::fill(gainDb_[c].begin(), gainDb_[c].end(), 0.f);
            std::fill(gainLin_[c].begin(), gainLin_[c].end(), 1.f);
        }
        for (auto& slot : displayRing_) {
            for (int s = 0; s < kSpectralStreams; ++s) {
                std::fill(slot[static_cast<size_t>(s)].begin(), slot[static_cast<size_t>(s)].end(),
                          s == GainDisplay ? 0.f : kFloorDb);
            }
        }
        write_ = 0;
        hopCount_ = 0;
        slot_ = 0;
        stepCount_ = stepsDone_ = 0;  // no frame in flight
        quiet_ = frame_;              // it starts silent
        peakIn_ = peakOut_ = 0.f;
        keyed_ = false;
        snapToParams();
        // And again at the first stretch: what is set before it (a project loading) holds from the start.
        fresh_ = true;
    }

protected:
    void render(const ProcessContext&, float* const* ch, int numChannels, int numFrames) override {
        const int n = std::min(numChannels, kChannels);
        if (n <= 0 || frame_ == 0) return;
        if (fresh_) {
            snapToParams();
            fresh_ = false;
        }
        if (n != channels_) {  // a channel not processed until now has no history to go on from
            if (channels_ != 0) clearChannel(1);
            channels_ = n;
        }
        // A source chosen keys it (its pointers null while it is silent); else its own input does.
        keyed_ = sidechainConnected();
        const float* keyL = keyed_ ? sidechain(0) : nullptr;
        const float* keyR = keyed_ ? sidechain(1) : nullptr;
        // What the output's controls ask for now reaches the output with the input it came with, the latency on
        // (automation stays with the audio, as the engine keeps it with the device's input).
        const float mixNow = param(Mix) / 100.f, outputNow = expDbToGain(param(Output));
        const float deltaNow = isOn(Delta) ? 1.f : 0.f;

        float* in[kChannels] = {inRing_[0].data(), inRing_[1].data()};
        float* mixes = mixRing_.data();
        float* outputs = outputRing_.data();
        float* deltas = deltaRing_.data();
        float* acc[kChannels] = {accum_[0].data(), accum_[1].data()};
        float* keys[kChannels] = {keyRing_[0].data(), keyRing_[1].data()};
        const int back = ring_ - latency_;  // from the slot written to the one written the latency ago
        for (int i = 0; i < numFrames;) {
            const int run = std::min(numFrames - i, hop_ - hopCount_);
            for (const int end = i + run; i < end; ++i) {
                const int w = write_, r = (w + back) & ringMask_;
                float dry[kChannels] = {}, wet[kChannels] = {};
                bool silent = true;
                for (int c = 0; c < n; ++c) {
                    const float x = ch[c][i];
                    dry[c] = in[c][r];  // the input the latency ago: in step with the wet
                    in[c][w] = x;
                    wet[c] = acc[c][w];
                    acc[c][w] = 0.f;
                    silent = silent && x == 0.f;
                }
                // (The key is the engine's buffer, not sanitised as the input is: what isn't audio is silence.)
                const float kl = keyL ? audioOrSilence(keyL[i]) : 0.f, kr = keyR ? audioOrSilence(keyR[i]) : 0.f;
                keys[0][w] = kl;
                keys[1][w] = kr;
                silent = silent && kl == 0.f && kr == 0.f;
                quiet_ = silent ? std::min(quiet_ + 1, frame_) : 0;
                mix_.setTarget(mixes[r]);
                output_.setTarget(outputs[r]);
                delta_.setTarget(deltas[r]);
                mixes[w] = mixNow;
                outputs[w] = outputNow;
                deltas[w] = deltaNow;
                write_ = (w + 1) & ringMask_;

                const float m = mix_.next(), o = output_.next(), d = delta_.next();
                for (int c = 0; c < n; ++c) {
                    const float change = m * (wet[c] - dry[c]);  // what the device changes, at Dry/Wet's share
                    // Delta off: dry + change; on: -change = Dry/Wet × (dry - wet).
                    const float y = ((1.f - d) * dry[c] + (1.f - 2.f * d) * change) * o;
                    ch[c][i] = y;
                    peakIn_ = std::max(peakIn_, std::abs(dry[c]));
                    peakOut_ = std::max(peakOut_, std::abs(y));
                }
            }
            hopCount_ += run;
            // The frame in flight's steps, evenly through the hop: by its end, all of them (its output is read
            // from the next sample on).
            runSteps((stepCount_ * hopCount_ + hop_ - 1) / hop_);
            if (hopCount_ == hop_) {
                hopCount_ = 0;
                beginFrame();
            }
        }
    }

private:
    // The kinds of step a frame's work is cut into. Each runs one part: a step of a transform, or a share of the
    // bins.
    enum class Job : uint8_t {
        Boxes,       // Smoothing's band per bin (when Smoothing changed)
        Focus,       // the Focus weight per bin (when the edges moved)
        Analyse,     // a main channel's forward transform, its power per bin
        AnalyseKey,  // a key channel's
        Envelope,    // a detector channel's envelope per bin (its smoothing across bins first)
        Levels,      // pink-referenced dB, linked
        Gains,       // the gain computer per bin, for a set of levels
        Smooth,      // the gains smoothed across bins, as gains to multiply by
        Synthesise,  // a main channel gained, transformed back, added into its accumulator
        Display      // the frame's display values
    };
    struct Step {
        Job job = Job::Display;
        uint8_t channel = 0;
        uint16_t part = 0;
    };

    // Every glide, ramp, ring of the output's controls and parameter snapshot where the parameters are now.
    void snapToParams() {
        for (auto& snapshot : paramRing_) {
            for (int i = 0; i < NumParams; ++i) snapshot[static_cast<size_t>(i)] = param(i);
        }
        std::fill(mixRing_.begin(), mixRing_.end(), param(Mix) / 100.f);
        std::fill(outputRing_.begin(), outputRing_.end(), expDbToGain(param(Output)));
        std::fill(deltaRing_.begin(), deltaRing_.end(), isOn(Delta) ? 1.f : 0.f);
        threshold_ = param(Threshold);
        below_ = param(Below);
        tilt_ = param(Tilt);
        knee_ = param(Knee);
        range_ = param(Range);
        down_ = spectral::downSlope(param(Ratio));
        up_ = spectral::upSlope(param(Upward));
        link_ = param(Link) / 100.0;
        focusLo_ = spectral::focusOctaves(param(FocusLo));
        focusHi_ = spectral::focusOctaves(param(FocusHi));
        focusFor_[0] = focusFor_[1] = NAN;  // worked out again by the next frame
        boxFor_ = -1.f;
        attackFor_ = releaseFor_ = -1.f;
        mix_.snapTo(param(Mix) / 100.f);
        output_.snapTo(expDbToGain(param(Output)));
        delta_.snapTo(isOn(Delta) ? 1.f : 0.f);
    }

    // --- Per hop ---------------------------------------------------------------------------

    // A frame's inputs are all in: publishes the displays of the frame now heard, works out what this frame
    // is (its parameters, silent or not, keyed or not, how many sets of gains) and plans its steps.
    void beginFrame() {
        // The parameters now, kept for two frames on; this frame works with those of two frames ago, when its
        // centre went in (so a change meets the audio it came with, as the displays do).
        const auto slot = static_cast<size_t>(slot_);
        for (int i = 0; i < NumParams; ++i) paramRing_[slot][static_cast<size_t>(i)] = param(i);
        frameParams_ = slot_ == kSlots - 1 ? 0 : slot_ + 1;
        const Params& p = paramRing_[static_cast<size_t>(frameParams_)];

        // The frame from three hops ago is centred on what is heard now: its displays go out, and this frame's
        // fill its slot. The hop's peaks with them.
        for (int s = 0; s < kSpectralStreams; ++s) {
            for (const float v : displayRing_[slot][static_cast<size_t>(s)]) publish(s, v);
        }
        publish(InLevel, std::clamp(gainToDb(peakIn_), kMeterFloorDb, kTopDb));
        publish(OutLevel, std::clamp(gainToDb(peakOut_), kMeterFloorDb, kTopDb));
        peakIn_ = peakOut_ = 0.f;
        frameSlot_ = slot_;
        slot_ = slot_ == kSlots - 1 ? 0 : slot_ + 1;

        updateControls(p);
        frameQuiet_ = quiet_ >= frame_;  // the frame's inputs and keys are all exact zeros
        frameKeyed_ = keyed_;
        frameChannels_ = channels_;
        detectors_ = keyed_ ? kChannels : channels_;
        // One set of gains when there is one detector, one main channel (keyed: the louder key channel's) or the
        // link is full; else each channel's.
        single_ = detectors_ == 1 || channels_ == 1 || link_ >= kLinkedFully;
        sets_ = single_ ? 1 : 2;
        resting_ = frameQuiet_;  // (a silent frame's envelopes may all be at their floor: its steps find out)
        frameIn_ = (write_ + ring_ - frame_) & ringMask_;
        frameOut_ = (write_ + hop_) & ringMask_;

        stepCount_ = stepsDone_ = 0;
        const auto add = [this](Job job, int channel, int parts) {
            for (int part = 0; part < parts && stepCount_ < kMaxSteps; ++part)
                steps_[static_cast<size_t>(stepCount_++)] =
                    Step{job, static_cast<uint8_t>(channel), static_cast<uint16_t>(part)};
        };
        if (p[Smooth] != boxFor_) {
            boxFor_ = p[Smooth];
            boxHalf_ = std::exp2(0.5 * spectral::smoothingOctaves(boxFor_));
            boxWide_ = boxFor_ > 0.f;
            add(Job::Boxes, 0, parts_);
        }
        if (focusLo_ != focusFor_[0] || focusHi_ != focusFor_[1]) {
            focusFor_[0] = focusLo_;
            focusFor_[1] = focusHi_;
            add(Job::Focus, 0, parts_);
        }
        if (!frameQuiet_) {
            for (int c = 0; c < channels_; ++c) add(Job::Analyse, c, fftSteps_);
            if (keyed_) {
                for (int c = 0; c < kChannels; ++c) add(Job::AnalyseKey, c, fftSteps_);
            }
        }
        for (int c = 0; c < detectors_; ++c) add(Job::Envelope, c, parts_);
        add(Job::Levels, 0, parts_);
        for (int s = 0; s < sets_; ++s) {
            add(Job::Gains, s, parts_);
            add(Job::Smooth, s, parts_);
        }
        if (!frameQuiet_) {
            for (int c = 0; c < channels_; ++c) add(Job::Synthesise, c, fftSteps_);
        }
        add(Job::Display, 0, parts_ + 2);
    }

    // Runs the frame's steps until `due` of them are done.
    void runSteps(int due) {
        while (stepsDone_ < due) {
            const Step step = steps_[static_cast<size_t>(stepsDone_++)];
            const int c = step.channel, part = step.part;
            switch (step.job) {
            case Job::Boxes: makeBoxes(part); break;
            case Job::Focus: makeFocus(part); break;
            case Job::Analyse: analyse(inRing_[c].data(), spec_[c].data(), power_[c].data(), part); break;
            case Job::AnalyseKey: analyse(keyRing_[c].data(), keySpec_.data(), keyPower_[c].data(), part); break;
            case Job::Envelope: envelope(c, part); break;
            case Job::Levels: levels(part); break;
            case Job::Gains: gains(c, part); break;
            case Job::Smooth: smoothGains(c, part); break;
            case Job::Synthesise: synthesise(c, part); break;
            case Job::Display: display(part); break;
            }
        }
    }

    // The bins a per-bin step's part covers: [first, second).
    std::pair<int, int> binsOf(int part) const { return {part * bins_ / parts_, (part + 1) * bins_ / parts_}; }

    // Glides the curves' controls one hop on towards `p`, and works out the envelopes' coefficients when the
    // times change.
    void updateControls(const Params& p) {
        const auto glide = [this](double& value, double target) {
            value += glide_ * (target - value);
            if (std::abs(target - value) < kLanded) value = target;
        };
        glide(threshold_, p[Threshold]);
        glide(below_, p[Below]);
        glide(tilt_, p[Tilt]);
        glide(knee_, p[Knee]);
        glide(range_, p[Range]);
        glide(down_, spectral::downSlope(p[Ratio]));
        glide(up_, spectral::upSlope(p[Upward]));
        glide(link_, p[Link] / 100.0);
        glide(focusLo_, spectral::focusOctaves(p[FocusLo]));
        glide(focusHi_, spectral::focusOctaves(p[FocusHi]));

        const float attack = p[Attack], release = p[Release];
        if (attack != attackFor_) {
            attackFor_ = attack;
            attackCoeff_ = static_cast<float>(onePoleCoefficient(std::max(0.001f, attack) * 0.001, sampleRate_, hop_));
        }
        if (release != releaseFor_) {
            releaseFor_ = release;
            releaseCoeff_ =
                static_cast<float>(onePoleCoefficient(std::max(0.001f, release) * 0.001, sampleRate_, hop_));
        }
    }

    // Smoothing's band for each bin of a part: a box constant in octaves, centred on the bin in log frequency.
    void makeBoxes(int part) {
        const auto [first, end] = binsOf(part);
        const int top = bins_ - 1;
        for (int k = first; k < end; ++k) {
            const int lo = std::clamp(static_cast<int>(std::lround(k / boxHalf_)), 0, k);
            const int hi = std::clamp(static_cast<int>(std::lround(k * boxHalf_)), k, top);
            boxLo_[static_cast<size_t>(k)] = lo;
            boxHi_[static_cast<size_t>(k)] = hi;
            boxInv_[static_cast<size_t>(k)] = 1.f / static_cast<float>(hi - lo + 1);
        }
    }

    // The Focus weight for each bin of a part, from the glided edges (in octaves; focusOct_ is a table).
    void makeFocus(int part) {
        const auto [first, end] = binsOf(part);
        const auto lo = static_cast<float>(focusFor_[0]), hi = static_cast<float>(focusFor_[1]);
        for (int k = first; k < end; ++k) {
            const float o = focusOct_[static_cast<size_t>(k)];
            const float lower = std::clamp((o - lo) * kInvFocusEdge + 1.f, 0.f, 1.f);
            const float upper = std::clamp((hi - o) * kInvFocusEdge + 1.f, 0.f, 1.f);
            focus_[static_cast<size_t>(k)] = lower * upper;
        }
    }

    // A step of a forward transform of the frame in `ring`: the first windows it (oldest first), the last
    // unpacks each bin's power |X|².
    void analyse(const float* ring, Complex* spec, float* power, int part) {
        float* frame = frameBuffer_.data();
        if (part == 0) {
            const float* win = window_.data();
            const int first = std::min(frame_, ring_ - frameIn_);  // from the oldest sample to the ring's end
            for (int i = 0; i < first; ++i) frame[i] = ring[frameIn_ + i] * win[i];
            for (int i = first; i < frame_; ++i) frame[i] = ring[i - first] * win[i];
        }
        fft_.fft(static_cast<size_t>(part), frame, spec);
        if (part == fftSteps_ - 1) {
            const int nyquist = bins_ - 1;
            power[0] = spec[0].real() * spec[0].real();  // DC and Nyquist come packed in the first bin
            power[nyquist] = spec[0].imag() * spec[0].imag();
            for (int k = 1; k < nyquist; ++k) power[k] = std::norm(spec[k]);
        }
    }

    // A part of a detector channel's envelopes. Each bin's level is the mean power over its band (Smoothing;
    // the power itself at 0 %), followed as a magnitude (its square root) by a one-pole with the attack rising
    // and the release falling: as the Compressor's follower and nih-plug's, so a time means the same here. A
    // silent frame's levels fall with the release.
    void envelope(int c, int part) {
        const auto [first, end] = binsOf(part);
        float* env = env_[c].data();
        if (frameQuiet_) {
            const float release = releaseCoeff_;
            bool resting = resting_;
            for (int k = first; k < end; ++k) {
                const float e = release * env[k];
                env[k] = e > kTinyMagnitude ? e : kTinyMagnitude;
                resting = resting && env[k] == kTinyMagnitude;
            }
            resting_ = resting;
            return;
        }
        const float* power = (frameKeyed_ ? keyPower_[c] : power_[c]).data();
        if (boxWide_ && part == 0) {  // the prefix sums the boxes' means come from
            double sum = 0.0;
            prefix_[0] = 0.0;
            for (int k = 0; k < bins_; ++k) prefix_[static_cast<size_t>(k) + 1] = sum += power[k];
        }
        const float attack = attackCoeff_, release = releaseCoeff_;
        for (int k = first; k < end; ++k) {
            const auto i = static_cast<size_t>(k);
            float mean = power[k];
            if (boxWide_) {
                mean = static_cast<float>(prefix_[static_cast<size_t>(boxHi_[i]) + 1] -
                                          prefix_[static_cast<size_t>(boxLo_[i])]) *
                       boxInv_[i];
                mean = std::max(0.f, mean);
            }
            const float x = std::sqrt(mean), e = env[k];
            const float next = x + (x > e ? attack : release) * (e - x);
            // (A power overflowed by an absurd input sample makes NaN here, which lands on the floor too.)
            env[k] = next > kTinyMagnitude ? next : kTinyMagnitude;
        }
    }

    // A part of the levels in pink-referenced dB: one set (the louder detector's) or each channel's, blended in
    // dB towards the louder one's by Stereo Link.
    void levels(int part) {
        const auto [first, end] = binsOf(part);
        if (resting_) {
            for (int s = 0; s < sets_; ++s)
                std::copy(floorLevel_.begin() + first, floorLevel_.begin() + end, level_[s].begin() + first);
        } else if (single_) {
            float* level = level_[0].data();
            const float* e0 = env_[0].data();
            const float* e1 = env_[detectors_ - 1].data();
            for (int k = first; k < end; ++k) level[k] = 20.f * std::log10(std::max(e0[k], e1[k])) + pinkCal_[k];
        } else {
            float* l0 = level_[0].data();
            float* l1 = level_[1].data();
            const auto link = static_cast<float>(link_);
            for (int k = first; k < end; ++k) {
                const auto i = static_cast<size_t>(k);
                const float a = 20.f * std::log10(env_[0][i]) + pinkCal_[i];
                const float b = 20.f * std::log10(env_[1][i]) + pinkCal_[i];
                const float loudest = std::max(a, b);
                l0[k] = a + link * (loudest - a);
                l1[k] = b + link * (loudest - b);
            }
        }
    }

    // A part of the gain computer for a set of levels (spectral::gainDb() in float, times the Focus weight),
    // into the scratch row smoothGains() reads.
    void gains(int set, int part) {
        const auto [first, end] = binsOf(part);
        const float* level = level_[set].data();
        const auto threshold = static_cast<float>(threshold_), below = static_cast<float>(std::min(below_, threshold_));
        const auto tilt = static_cast<float>(tilt_), down = static_cast<float>(down_), up = static_cast<float>(up_);
        const auto range = static_cast<float>(range_);
        const KneeCurve knee(static_cast<float>(knee_));
        float* raw = scratch_.data();
        for (int k = first; k < end; ++k) {
            const auto i = static_cast<size_t>(k);
            const float weight = focus_[i];
            if (weight == 0.f) {
                raw[k] = 0.f;
                continue;
            }
            const float l = level[k], turn = tilt * oct_[i];
            float gain = 0.f;
            if (down != 0.f) gain = down * knee(l - (threshold + turn));
            if (up != 0.f) {
                const float fade = std::clamp((l - kUpwardFloorDb) * kInvUpwardFadeDb, 0.f, 1.f);
                if (fade > 0.f) gain += up * fade * knee((below + turn) - l);
            }
            raw[k] = weight * std::clamp(gain, -range, range);
        }
    }

    // A part of the gains smoothed once across bins, [1 2 1] / 4 (the ends take themselves for the missing
    // neighbour): a raised cosine over the gain's impulse response, so a jagged gain smears less in time. It
    // stays within ±Range. Then, for a frame with sound, as gains to multiply by; one set serves both channels.
    void smoothGains(int set, int part) {
        const auto [first, end] = binsOf(part);
        const float* raw = scratch_.data();
        float* gainDb = gainDb_[set].data();
        float* gainLin = gainLin_[set].data();
        const int last = bins_ - 1;
        for (int k = first; k < end; ++k) {
            const float lower = raw[k > 0 ? k - 1 : k], upper = raw[k < last ? k + 1 : k];
            gainDb[k] = 0.25f * (lower + upper) + 0.5f * raw[k];
        }
        if (!frameQuiet_) {
            for (int k = first; k < end; ++k) gainLin[k] = gainDb[k] == 0.f ? 1.f : expDbToGain(gainDb[k]);
        }
        if (single_ && frameChannels_ == 2) {
            std::copy(gainDb + first, gainDb + end, gainDb_[1].begin() + first);
            if (!frameQuiet_) std::copy(gainLin + first, gainLin + end, gainLin_[1].begin() + first);
        }
    }

    // A step of a main channel's synthesis: the first gains the bins, then the inverse transform, the last
    // adds the frame into the accumulator through the synthesis window (its first sample is heard a hop on).
    void synthesise(int c, int part) {
        Complex* spec = spec_[c].data();
        if (part == 0) {
            const float* gain = gainLin_[c].data();
            const int nyquist = bins_ - 1;
            spec[0] = {spec[0].real() * gain[0], spec[0].imag() * gain[nyquist]};
            for (int k = 1; k < nyquist; ++k) spec[k] *= gain[k];
        }
        float* frame = frameBuffer_.data();
        fft_.ifft(static_cast<size_t>(part), spec, frame);
        if (part == fftSteps_ - 1) {
            float* accum = accum_[c].data();
            const float* syn = synthesis_.data();
            const int first = std::min(frame_, ring_ - frameOut_);
            for (int i = 0; i < first; ++i) accum[frameOut_ + i] += frame[i] * syn[i];
            for (int i = first; i < frame_; ++i) accum[i - first] += frame[i] * syn[i];
        }
    }

    // --- Displays --------------------------------------------------------------------------

    void buildDisplayTable() {
        const double perBin = frame_ / sampleRate_;  // bins per hertz
        const double halfStep = 0.5 / (kPoints - 1);
        const double span = spectral::kDisplayHighHz / spectral::kDisplayLowHz;
        const int nyquist = bins_ - 1;
        for (int j = 0; j < kPoints; ++j) {
            DisplayPoint& p = displayPoints_[static_cast<size_t>(j)];
            const double f = spectral::displayFrequency(j);
            p = DisplayPoint{};
            p.pinkDb = static_cast<float>(spectral::pinkDb(f)) + calibration_;
            if (f >= 0.5 * sampleRate_) continue;  // above Nyquist
            const double b = f * perBin;
            const int lo = static_cast<int>(std::ceil(f * std::pow(span, -halfStep) * perBin));
            const int hi = std::min(nyquist, static_cast<int>(std::floor(f * std::pow(span, halfStep) * perBin)));
            if (hi - lo >= 1) {
                p.kind = DisplayPoint::Range;
                p.k0 = lo;
                p.k1 = hi;
                p.invCount = 1.f / static_cast<float>(hi - lo + 1);
            } else {
                p.kind = DisplayPoint::Interpolate;
                p.k0 = std::min(static_cast<int>(std::floor(b)), nyquist - 1);
                p.k1 = p.k0 + 1;
                p.t = static_cast<float>(std::clamp(b - p.k0, 0.0, 1.0));
            }
        }
    }

    // A power spectrum's display values: each point's mean power (or between two bins), pink-referenced dB.
    void showSpectrum(const float* power, float* out) const {
        for (int j = 0; j < kPoints; ++j) {
            const DisplayPoint& p = displayPoints_[static_cast<size_t>(j)];
            float value = 0.f;
            if (p.kind == DisplayPoint::AboveNyquist) {
                out[j] = kFloorDb;
                continue;
            }
            if (p.kind == DisplayPoint::Range) {
                for (int k = p.k0; k <= p.k1; ++k) value += power[k];
                value *= p.invCount;
            } else {
                value = power[p.k0] + p.t * (power[p.k1] - power[p.k0]);
            }
            const float db = 10.f * std::log10(value + 1e-30f) + p.pinkDb;
            out[j] = db > kFloorDb ? std::min(db, kTopDb) : kFloorDb;  // (an overflowed power's NaN: the floor)
        }
    }

    // Levels (dB) as display values: each point's highest (or between two bins), floored, and held to the top as the
    // spectra are (an absurd level whose power didn't quite overflow reads far over it).
    void showLevels(const float* level, float* out) const {
        for (int j = 0; j < kPoints; ++j) {
            const DisplayPoint& p = displayPoints_[static_cast<size_t>(j)];
            float value = kFloorDb;
            if (p.kind == DisplayPoint::Range) {
                for (int k = p.k0; k <= p.k1; ++k) value = std::max(value, level[k]);
            } else if (p.kind == DisplayPoint::Interpolate) {
                value = level[p.k0] + p.t * (level[p.k1] - level[p.k0]);
            }
            out[j] = std::clamp(value, kFloorDb, kTopDb);
        }
    }

    // Gains (dB) as display values: each point's deepest cut if any bin there is cut, else its biggest lift.
    void showGains(const float* gain, float* out) const {
        for (int j = 0; j < kPoints; ++j) {
            const DisplayPoint& p = displayPoints_[static_cast<size_t>(j)];
            float value = 0.f;
            if (p.kind == DisplayPoint::Range) {
                float lowest = gain[p.k0], highest = gain[p.k0];
                for (int k = p.k0 + 1; k <= p.k1; ++k) {
                    lowest = std::min(lowest, gain[k]);
                    highest = std::max(highest, gain[k]);
                }
                value = lowest < 0.f ? lowest : highest;
            } else if (p.kind == DisplayPoint::Interpolate) {
                value = gain[p.k0] + p.t * (gain[p.k1] - gain[p.k0]);
            }
            out[j] = value;
        }
    }

    // A part of the frame's display values, into its slot (published when the frame is heard): per bin, a part
    // at a time, what the channels show together; then the levels compared and the gains as display points;
    // then the spectra in and out.
    void display(int part) {
        auto& slot = displayRing_[static_cast<size_t>(frameSlot_)];
        if (part < parts_) {
            const auto [first, end] = binsOf(part);
            if (sets_ == 2) {  // the louder channel's level; the deeper cut (else the bigger lift)
                for (int k = first; k < end; ++k) {
                    const auto i = static_cast<size_t>(k);
                    shownKey_[i] = std::max(level_[0][i], level_[1][i]);
                    const float a = gainDb_[0][i], c = gainDb_[1][i];
                    const float lower = std::min(a, c);
                    shownGain_[i] = lower < 0.f ? lower : std::max(a, c);
                }
            }
            if (frameQuiet_) return;
            // In: the louder channel's power. Out: each channel's power through what is heard of the gain
            // (Delta, Dry/Wet and Output as the frame's parameters set them), the louder.
            const Params& p = paramRing_[static_cast<size_t>(frameParams_)];
            const float d = automationSwitchOn(p[Delta]) ? 1.f : 0.f, m = p[Mix] / 100.f, o = expDbToGain(p[Output]);
            const float o2 = o * o;
            for (int k = first; k < end; ++k) {
                const auto i = static_cast<size_t>(k);
                float in = 0.f, out = 0.f;
                for (int c = 0; c < frameChannels_; ++c) {
                    const float power = power_[c][i];
                    const float f = (1.f - d) + (1.f - 2.f * d) * m * (gainLin_[c][i] - 1.f);
                    in = std::max(in, power);
                    out = std::max(out, power * f * f * o2);
                }
                shownInput_[i] = in;
                shownOutput_[i] = out;
            }
        } else if (part == parts_) {
            showLevels(sets_ == 2 ? shownKey_.data() : level_[0].data(), slot[KeyDisplay].data());
            showGains(sets_ == 2 ? shownGain_.data() : gainDb_[0].data(), slot[GainDisplay].data());
        } else if (frameQuiet_) {
            std::fill(slot[InputDisplay].begin(), slot[InputDisplay].end(), kFloorDb);
            std::fill(slot[OutputDisplay].begin(), slot[OutputDisplay].end(), kFloorDb);
        } else {
            showSpectrum(shownInput_.data(), slot[InputDisplay].data());
            showSpectrum(shownOutput_.data(), slot[OutputDisplay].data());
        }
    }

    void clearChannel(int c) {
        std::fill(inRing_[c].begin(), inRing_[c].end(), 0.f);
        std::fill(accum_[c].begin(), accum_[c].end(), 0.f);
        std::fill(power_[c].begin(), power_[c].end(), 0.f);
        std::fill(gainDb_[c].begin(), gainDb_[c].end(), 0.f);
        std::fill(gainLin_[c].begin(), gainLin_[c].end(), 1.f);
    }

    static const std::vector<ParamInfo>& infos() {
        static const std::vector<ParamInfo> kInfos = {
            {"threshold", "Threshold", "dB", -72.f, 12.f, -18.f},
            {"ratio", "Ratio", ":1", 1.f, 20.f, 2.f, true},
            {"below", "Below", "dB", -72.f, 12.f, -48.f},
            {"upward", "Upward", ":1", 1.f, 10.f, 1.f, true},
            {"tilt", "Tilt", "dB/oct", -6.f, 6.f, 0.f},
            {"knee", "Knee", "dB", 0.f, 24.f, 6.f},
            {"range", "Range", "dB", 0.f, 48.f, 24.f},
            {"smooth", "Smoothing", "%", 0.f, 100.f, 40.f},
            {"focus_lo", "Focus Low", "Hz", 20.f, 20000.f, 20.f, true},
            {"focus_hi", "Focus High", "Hz", 20.f, 20000.f, 20000.f, true},
            {"attack", "Attack", "ms", 1.f, 1000.f, 20.f, true},
            {"release", "Release", "ms", 10.f, 5000.f, 150.f, true},
            {"link", "Stereo Link", "%", 0.f, 100.f, 100.f},
            {"mix", "Dry/Wet", "%", 0.f, 100.f, 100.f},
            {"output", "Output", "dB", -24.f, 24.f, 0.f},
            {"delta", "Delta", "", 0.f, 1.f, 0.f, false, offOnLabels()},
        };
        return kInfos;
    }

    double sampleRate_ = 48000.0;
    int frame_ = 0, hop_ = 0, latency_ = 0, bins_ = 0;  // N, N/4, N + N/4, N/2 + 1 (0 until prepared)
    int ring_ = 0, ringMask_ = 0;                       // 2N, 2N - 1
    int parts_ = 1;                                     // the parts a loop over the bins is run in
    int reportedLatency_ = 0;
    int channels_ = 0;

    signalsmith::linear::RealFFT<float, true> fft_;  // computed in steps
    int fftSteps_ = 1;
    std::vector<float> window_, synthesis_, frameBuffer_;
    std::vector<float> inRing_[kChannels], keyRing_[kChannels], accum_[kChannels];
    std::vector<Complex> spec_[kChannels], keySpec_;
    std::vector<float> power_[kChannels], keyPower_[kChannels];
    std::vector<float> env_[kChannels];    // the envelopes: magnitudes (uncalibrated), at least kTinyMagnitude
    std::vector<float> level_[kChannels];  // what the gain computer compares: pink-referenced dB, linked
    std::vector<float> gainDb_[kChannels], gainLin_[kChannels];
    std::vector<float> scratch_;
    std::vector<double> prefix_;
    std::vector<int> boxLo_, boxHi_;
    std::vector<float> boxInv_;
    double boxHalf_ = 1.0;  // half Smoothing's band as a frequency ratio
    bool boxWide_ = false;
    std::vector<float> oct_, pinkCal_, focusOct_, focus_, floorLevel_;
    float calibration_ = 0.f;

    int write_ = 0;     // the ring slot the next sample goes into
    int hopCount_ = 0;  // samples since the last frame went in
    int quiet_ = 0;     // exact zeros in and in the key since the last sound, up to N
    bool keyed_ = false;
    bool fresh_ = true;  // reset, and nothing rendered since

    // The frame in flight: its steps, and what it is.
    std::array<Step, kMaxSteps> steps_{};
    int stepCount_ = 0, stepsDone_ = 0;
    int frameIn_ = 0, frameOut_ = 0;  // ring slots: its oldest sample's, and where its first output sample goes
    int frameParams_ = 0;             // its parameters' snapshot (two frames before it)
    int frameSlot_ = 0;               // its display slot
    int frameChannels_ = 0, detectors_ = 0, sets_ = 1;
    bool frameQuiet_ = true, frameKeyed_ = false, single_ = true;
    bool resting_ = false;  // silent, and every envelope at its floor

    double glide_ = 0.3;
    double threshold_ = 0.0, below_ = 0.0, tilt_ = 0.0, knee_ = 0.0, range_ = 0.0, down_ = 0.0, up_ = 0.0, link_ = 1.0;
    double focusLo_ = 0.0, focusHi_ = 0.0;  // the Focus edges, in octaves from 1 kHz
    double focusFor_[2] = {NAN, NAN};       // the edges the Focus weights were worked out for
    float boxFor_ = -1.f;                   // the Smoothing the boxes were worked out for
    float attackFor_ = -1.f, releaseFor_ = -1.f;
    float attackCoeff_ = 0.f, releaseCoeff_ = 0.f;
    SmoothedValue mix_, output_, delta_;
    std::vector<float> mixRing_, outputRing_, deltaRing_;  // their targets, on their way to the output
    std::array<Params, kSlots> paramRing_{};               // the parameters at each of the last three frames

    std::array<DisplayPoint, kPoints> displayPoints_{};
    std::array<std::array<std::array<float, kPoints>, kSpectralStreams>, kSlots> displayRing_{};
    std::vector<float> shownInput_, shownKey_, shownOutput_, shownGain_;
    int slot_ = 0;  // the slot the next frame takes: its parameters' snapshot, its displays
    float peakIn_ = 0.f, peakOut_ = 0.f;
};

}  // namespace

SUB_REGISTER_BUILTIN(SpectralProcessor, AudioEffect);

}  // namespace sub
