// Built-in "Multiband Dynamics" device, after Ableton's: three bands, each
// with an Above and a Below threshold and a ratio for each, so one band can be
// compressed or expanded downwards and upwards at once (MultibandDesign.h has
// the gain law). Over The Top is one preset of it.
//
// - The split: Linkwitz-Riley crossovers (dsp::Crossover) at Low-Mid and
//   Mid-High, the low band through the upper crossover's all-pass, so the bands
//   add up to an all-pass (flat) whatever their gains are when equal. The dry
//   signal is never mixed in: at 1:1 the output is the bands summed. A Low-Mid
//   at or above the Mid-High splits both there (the mid band narrows to a band
//   around it, and the sum stays flat).
// - High and Low, the split switches (`high_on`, `low_on`; not automatable, as
//   Live's), switch the outer bands off as Ableton's do: the split doesn't change
//   (nor the phase), but a band switched off takes the mid band's gain (Input,
//   dynamics, Output) and feeds the mid band's detector, so with both off the
//   mid band shapes the whole spectrum. The switches crossfade the gains (a
//   glide), and every band's detector keeps running, so a band switched back on
//   picks up where its own level is.
// - Each band's activator (`<band>_active`, Live's band buttons) bypasses it:
//   its Input, dynamics and Output glide to unity while its split stays (its
//   frequencies are its own, not another band's to shape). Its detector runs on.
// - Detectors, linked stereo (the louder channel), per band: Peak, the largest
//   value over a window of the band's lowest period (1..25 ms: a steady tone
//   holds its peak, so it isn't distorted); RMS, each channel's mean square
//   through a one-pole of that window or 10 ms, whichever is longer, the larger
//   of the two taken. Peak/RMS crossfades their levels in dB. Input gain drives
//   the detector as it drives the audio.
// - Attack and release as Ableton defines them, for each side on its own: each
//   side's gain change (dB) moves at the attack while it grows and at the
//   release while it shrinks. Time scales both. Amount scales the band's change.
// - Every control glides (dsp::Glide: two one-poles of 5 ms in a row, so a jump
//   eases in and out), crossover frequencies in log, stepped every 32 samples
//   with their coefficients interpolated sample by sample between the steps. The
//   32-sample grid runs across render() calls, so the output doesn't depend on
//   where automation cuts a block.
// - A sidechain keys each band by the same band of the key (its own split, run
//   only while some of the key is heard); S/C Mix blends the key with the
//   device's own input as the trigger. S/C Listen puts the trigger out instead
//   of the bands (the key after S/C Gain, as much of it as S/C Mix takes; without
//   a sidechain, the input). A key sample that isn't audio (NaN, infinity, past
//   BuiltinProcessor::kMaxInput) is taken as silence: unlike the input, the
//   sidechain reaches the device as the engine has it.
// - States are flushed on the 32-sample grid, so silence rings out to exact
//   zeros. No lookahead, no latency; the tail is the crossovers' ringing.
//
// Displays, one value per multiband::kDisplaySamples, per band (Low, Mid, High):
// `in`, the detector's level (dB, after Input); `out`, that level plus the gain
// change the dynamics are applying (the static curve's output once attack and
// release have settled); `gain`, the change itself (dB, signed). A band bypassed
// by its activator shows its level as it is (no Input) and no change.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/Dsp.h"
#include "builtin/DspBlocks.h"
#include "builtin/MultibandDesign.h"
#include "rt/RtUtils.h"

