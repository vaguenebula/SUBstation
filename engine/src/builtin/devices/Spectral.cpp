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
// the editor shares (frame size, calibration, curves, gain computer) is in
// builtin/SpectralDesign.h.
//
// - The STFT: a periodic Hann window of N samples (frameSize(): 2048 at 44.1
//   and 48 kHz, scaled with the rate so a bin is about 23 Hz wide), a hop of
//   N/4, the same window again on the way out (Hann² at 4x overlap sums to
//   1.5). Input goes into a ring of N samples; every hop the ring is
//   transformed, gained, transformed back and added into an accumulator that
//   is read (and cleared) N samples later: the latency is N, exactly. The dry
//   path is the sample the input ring is about to overwrite, so Dry/Wet blends
//   in time with no delay line of its own, and at 0 % passes the input,
//   delayed, bit for bit.
// - Levels: each bin's power, its mean over a band constant in octaves
//   (Smoothing: 0 to 2 octaves; prefix sums), then an envelope in linear power
//   per bin and hop (Attack and Release as one-poles at the hop rate), then
//   dB calibrated and pink-referenced (calibrationDb(), pinkDb()). Stereo Link
//   blends each channel's level towards the louder one's in dB.
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
//   the output N samples on, with the input they came with (rings, as the dry
//   path's).
// - A frame whose input (and key) is all exact zeros skips its transforms: its
//   envelopes release, and nothing is added to the output. So silence costs
//   little and comes out as exact zeros 2N after the last sound.
// - Displays: per hop, 128 values (log-spaced 20 Hz..20 kHz) of the input
//   spectrum, the levels compared, the output spectrum and the gains, published
//   two hops late so they are in step with what is heard; and the In and Out
//   peaks over the hop.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <string>
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