namespace sub {
namespace {

namespace mb = multiband;

constexpr int kChunk = 32;                   // samples per step of the crossovers' glides, and per flush
constexpr double kGlideSeconds = 0.005;      // each of a glide's two one-poles
constexpr double kLanded = 1e-6;             // a glide this close to its target lands there
constexpr float kButterworth = 1.41421356f;  // the crossovers' sections' damping (1 / Q)
constexpr float kLevelFloorDb = -120.f;      // the detectors' floor
// The detectors' squares at most: +100 dBFS, far over any level the bands carry, so the law is untouched. The
// loudest input let in (1e30) squared would be infinite, and an infinite mean square would never come down; held
// here, the RMS detector is back from it within about a second.
constexpr float kMaxSquare = 1e10f;
constexpr float kDbPerLog2 = 6.0206f;  // 20 log10(x) = 6.0206 log2(x)

// A control's value as the loop reads it, gliding to its target (dsp::Glide: a jump eases in and out, within 1 %
// of a step after about 33 ms at 5 ms a pole).
struct Control {
    dsp::Glide glide;
    double target = 0.0;
    float value = 0.f;

    void snap(double to) noexcept {
        target = to;
        glide.snap(to);
        value = static_cast<float>(to);
    }
    bool settled() const noexcept { return glide.settled(target); }
    // A step, `share` of the way per pole (none once landed); false once it has landed.
    bool step(double share) noexcept {
        value = static_cast<float>(glide.next(target, share, kLanded));
        return !settled();
    }
};

// A key's sample, or silence if it isn't audio (BuiltinProcessor::process() sees to the input, not the key).
inline float keySample(float x) noexcept { return std::abs(x) <= BuiltinProcessor::kMaxInput ? x : 0.f; }

// A channel's three-way split. Seven state-variable sections, as Over The Top's.
struct Split {
    dsp::Crossover low, high;
    dsp::CrossoverAllpass lowAllpass;

    void reset() noexcept {
        low.reset();
        high.reset();
        lowAllpass.reset();
    }
    void process(const dsp::CrossoverCoefficients& lowCoeffs, const dsp::CrossoverCoefficients& highCoeffs, float x,
                 float* bands) noexcept {
        float lows, rest;
        low.process(lowCoeffs, x, lows, rest);
        high.process(highCoeffs, rest, bands[mb::Mid], bands[mb::High]);
        bands[mb::Low] = lowAllpass.process(highCoeffs, lows);
    }
    void flush() noexcept {
        low.flush();
        high.flush();
        lowAllpass.flush();
    }
};

class MultibandProcessor final : public BuiltinProcessor {
public:
    enum BandParam { Active = 0, In, Out, Above, AboveRatio, Below, BelowRatio, Attack, Release, Solo, BandParams };
    // Band b's field f is FirstBand + BandParams * b + f.
    enum Param {
        XoverLow = 0,
        XoverHigh,
        LowOn,
        HighOn,
        FirstBand,
        Amount = FirstBand + BandParams * mb::kBands,
        Time,
        Output,
        SoftKnee,
        Mode,
        ScGain,
        ScMix,
        ScListen,
        NumParams
    };
    // Band b's display k is DisplaysPerBand * b + k.
    enum Display { InLevel = 0, OutLevel, GainChange, DisplaysPerBand };

    MultibandProcessor() : BuiltinProcessor(infos(), displayInfos()) {}

    std::string typeId() const override { return "builtin:multiband"; }
    std::string name() const override { return "Multiband Dynamics"; }
    bool hasSidechain() const override { return true; }

    // The crossovers' ringing: 14 time constants (-120 dB) of the slowest
    // Butterworth pole. The dynamics add none.
    int tailSamples() const override {
        const double f = std::max(1.0, static_cast<double>(std::min(param(XoverLow), param(XoverHigh))));
        return static_cast<int>(std::ceil(14.0 * std::sqrt(2.0) / (dsp::kTwoPi * f) * sampleRate_));
    }

    void prepare(double sampleRate, int) override {
        sampleRate_ = sampleRate;
        glide_ = 1.0 - onePoleCoefficient(kGlideSeconds, sampleRate);
        chunkGlide_ = 1.0 - onePoleCoefficient(kGlideSeconds, sampleRate, kChunk);
        const int longest = static_cast<int>(std::ceil(mb::kMaxWindowSeconds * sampleRate)) + 1;
        for (BandState& band : bands_) band.peak.prepare(longest);
        reset();
    }

    // Silent, every glide where the parameters are.
    void reset() override {
        for (Split& s : main_) s.reset();
        for (Split& s : key_) s.reset();
        keyLive_ = false;
        for (BandState& band : bands_) {
            band.peak.reset();
            band.meanSquare[0] = band.meanSquare[1] = band.aboveDb = band.belowDb = 0.f;
            band.clearMeters();
        }
        readParams(true);
        keyOn_.snap(sidechainConnected() ? 1.0 : 0.0);
        snapKey_ = true;
        keyGain_ = expDbToGain(scGain_.value);
        gLowTo_ = crossoverG(std::min(log2Low_.value, log2High_.value));
        gHighTo_ = crossoverG(log2High_.value);
        gLowFrom_ = gLowTo_;
        gHighFrom_ = gHighTo_;
        lowCoeffs_ = crossoverCoefficients(gLowTo_);
        highCoeffs_ = crossoverCoefficients(gHighTo_);
        crossoversMoving_ = false;
        chunkPos_ = 0;
        windowsLog2Low_ = windowsLog2High_ = std::numeric_limits<float>::quiet_NaN();  // (worked out again)
        updateWindows();
        meterCount_ = 0;
    }

protected:
    void render(const ProcessContext&, float* const* ch, int numChannels, int numFrames) override {
        const int n = std::min(numChannels, 2);
        if (n <= 0) return;
        if (n != channels_) {  // a channel that wasn't processed has no history to go on from
            main_[1].reset();
            key_[1].reset();
            for (BandState& band : bands_) band.meanSquare[1] = 0.f;
            channels_ = n;
        }
        readParams(false);
        // Whether a sidechain keys it: glided, but snapped after a reset (which cleared every state,
        // so nothing can click), since the renderer sets the flag after resetting.
        if (snapKey_) {
            keyOn_.snap(sidechainConnected() ? 1.0 : 0.0);
            snapKey_ = false;
        } else {
            keyOn_.target = sidechainConnected() ? 1.0 : 0.0;
        }
        gliding_ = bandsGliding_ = true;  // (until a sample finds their glides landed)
        for (BandState& band : bands_) band.gliding = true;
        // The key (silence while solo leaves its source out).
        const float* key[2] = {sidechain(0), sidechain(1)};
        if (n == 2) {
            renderFrames<2>(ch, numFrames, key);
        } else {
            renderFrames<1>(ch, numFrames, key);
        }
    }

private:
    struct BandState {
        dsp::SlidingMax peak;                // the Peak detector
        float meanSquare[2] = {};            // the RMS detector, per channel (the louder is taken)
        float aboveDb = 0.f, belowDb = 0.f;  // each side's gain change, after attack and release
        Control active, above, below, aboveSlope, belowSlope, inDb, outDb, audible;
        float attack = 0.f, release = 0.f;  // one-pole coefficients, Time applied
        float rms = 0.f;                    // the RMS detector's coefficient
        int window = 1;                     // the Peak detector's, in samples
        bool gliding = true;                // a control above hasn't landed
        float meterIn = mb::kDisplayFloorDb, meterOut = mb::kDisplayFloorDb, meterGain = 0.f;

        void clearMeters() noexcept {
            meterIn = meterOut = mb::kDisplayFloorDb;
            meterGain = 0.f;
        }
    };

    static int bandParam(int band, BandParam field) noexcept { return FirstBand + BandParams * band + field; }