constexpr int kChannels = 2;            // a sidechain is always stereo; the main input up to two
constexpr float kTinyPower = 1e-12f;    // the envelopes' floor (uncalibrated power: about -154 dB)
constexpr float kMeterFloorDb = -90.f;  // in_level and out_level
constexpr int kPoints = spectral::kDisplayPoints;
constexpr int kSpectralStreams = 4;  // input, key, output, gain
constexpr int kDisplaySlots = 3;     // frames waiting: the one written and the two before it
constexpr float kFloorDb = static_cast<float>(spectral::kFloorDb);
constexpr float kUpwardFloorDb = static_cast<float>(spectral::kUpwardFloorDb);
constexpr float kInvUpwardFadeDb = static_cast<float>(1.0 / spectral::kUpwardFadeDb);
constexpr float kInvFocusEdge = static_cast<float>(1.0 / spectral::kFocusEdgeOctaves);
constexpr double kLinkedFully = 0.9999;  // Stereo Link from here: one set of gains for both channels
constexpr double kLanded = 1e-7;         // a glide this close to its target lands on it

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

    // The frame: what goes in comes out N samples later, and the last frames to hear a sound end N after it.
    int latencySamples() const override { return frame_; }
    int tailSamples() const override { return frame_; }

    // The latency changes only with the sample rate (in prepare(), with audio stopped).
    bool idle() override {
        if (frame_ == reportedLatency_) return false;
        reportedLatency_ = frame_;
        return true;
    }

    void prepare(double sampleRate, int) override {
        sampleRate_ = sampleRate;
        frame_ = spectral::frameSize(sampleRate);
        hop_ = frame_ / spectral::kOverlap;
        bins_ = frame_ / 2 + 1;
        mask_ = frame_ - 1;
        const auto n = static_cast<size_t>(frame_), b = static_cast<size_t>(bins_);

        fft_.resize(n);
        window_.assign(n, 0.f);
        synthesis_.assign(n, 0.f);
        for (size_t i = 0; i < n; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / frame_);
            window_[i] = static_cast<float>(w);
            synthesis_[i] = static_cast<float>(w / (1.5 * frame_));  // Hann² sums to 1.5; the inverse gives N times
        }
        frameBuffer_.assign(n, 0.f);
        keySpec_.assign(n / 2, Complex{});
        mixRing_.assign(n, 0.f);
        outputRing_.assign(n, 1.f);
        deltaRing_.assign(n, 0.f);
        for (int c = 0; c < kChannels; ++c) {
            inRing_[c].assign(n, 0.f);
            keyRing_[c].assign(n, 0.f);
            accum_[c].assign(n, 0.f);
            spec_[c].assign(n / 2, Complex{});
            power_[c].assign(b, 0.f);
            keyPower_[c].assign(b, 0.f);
            env_[c].assign(b, kTinyPower);
            level_[c].assign(b, 0.f);
            gainDb_[c].assign(b, 0.f);
            gainLin_[c].assign(b, 1.f);
        }
        detect_.assign(b, 0.f);
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
        const float tinyDb = 10.f * std::log10(kTinyPower);
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
        reportedLatency_ = frame_;
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
            std::fill(env_[c].begin(), env_[c].end(), kTinyPower);
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
        quiet_ = frame_;  // it starts silent
        peakIn_ = peakOut_ = 0.f;
        keyed_ = false;
        snapToParams();
        fresh_ =
            true;  // and again at the first stretch: what is set before it (a project loading) holds from the start
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
        // What the output's controls ask for now reaches the output with the input it came with, N samples on
        // (automation stays with the audio, as the engine keeps it with the device's input).
        const float mixNow = param(Mix) / 100.f, outputNow = expDbToGain(param(Output));
        const float deltaNow = isOn(Delta) ? 1.f : 0.f;

        float* in[kChannels] = {inRing_[0].data(), inRing_[1].data()};
        float* mixes = mixRing_.data();
        float* outputs = outputRing_.data();
        float* deltas = deltaRing_.data();
        float* acc[kChannels] = {accum_[0].data(), accum_[1].data()};
        float* keys[kChannels] = {keyRing_[0].data(), keyRing_[1].data()};
        for (int i = 0; i < numFrames;) {
            const int run = std::min(numFrames - i, hop_ - hopCount_);
            for (const int end = i + run; i < end; ++i) {
                const int w = write_;
                float dry[kChannels] = {}, wet[kChannels] = {};
                bool silent = true;
                for (int c = 0; c < n; ++c) {
                    const float x = ch[c][i];
                    dry[c] = in[c][w];  // the input N samples ago: in step with the wet
                    in[c][w] = x;
                    wet[c] = acc[c][w];
                    acc[c][w] = 0.f;
                    silent = silent && x == 0.f;
                }
                const float kl = keyL ? keyL[i] : 0.f, kr = keyR ? keyR[i] : 0.f;
                keys[0][w] = kl;
                keys[1][w] = kr;
                silent = silent && kl == 0.f && kr == 0.f;
                quiet_ = silent ? std::min(quiet_ + 1, frame_) : 0;
                mix_.setTarget(mixes[w]);
                output_.setTarget(outputs[w]);
                delta_.setTarget(deltas[w]);
                mixes[w] = mixNow;
                outputs[w] = outputNow;
                deltas[w] = deltaNow;
                write_ = (w + 1) & mask_;

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
            if (hopCount_ == hop_) {
                hopCount_ = 0;
                processFrame();
            }
        }
    }