    // The controls' targets (or, snapping, their values) and the per-call coefficients, from the parameters.
    void readParams(bool snap) noexcept {
        const auto set = [snap](Control& control, double value) {
            if (snap) {
                control.snap(value);
            } else {
                control.target = value;
            }
        };
        const bool lowOn = isOn(LowOn), highOn = isOn(HighOn);
        set(lowOn_, lowOn ? 1.0 : 0.0);
        set(highOn_, highOn ? 1.0 : 0.0);
        set(amount_, std::clamp(param(Amount), 0.f, 100.f) / 100.0);
        set(output_, param(Output));
        set(knee_, isOn(SoftKnee) ? mb::kKneeDb : 0.0);
        set(mode_, isOn(Mode) ? 1.0 : 0.0);
        set(scGain_, param(ScGain));
        set(scMix_, std::clamp(param(ScMix), 0.f, 100.f) / 100.0);
        set(listen_, isOn(ScListen) ? 1.0 : 0.0);
        set(log2Low_, std::log2(std::max(1.f, param(XoverLow))));
        set(log2High_, std::log2(std::max(1.f, param(XoverHigh))));

        // Solo: a band switched off follows the mid band's (its sound is the mid band's).
        const bool soloMid = isOn(bandParam(mb::Mid, Solo));
        const bool soloLow = lowOn && isOn(bandParam(mb::Low, Solo));
        const bool soloHigh = highOn && isOn(bandParam(mb::High, Solo));
        const bool anySolo = soloMid || soloLow || soloHigh;
        const bool heard[mb::kBands] = {!anySolo || (lowOn ? soloLow : soloMid), !anySolo || soloMid,
                                        !anySolo || (highOn ? soloHigh : soloMid)};

        const double time = std::clamp(param(Time), 10.f, 1000.f) / 100.0;
        for (int b = 0; b < mb::kBands; ++b) {
            BandState& band = bands_[static_cast<size_t>(b)];
            set(band.active, isOn(bandParam(b, Active)) ? 1.0 : 0.0);
            set(band.above, param(bandParam(b, Above)));
            set(band.below, param(bandParam(b, Below)));
            set(band.aboveSlope, mb::slope(param(bandParam(b, AboveRatio))));
            set(band.belowSlope, mb::slope(param(bandParam(b, BelowRatio))));
            set(band.inDb, param(bandParam(b, In)));
            set(band.outDb, param(bandParam(b, Out)));
            set(band.audible, heard[b] ? 1.0 : 0.0);
            band.attack = smoothing(param(bandParam(b, Attack)) * time);
            band.release = smoothing(param(bandParam(b, Release)) * time);
        }
    }

    float smoothing(double ms) const noexcept {
        const double seconds = ms * 0.001;
        return seconds < 1e-6 ? 0.f : static_cast<float>(std::exp(-1.0 / (seconds * sampleRate_)));
    }

    // A crossover's g = tan(pi f / sr) for log2(f), f kept to 1 Hz .. 0.49 of the rate.
    float crossoverG(float log2Freq) const noexcept {
        const double f = std::clamp(std::exp2(static_cast<double>(log2Freq)), 1.0, 0.49 * sampleRate_);
        return static_cast<float>(std::tan(dsp::kPi * f / sampleRate_));
    }
    static dsp::CrossoverCoefficients crossoverCoefficients(float g) noexcept {
        return {dsp::SvfCoefficients(g, kButterworth)};
    }

    // At the start of each 32-sample chunk: the crossovers' glides take a step
    // (their g interpolated from here to there over the chunk), the windows
    // follow them, and what has died away is flushed.
    void beginChunk() noexcept {
        gLowFrom_ = gLowTo_;
        gHighFrom_ = gHighTo_;
        if (!log2Low_.settled() || !log2High_.settled()) {
            log2Low_.step(chunkGlide_);
            log2High_.step(chunkGlide_);
            gLowTo_ = crossoverG(std::min(log2Low_.value, log2High_.value));
            gHighTo_ = crossoverG(log2High_.value);
        }
        crossoversMoving_ = gLowFrom_ != gLowTo_ || gHighFrom_ != gHighTo_;
        updateWindows();
        flush();
    }

    // The detectors' windows and RMS coefficients, when the crossovers or the low band's switch moved them.
    void updateWindows() noexcept {
        const bool lowMerged = lowOn_.value < 1.f;
        if (log2Low_.value == windowsLog2Low_ && log2High_.value == windowsLog2High_ && lowMerged == windowsLowMerged_)
            return;
        windowsLog2Low_ = log2Low_.value;
        windowsLog2High_ = log2High_.value;
        windowsLowMerged_ = lowMerged;
        const double low = std::exp2(static_cast<double>(log2Low_.value)),
                     high = std::exp2(static_cast<double>(log2High_.value));
        for (int b = 0; b < mb::kBands; ++b) {
            BandState& band = bands_[static_cast<size_t>(b)];
            const double seconds = mb::windowSeconds(b, low, high, lowMerged);
            band.window = std::max(1, static_cast<int>(std::lround(seconds * sampleRate_)));
            band.rms = static_cast<float>(std::exp(-1.0 / (std::max(seconds, mb::kMinRmsSeconds) * sampleRate_)));
        }
    }

    // Flushes what has died away to exact zeros, so silence never runs into denormals.
    void flush() noexcept {
        for (Split& s : main_) s.flush();
        if (keyLive_) {
            for (Split& s : key_) s.flush();
        }
        for (BandState& band : bands_) {
            for (float& meanSquare : band.meanSquare) meanSquare = dsp::flushTiny(meanSquare);
            band.aboveDb = dsp::flushTiny(band.aboveDb);
            band.belowDb = dsp::flushTiny(band.belowDb);
        }
    }

    // Whether a band's switch is off and done fading: it publishes the floor.
    static bool settledOff(const Control& on) noexcept { return on.settled() && on.value == 0.f; }

    void publishMeters() noexcept {
        for (int b = 0; b < mb::kBands; ++b) {
            BandState& band = bands_[static_cast<size_t>(b)];
            const bool off = (b == mb::Low && settledOff(lowOn_)) || (b == mb::High && settledOff(highOn_));
            publish(DisplaysPerBand * b + InLevel, off ? mb::kDisplayFloorDb : band.meterIn);
            publish(DisplaysPerBand * b + OutLevel, off ? mb::kDisplayFloorDb : band.meterOut);
            publish(DisplaysPerBand * b + GainChange, off ? 0.f : band.meterGain);
            band.clearMeters();
        }
    }

    // One sample of the device-wide controls' glides (one that has landed stays put); false once they
    // have all landed.
    bool stepGlides() noexcept {
        bool moving = false;
        for (Control* control : {&lowOn_, &highOn_, &keyOn_, &scMix_, &amount_, &output_, &knee_, &mode_, &listen_})
            moving = control->step(glide_) || moving;
        if (!scGain_.settled()) {
            moving = scGain_.step(glide_) || moving;
            keyGain_ = expDbToGain(scGain_.value);
        }
        return moving;
    }
    // The same for a band's.
    bool stepGlides(BandState& band) const noexcept {
        bool moving = false;
        for (Control* control : {&band.active, &band.above, &band.below, &band.aboveSlope, &band.belowSlope,
                                 &band.inDb, &band.outDb, &band.audible})
            moving = control->step(glide_) || moving;
        return moving;
    }