private:
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
        focusFor_[0] = focusFor_[1] = NAN;  // worked out again at the next frame
        boxFor_ = -1.f;
        attackFor_ = releaseFor_ = -1.f;
        mix_.snapTo(param(Mix) / 100.f);
        output_.snapTo(expDbToGain(param(Output)));
        delta_.snapTo(isOn(Delta) ? 1.f : 0.f);
    }

    // --- Per hop ---------------------------------------------------------------------------

    void processFrame() {
        // The parameters now, kept for two frames on; this frame works with those of two frames ago, when its
        // centre went in (so a change meets the audio it came with, as the displays do).
        auto& now = paramRing_[static_cast<size_t>(slot_)];
        for (int i = 0; i < NumParams; ++i) now[static_cast<size_t>(i)] = param(i);
        const Params& p = paramRing_[static_cast<size_t>(slot_ == kDisplaySlots - 1 ? 0 : slot_ + 1)];
        updateControls(p);
        const bool quiet = quiet_ >= frame_;  // the frame's inputs and keys are all exact zeros
        const int detectors = keyed_ ? kChannels : channels_;
        const auto b = static_cast<size_t>(bins_);

        if (!quiet) {
            for (int c = 0; c < channels_; ++c) analyse(inRing_[c].data(), spec_[c].data(), power_[c].data());
            if (keyed_) {
                for (int c = 0; c < kChannels; ++c) analyse(keyRing_[c].data(), keySpec_.data(), keyPower_[c].data());
            }
        }

        // The envelopes, per detector channel (every one runs whatever the link, so turning it finds them current).
        bool resting = true;
        for (int c = 0; c < detectors; ++c) {
            float* env = env_[c].data();
            if (quiet) {
                // Silence: its level falls with the release.
                const float release = releaseCoeff_;
                for (size_t k = 0; k < b; ++k) {
                    const float e = release * env[k];
                    env[k] = e > kTinyPower ? e : kTinyPower;
                    resting = resting && env[k] == kTinyPower;
                }
            } else {
                resting = false;
                const float* detect = smoothAcross((keyed_ ? keyPower_[c] : power_[c]).data());
                const float attack = attackCoeff_, release = releaseCoeff_;
                for (size_t k = 0; k < b; ++k) {
                    const float x = detect[k], e = env[k];
                    const float next = x + (x > e ? attack : release) * (e - x);
                    env[k] = next > kTinyPower ? next : kTinyPower;  // (a NaN in the input lands here too)
                }
            }
        }

        // Levels in pink-referenced dB, then the gains. One set when there is one detector, one main channel
        // (keyed: the louder key channel's) or the link is full; else each channel's level, blended in dB
        // towards the louder one's by Stereo Link.
        const bool single = detectors == 1 || channels_ == 1 || link_ >= kLinkedFully;
        const int sets = single ? 1 : 2;
        if (resting) {
            for (int c = 0; c < sets; ++c) std::copy(floorLevel_.begin(), floorLevel_.end(), level_[c].begin());
        } else if (single) {
            float* level = level_[0].data();
            const float* e0 = env_[0].data();
            const float* e1 = env_[detectors - 1].data();
            for (size_t k = 0; k < b; ++k) level[k] = 10.f * std::log10(std::max(e0[k], e1[k])) + pinkCal_[k];
        } else {
            float* l0 = level_[0].data();
            float* l1 = level_[1].data();
            const auto link = static_cast<float>(link_);
            for (size_t k = 0; k < b; ++k) {
                const float a = 10.f * std::log10(env_[0][k]) + pinkCal_[k];
                const float c = 10.f * std::log10(env_[1][k]) + pinkCal_[k];
                const float loudest = std::max(a, c);
                l0[k] = a + link * (loudest - a);
                l1[k] = c + link * (loudest - c);
            }
        }
        for (int c = 0; c < sets; ++c) computeGains(level_[c].data(), gainDb_[c].data(), gainLin_[c].data(), !quiet);
        if (single && channels_ == 2) {
            std::copy(gainDb_[0].begin(), gainDb_[0].end(), gainDb_[1].begin());
            if (!quiet) std::copy(gainLin_[0].begin(), gainLin_[0].end(), gainLin_[1].begin());
        }

        // Apply and add back.
        if (!quiet) {
            for (int c = 0; c < channels_; ++c) synthesise(spec_[c].data(), gainLin_[c].data(), accum_[c].data());
        }

        publishDisplays(p, quiet, single);
    }

    // Glides the curves' controls one hop on towards `p`, and works out what depends on the others when they
    // change.
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
        if (focusLo_ != focusFor_[0] || focusHi_ != focusFor_[1]) {
            focusFor_[0] = focusLo_;
            focusFor_[1] = focusHi_;
            const auto lo = static_cast<float>(focusLo_), hi = static_cast<float>(focusHi_);
            for (int k = 0; k < bins_; ++k) {
                const float o = focusOct_[static_cast<size_t>(k)];
                const float lower = std::clamp((o - lo) * kInvFocusEdge + 1.f, 0.f, 1.f);
                const float upper = std::clamp((hi - o) * kInvFocusEdge + 1.f, 0.f, 1.f);
                focus_[static_cast<size_t>(k)] = lower * upper;
            }
        }
        const float smooth = p[Smooth];
        if (smooth != boxFor_) {
            boxFor_ = smooth;
            const double half = std::exp2(0.5 * spectral::smoothingOctaves(smooth));
            const int top = bins_ - 1;
            for (int k = 0; k < bins_; ++k) {
                const int lo = std::clamp(static_cast<int>(std::lround(k / half)), 0, k);
                const int hi = std::clamp(static_cast<int>(std::lround(k * half)), k, top);
                boxLo_[static_cast<size_t>(k)] = lo;
                boxHi_[static_cast<size_t>(k)] = hi;
                boxInv_[static_cast<size_t>(k)] = 1.f / static_cast<float>(hi - lo + 1);
            }
            boxWide_ = smooth > 0.f;
        }
    }

    // Windows the ring (oldest first), transforms it, and unpacks each bin's power |X|².
    void analyse(const float* ring, Complex* spec, float* power) {
        const int first = frame_ - write_;  // from the oldest sample to the ring's end
        const float* win = window_.data();
        float* frame = frameBuffer_.data();
        for (int i = 0; i < first; ++i) frame[i] = ring[write_ + i] * win[i];
        for (int i = first; i < frame_; ++i) frame[i] = ring[i - first] * win[i];
        fft_.fft(frame, spec);
        const int nyquist = bins_ - 1;
        power[0] = spec[0].real() * spec[0].real();  // DC and Nyquist come packed in the first bin
        power[nyquist] = spec[0].imag() * spec[0].imag();
        for (int k = 1; k < nyquist; ++k) power[k] = std::norm(spec[k]);
    }

    // Smoothing: each bin's mean power over its band (a box constant in octaves); the power itself at 0 %.
    const float* smoothAcross(const float* power) {
        if (!boxWide_) return power;
        double sum = 0.0;
        prefix_[0] = 0.0;
        for (int k = 0; k < bins_; ++k) prefix_[static_cast<size_t>(k) + 1] = sum += power[k];
        float* detect = detect_.data();
        for (int k = 0; k < bins_; ++k) {
            const auto i = static_cast<size_t>(k);
            const auto mean = static_cast<float>(prefix_[static_cast<size_t>(boxHi_[i]) + 1] -
                                                 prefix_[static_cast<size_t>(boxLo_[i])]);
            detect[k] = std::max(0.f, mean * boxInv_[i]);
        }
        return detect;
    }

    // The gain computer per bin (spectral::gainDb() in float, times the Focus weight), smoothed once across
    // bins; then, if `linear`, as gains to multiply by.
    void computeGains(const float* level, float* gainDb, float* gainLin, bool linear) {
        const auto threshold = static_cast<float>(threshold_), below = static_cast<float>(std::min(below_, threshold_));
        const auto tilt = static_cast<float>(tilt_), down = static_cast<float>(down_), up = static_cast<float>(up_);
        const auto range = static_cast<float>(range_);
        const KneeCurve knee(static_cast<float>(knee_));
        float* raw = scratch_.data();
        const int last = bins_ - 1;
        for (int k = 0; k <= last; ++k) {
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
        // [1 2 1] / 4 across bins (the ends take themselves for the missing neighbour): a raised cosine over the
        // gain's impulse response, so a jagged gain smears less in time. It stays within ±Range.
        gainDb[0] = 0.75f * raw[0] + 0.25f * raw[1];
        for (int k = 1; k < last; ++k) gainDb[k] = 0.25f * (raw[k - 1] + raw[k + 1]) + 0.5f * raw[k];
        gainDb[last] = 0.75f * raw[last] + 0.25f * raw[last - 1];
        if (!linear) return;
        for (int k = 0; k <= last; ++k) gainLin[k] = gainDb[k] == 0.f ? 1.f : expDbToGain(gainDb[k]);
    }

    // Gains the bins, transforms back and adds the frame into the accumulator, through the synthesis window.
    void synthesise(Complex* spec, const float* gain, float* accum) {
        const int nyquist = bins_ - 1;
        spec[0] = {spec[0].real() * gain[0], spec[0].imag() * gain[nyquist]};
        for (int k = 1; k < nyquist; ++k) spec[k] *= gain[k];
        float* frame = frameBuffer_.data();
        fft_.ifft(spec, frame);
        const float* syn = synthesis_.data();
        const int first = frame_ - write_;
        for (int i = 0; i < first; ++i) accum[write_ + i] += frame[i] * syn[i];
        for (int i = first; i < frame_; ++i) accum[i - first] += frame[i] * syn[i];
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
            out[j] = std::max(kFloorDb, 10.f * std::log10(value + 1e-30f) + p.pinkDb);
        }
    }

    // Levels (dB) as display values: each point's highest (or between two bins), floored.
    void showLevels(const float* level, float* out) const {
        for (int j = 0; j < kPoints; ++j) {
            const DisplayPoint& p = displayPoints_[static_cast<size_t>(j)];
            float value = kFloorDb;
            if (p.kind == DisplayPoint::Range) {
                for (int k = p.k0; k <= p.k1; ++k) value = std::max(value, level[k]);
            } else if (p.kind == DisplayPoint::Interpolate) {
                value = level[p.k0] + p.t * (level[p.k1] - level[p.k0]);
            }
            out[j] = std::max(kFloorDb, value);
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

    // Fills this frame's display slot, publishes the slot from two frames ago (in step with what is heard
    // now), and the hop's In and Out peaks.
    void publishDisplays(const Params& p, bool quiet, bool single) {
        auto& slot = displayRing_[static_cast<size_t>(slot_)];
        const auto b = static_cast<size_t>(bins_);
        const int gainSets = single ? 1 : channels_;
        const int levelSets = single ? 1 : channels_;

        // The levels compared, and the gains: per bin the louder channel's level, the deeper cut (else the
        // bigger lift).
        const float* key = level_[0].data();
        const float* gain = gainDb_[0].data();
        if (levelSets == 2) {
            for (size_t k = 0; k < b; ++k) shownKey_[k] = std::max(level_[0][k], level_[1][k]);
            key = shownKey_.data();
        }
        if (gainSets == 2) {
            for (size_t k = 0; k < b; ++k) {
                const float a = gainDb_[0][k], c = gainDb_[1][k];
                const float lower = std::min(a, c);
                shownGain_[k] = lower < 0.f ? lower : std::max(a, c);
            }
            gain = shownGain_.data();
        }
        showLevels(key, slot[KeyDisplay].data());
        showGains(gain, slot[GainDisplay].data());

        if (quiet) {
            std::fill(slot[InputDisplay].begin(), slot[InputDisplay].end(), kFloorDb);
            std::fill(slot[OutputDisplay].begin(), slot[OutputDisplay].end(), kFloorDb);
        } else {
            // In: the louder channel's power. Out: each channel's power through what is heard of the gain
            // (Delta, Dry/Wet and Output as the frame's parameters set them), the louder.
            const float d = automationSwitchOn(p[Delta]) ? 1.f : 0.f, m = p[Mix] / 100.f, o = expDbToGain(p[Output]);
            const float o2 = o * o;
            for (size_t k = 0; k < b; ++k) {
                float in = 0.f, out = 0.f;
                for (int c = 0; c < channels_; ++c) {
                    const float power = power_[c][k];
                    const float f = (1.f - d) + (1.f - 2.f * d) * m * (gainLin_[c][k] - 1.f);
                    in = std::max(in, power);
                    out = std::max(out, power * f * f * o2);
                }
                shownInput_[k] = in;
                shownOutput_[k] = out;
            }
            showSpectrum(shownInput_.data(), slot[InputDisplay].data());
            showSpectrum(shownOutput_.data(), slot[OutputDisplay].data());
        }

        slot_ = slot_ == kDisplaySlots - 1 ? 0 : slot_ + 1;  // the oldest: two frames ago
        const auto& heard = displayRing_[static_cast<size_t>(slot_)];
        for (int s = 0; s < kSpectralStreams; ++s) {
            for (const float v : heard[static_cast<size_t>(s)]) publish(s, v);
        }
        publish(InLevel, std::max(kMeterFloorDb, gainToDb(peakIn_)));
        publish(OutLevel, std::max(kMeterFloorDb, gainToDb(peakOut_)));
        peakIn_ = peakOut_ = 0.f;
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
            {"threshold", "Threshold", "dB", -72.f, 12.f, -24.f},
            {"ratio", "Ratio", ":1", 1.f, 20.f, 3.f, true},
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
    int frame_ = 0, hop_ = 0, bins_ = 0, mask_ = 0;  // N, N/4, N/2 + 1, N - 1 (0 until prepared)
    int reportedLatency_ = 0;
    int channels_ = 0;

    signalsmith::linear::RealFFT<float> fft_;
    std::vector<float> window_, synthesis_, frameBuffer_;
    std::vector<float> inRing_[kChannels], keyRing_[kChannels], accum_[kChannels];
    std::vector<Complex> spec_[kChannels], keySpec_;
    std::vector<float> power_[kChannels], keyPower_[kChannels];
    std::vector<float> env_[kChannels];    // the envelopes: linear power (uncalibrated), at least kTinyPower
    std::vector<float> level_[kChannels];  // what the gain computer compares: pink-referenced dB, linked
    std::vector<float> gainDb_[kChannels], gainLin_[kChannels];
    std::vector<float> detect_, scratch_;
    std::vector<double> prefix_;
    std::vector<int> boxLo_, boxHi_;
    std::vector<float> boxInv_;
    bool boxWide_ = false;
    std::vector<float> oct_, pinkCal_, focusOct_, focus_, floorLevel_;
    float calibration_ = 0.f;

    int write_ = 0;     // the ring slot the next sample goes into (the oldest sample's)
    int hopCount_ = 0;  // samples since the last frame
    int quiet_ = 0;     // exact zeros in and in the key since the last sound, up to N
    bool keyed_ = false;
    bool fresh_ = true;  // reset, and nothing rendered since

    double glide_ = 0.3;
    double threshold_ = 0.0, below_ = 0.0, tilt_ = 0.0, knee_ = 0.0, range_ = 0.0, down_ = 0.0, up_ = 0.0, link_ = 1.0;
    double focusLo_ = 0.0, focusHi_ = 0.0;  // the Focus edges, in octaves from 1 kHz
    double focusFor_[2] = {NAN, NAN};       // the edges the Focus weights were worked out for
    float boxFor_ = -1.f;                   // the Smoothing the boxes were worked out for
    float attackFor_ = -1.f, releaseFor_ = -1.f;
    float attackCoeff_ = 0.f, releaseCoeff_ = 0.f;
    SmoothedValue mix_, output_, delta_;
    std::vector<float> mixRing_, outputRing_, deltaRing_;  // their targets, on their way to the output (N samples)
    std::array<Params, kDisplaySlots> paramRing_{};        // the parameters at each of the last three frames

    std::array<DisplayPoint, kPoints> displayPoints_{};
    std::array<std::array<std::array<float, kPoints>, kSpectralStreams>, kDisplaySlots> displayRing_{};
    std::vector<float> shownInput_, shownKey_, shownOutput_, shownGain_;
    int slot_ = 0;  // the display slot this frame fills
    float peakIn_ = 0.f, peakOut_ = 0.f;
};

}  // namespace

SUB_REGISTER_BUILTIN(SpectralProcessor, AudioEffect);

}  // namespace sub