    template <int N>
    void renderFrames(float* const* ch, int numFrames, const float* const* key) noexcept {
        for (int i = 0; i < numFrames; ++i) {
            if (chunkPos_ == 0) beginChunk();
            if (crossoversMoving_) {
                const float t = static_cast<float>(chunkPos_ + 1) * (1.f / kChunk);
                lowCoeffs_ = crossoverCoefficients(gLowFrom_ + t * (gLowTo_ - gLowFrom_));
                highCoeffs_ = crossoverCoefficients(gHighFrom_ + t * (gHighTo_ - gHighFrom_));
            }
            chunkPos_ = (chunkPos_ + 1) & (kChunk - 1);

            // The bands, and what each band's detector hears: its own band, or (keyed) the key's same band.
            float bands[N][mb::kBands], heard[N][mb::kBands];
            for (int k = 0; k < N; ++k) {
                main_[k].process(lowCoeffs_, highCoeffs_, ch[k][i], bands[k]);
                for (int b = 0; b < mb::kBands; ++b) heard[k][b] = bands[k][b];
            }
            // Every control's value this sample: its glide's (read as .value below). Only what moves steps.
            if (gliding_) gliding_ = stepGlides();
            if (bandsGliding_) {
                bandsGliding_ = false;
                for (BandState& band : bands_) {
                    if (band.gliding) band.gliding = stepGlides(band);
                    bandsGliding_ = bandsGliding_ || band.gliding;
                }
            }
            // How much of the key the detectors hear. Its split runs only while that is more than none (a
            // sidechain connected and S/C Mix over 0 %): at 0 % it would be worked out to be thrown away.
            const float share = keyOn_.value * scMix_.value;
            if (share > 0.f) {
                float keys[N];
                if constexpr (N == 2) {
                    for (int k = 0; k < 2; ++k) keys[k] = key[k] ? keyGain_ * keySample(key[k][i]) : 0.f;
                } else {  // one channel: the key's two summed (a stereo key isn't half ignored)
                    keys[0] = keyGain_ * 0.5f *
                              ((key[0] ? keySample(key[0][i]) : 0.f) + (key[1] ? keySample(key[1][i]) : 0.f));
                }
                for (int k = 0; k < N; ++k) {
                    float keyBands[mb::kBands];
                    key_[k].process(lowCoeffs_, highCoeffs_, keys[k], keyBands);
                    for (int b = 0; b < mb::kBands; ++b) heard[k][b] += share * (keyBands[b] - heard[k][b]);
                }
                keyLive_ = true;
            } else if (keyLive_) {  // let go: a key heard again starts from silence
                for (Split& s : key_) s.reset();
                keyLive_ = false;
            }

            // A band switched off is heard by the mid band's detector (its signal is the mid band's).
            const float lowOn = lowOn_.value, highOn = highOn_.value;
            float peak[mb::kBands] = {}, square[N][mb::kBands];
            for (int k = 0; k < N; ++k) {
                const float d[mb::kBands] = {
                    heard[k][mb::Low],
                    heard[k][mb::Mid] + (1.f - lowOn) * heard[k][mb::Low] + (1.f - highOn) * heard[k][mb::High],
                    heard[k][mb::High]};
                for (int b = 0; b < mb::kBands; ++b) {
                    peak[b] = std::max(peak[b], std::abs(d[b]));
                    square[k][b] = std::min(d[b] * d[b], kMaxSquare);
                }
            }

            const float rmsShare = mode_.value, knee = knee_.value, amount = amount_.value, outputDb = output_.value;
            float total[mb::kBands], audible[mb::kBands];
            for (int b = 0; b < mb::kBands; ++b) {
                BandState& band = bands_[static_cast<size_t>(b)];
                // Peak: the louder channel's (the largest of either). RMS: each channel's mean square, the
                // louder taken (the mean of the larger square each sample would read hot on wide stereo).
                const float held = band.peak.push(peak[b], band.window);
                float meanSquare = 0.f;
                for (int k = 0; k < N; ++k) {
                    float& state = band.meanSquare[k];
                    state = square[k][b] + band.rms * (state - square[k][b]);
                    meanSquare = k == 0 ? state : std::max(meanSquare, state);
                }
                float level;
                if (rmsShare == 0.f) {
                    level = kDbPerLog2 * std::log2(held + 1e-9f);
                } else if (rmsShare == 1.f) {
                    level = 0.5f * kDbPerLog2 * std::log2(meanSquare + 1e-18f);
                } else {  // switching Peak/RMS: a crossfade of the two levels
                    const float peakDb = kDbPerLog2 * std::log2(held + 1e-9f);
                    const float rmsDb = 0.5f * kDbPerLog2 * std::log2(meanSquare + 1e-18f);
                    level = peakDb + rmsShare * (rmsDb - peakDb);
                }
                const float inDb = band.inDb.value;
                level = std::max(level, kLevelFloorDb) + inDb;

                const float aboveTarget = mb::aboveGainDb(level, band.above.value, band.aboveSlope.value, knee);
                const float belowTarget = mb::belowGainDb(level, band.below.value, band.belowSlope.value, knee);
                // Attack while a side's change grows, release while it shrinks.
                band.aboveDb =
                    aboveTarget + (std::abs(aboveTarget) > std::abs(band.aboveDb) ? band.attack : band.release) *
                                      (band.aboveDb - aboveTarget);
                band.belowDb =
                    belowTarget + (std::abs(belowTarget) > std::abs(band.belowDb) ? band.attack : band.release) *
                                      (band.belowDb - belowTarget);
                const float gainDb = mb::bandGainDb(band.aboveDb, band.belowDb, amount);
                // Its activator: all of the band's own gains, or (bypassed) none of them.
                const float active = band.active.value;
                total[b] = active * (inDb + gainDb + band.outDb.value) + outputDb;
                audible[b] = band.audible.value;

                // (Bypassed, the level as it comes, and no change.)
                const float shown = level - (1.f - active) * inDb, change = active * gainDb;
                band.meterIn = std::max(band.meterIn, shown);
                band.meterOut = std::max(band.meterOut, shown + change);
                if (std::abs(change) > std::abs(band.meterGain)) band.meterGain = change;
            }

            // The gains: a band switched off takes the mid band's.
            const float midGain = std::exp(total[mb::Mid] * kNepersPerDb);
            const float lowGain = lowOn == 0.f ? midGain
                                  : lowOn == 1.f
                                      ? std::exp(total[mb::Low] * kNepersPerDb)
                                      : lowOn * std::exp(total[mb::Low] * kNepersPerDb) + (1.f - lowOn) * midGain;
            const float highGain = highOn == 0.f ? midGain
                                   : highOn == 1.f
                                       ? std::exp(total[mb::High] * kNepersPerDb)
                                       : highOn * std::exp(total[mb::High] * kNepersPerDb) + (1.f - highOn) * midGain;
            const float gains[mb::kBands] = {lowGain * audible[mb::Low], midGain * audible[mb::Mid],
                                             highGain * audible[mb::High]};
            for (int k = 0; k < N; ++k)
                ch[k][i] = bands[k][mb::Low] * gains[mb::Low] + bands[k][mb::Mid] * gains[mb::Mid] +
                           bands[k][mb::High] * gains[mb::High];
            // Listening: what the detectors hear instead (the bands of the trigger summed: an all-pass of it).
            if (const float listen = listen_.value; listen > 0.f) {
                for (int k = 0; k < N; ++k) {
                    const float trigger = heard[k][mb::Low] + heard[k][mb::Mid] + heard[k][mb::High];
                    ch[k][i] += listen * (trigger - ch[k][i]);
                }
            }

            if (++meterCount_ == mb::kDisplaySamples) {
                publishMeters();
                meterCount_ = 0;
            }
        }
    }

    static std::vector<DisplayInfo> displayInfos() {
        std::vector<DisplayInfo> list;
        for (const char* band : {"low", "mid", "high"}) {
            for (const char* kind : {"_in", "_out", "_gain"})
                list.push_back({std::string(band) + kind, mb::kDisplaySamples});
        }
        return list;
    }

    static const std::vector<ParamInfo>& infos() {
        static const std::vector<ParamInfo> kInfos = [] {
            const std::vector<std::string>& kOnOff = offOnLabels();
            // (The split switches, as Live's, and the solos and Listen, ways of listening, aren't automatable.)
            std::vector<ParamInfo> list = {
                {"xover_low", "Low-Mid Crossover", "Hz", 30.f, 3000.f, 120.f, true},
                {"xover_high", "Mid-High Crossover", "Hz", 300.f, 15000.f, 2500.f, true},
                {"low_on", "Low Band On", "", 0.f, 1.f, 1.f, false, kOnOff, 0, false},
                {"high_on", "High Band On", "", 0.f, 1.f, 1.f, false, kOnOff, 0, false},
            };
            const char* kIds[mb::kBands] = {"low", "mid", "high"};
            const char* kNames[mb::kBands] = {"Low", "Mid", "High"};
            for (int b = 0; b < mb::kBands; ++b) {
                const std::string id = kIds[b], name = kNames[b];
                const std::vector<ParamInfo> band = {
                    {id + "_active", name + " Activator", "", 0.f, 1.f, 1.f, false, kOnOff},
                    {id + "_in", name + " Input", "dB", -24.f, 24.f, 0.f},
                    {id + "_out", name + " Output", "dB", -24.f, 24.f, 0.f},
                    {id + "_above", name + " Above Threshold", "dB", mb::kMinThresholdDb, mb::kMaxThresholdDb, -20.f},
                    {id + "_above_ratio", name + " Above Ratio", "ratio", mb::kMinRatio, mb::kMaxRatio, 1.f, true},
                    {id + "_below", name + " Below Threshold", "dB", mb::kMinThresholdDb, mb::kMaxThresholdDb, -40.f},
                    {id + "_below_ratio", name + " Below Ratio", "ratio", mb::kMinRatio, mb::kMaxRatio, 1.f, true},
                    {id + "_attack", name + " Attack", "ms", 0.1f, 5000.f, 10.f, true},
                    {id + "_release", name + " Release", "ms", 0.1f, 5000.f, 100.f, true},
                    {id + "_solo", name + " Solo", "", 0.f, 1.f, 0.f, false, kOnOff, 0, false},
                };
                list.insert(list.end(), band.begin(), band.end());
            }
            const std::vector<ParamInfo> global = {
                {"amount", "Amount", "%", 0.f, 100.f, 100.f},
                {"time", "Time", "%", 10.f, 1000.f, 100.f, true},
                {"output", "Output", "dB", -24.f, 24.f, 0.f},
                {"soft_knee", "Soft Knee", "", 0.f, 1.f, 0.f, false, kOnOff},
                {"mode", "Peak/RMS", "", 0.f, 1.f, 1.f, false, {"Peak", "RMS"}},
                {"sc_gain", "S/C Gain", "dB", -70.f, 24.f, 0.f},
                {"sc_mix", "S/C Mix", "%", 0.f, 100.f, 100.f},
                {"sc_listen", "S/C Listen", "", 0.f, 1.f, 0.f, false, kOnOff, 0, false},
            };
            list.insert(list.end(), global.begin(), global.end());
            return list;
        }();
        return kInfos;
    }

    double sampleRate_ = 48000.0;
    double glide_ = 0.0, chunkGlide_ = 0.0;  // the glides' shares per step: per sample, per chunk
    int channels_ = 2;

    std::array<Split, 2> main_{}, key_{};  // per channel: the input's split and the key's
    bool keyLive_ = false;                 // the key's split has run since it was last cleared
    bool snapKey_ = true;                  // the first render() after a reset snaps keyOn_
    std::array<BandState, mb::kBands> bands_{};
    Control lowOn_, highOn_, keyOn_, amount_, output_, knee_, mode_, scMix_, scGain_, listen_;
    float keyGain_ = 1.f;       // the sidechain's gain, linear
    bool gliding_ = true;       // a device-wide control's glide hasn't landed: stepGlides() each sample
    bool bandsGliding_ = true;  // a band's hasn't

    // The crossovers: log2(f) glides stepped per chunk; g interpolated within it.
    Control log2Low_, log2High_;
    float gLowFrom_ = 0.f, gLowTo_ = 0.f, gHighFrom_ = 0.f, gHighTo_ = 0.f;
    dsp::CrossoverCoefficients lowCoeffs_, highCoeffs_;  // as made last (kept while nothing glides)
    bool crossoversMoving_ = false;
    int chunkPos_ = 0;                                    // 0..31, carried across render() calls
    float windowsLog2Low_ = 0.f, windowsLog2High_ = 0.f;  // what the windows were worked out for
    bool windowsLowMerged_ = false;

    int meterCount_ = 0;
};

}  // namespace

SUB_REGISTER_BUILTIN(MultibandProcessor, AudioEffect);

}  // namespace sub
