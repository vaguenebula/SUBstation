// Built-in "Gate" device, after Ableton's Gate: it passes only what is louder
// than the threshold, and turns everything else down to the floor (silence at
// the bottom of its range). Flip turns it round: only what is quieter passes.
//
// - The key, what opens it, is its own input; with a sidechain chosen, a blend
//   of the two (S/C Mix, after S/C Gain on the sidechain: NaN or infinity there,
//   from a broken source, is taken as silence, as the input's is), optionally
//   through an EQ (six RBJ types in Live's order, high-pass at 80 Hz by default).
//   Listening puts the key out instead of the gated audio.
// - Detection is peak, linked stereo: the louder key channel's level opens the
//   gate for both. With lookahead the audio is delayed and the level is the key's
//   largest over the lookahead window (a sliding maximum), so the gate opens
//   that much before a transient reaches the output and closes that much after
//   the level has fallen (the hold counts from when the fall reaches the output).
// - It opens when the level reaches the threshold and stays open while the level
//   is at or above the threshold less Return (hysteresis), then Hold more, then
//   closes. How far open it is moves linearly over Attack while open, over
//   Release while closed (from wherever it is: no jumps), eased in and out
//   (smoothstep); the gain is the floor's at closed, exactly 1 fully open, so an
//   open gate without lookahead passes its input bit for bit.
// - Nothing that moves the gain jumps: Floor ramps over 20 ms, Flip and Listen
//   fade over 10 ms, S/C Gain and Mix over 20 ms (also when a sidechain comes or
//   goes), the EQ fades in and out over 10 ms and its Freq, Q and Gain glide
//   (15 ms, every 32 frames while they move); a change of the EQ's type or of the
//   lookahead crossfades over 10 ms (one that comes during a crossfade starts when
//   it is done). A filter the EQ starts (switched on, or a new type) starts warm:
//   run first over the key's last moments, a few times faster than they came
//   until it has caught up (at most 6.7 ms), and heard only then, so it has
//   nothing to settle and its fade in is only ever between two settled outputs.
//   Threshold, Return, Attack, Hold and Release only move decisions and the
//   ramps' speeds.
// - Lookahead is latency (0, 1 or 10 ms; 1 ms by default, as Live's): idle()
//   tells the engine when it changes, so the other tracks are realigned.
//
// Its editor draws four displays, one value per gate::kDisplaySamples, pushed
// together: the input's level as it reaches the gain, the output's, the key's
// (before the lookahead: when the gate decides), and how much passed (0..1).

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/DspBlocks.h"
#include "builtin/GateDesign.h"
#include "rt/RtUtils.h"

namespace sub {
namespace {

constexpr double kFloorSeconds = 0.02;     // Floor's ramp
constexpr double kSwitchSeconds = 0.01;    // Flip, Listen, the EQ on and off; the lookahead's and EQ type's crossfades
constexpr double kKeySeconds = 0.02;       // S/C Gain and S/C Mix (and a sidechain coming or going)
constexpr double kEqGlideSeconds = 0.015;  // the key EQ's Freq, Q and Gain
constexpr int kChannels = 2;

using dsp::sCurve;  // (the crossfades' shape: flat at both ends)

// A sidechain sample as the key takes it: NaN, infinity or an absurd level (what a broken source
// can leave in the engine's buffer, which only the device's own input is cleaned of) as silence.
inline float audible(float x) noexcept { return std::abs(x) <= BuiltinProcessor::kMaxInput ? x : 0.f; }

class GateProcessor final : public BuiltinProcessor {
public:
    enum Param {
        Threshold = 0, Return, Attack, Hold, Release, Floor, Lookahead, Flip,
        ScGain, ScMix, ScListen, ScEq, ScEqType, ScEqFreq, ScEqQ, ScEqGain, NumParams
    };
    enum Display { InputLevel = 0, OutputLevel, KeyLevel, Open };

    GateProcessor()
        : BuiltinProcessor(infos(), {{"input", gate::kDisplaySamples}, {"output", gate::kDisplaySamples},
                                     {"key", gate::kDisplaySamples}, {"open", gate::kDisplaySamples}}) {}

    std::string typeId() const override { return "builtin:gate"; }
    std::string name() const override { return "Gate"; }
    bool hasSidechain() const override { return true; }

    int latencySamples() const override { return gate::lookaheadSamples(choiceIndex(Lookahead), sampleRate_); }
    // What is still in the lookahead's delay when the input stops.
    int tailSamples() const override { return latencySamples(); }

    // The lookahead changed: the engine must realign the tracks.
    bool idle() override {
        const int latency = latencySamples();
        if (latency == reportedLatency_) return false;
        reportedLatency_ = latency;
        return true;
    }

    void prepare(double sampleRate, int) override {
        sampleRate_ = sampleRate;
        const int most = gate::lookaheadSamples(gate::kLookaheads - 1, sampleRate);
        for (int c = 0; c < kChannels; ++c) {
            audio_[c].prepare(most + 8);
            keyDelay_[c].prepare(most + 8);
        }
        warmMost_ = std::max(1, static_cast<int>(std::lround(gate::kWarmSeconds * sampleRate)));
        for (dsp::DelayLine& key : keyIn_) key.prepare(warmMost_ + 1);
        keyPeaks_.prepare(most + 8);
        for (int i = 0; i < gate::kLookaheads; ++i) {
            lookaheads_[i] = gate::lookaheadSamples(i, sampleRate);
            peaks_[i].prepare(lookaheads_[i] + 1);
        }
        fadeLength_ = std::max(1, static_cast<int>(std::lround(kSwitchSeconds * sampleRate)));
        floor_.reset(sampleRate, kFloorSeconds);
        flip_.reset(sampleRate, kSwitchSeconds);
        listen_.reset(sampleRate, kSwitchSeconds);
        scGain_.reset(sampleRate, kKeySeconds);
        scMix_.reset(sampleRate, kKeySeconds);
        eqMix_.reset(sampleRate, kSwitchSeconds);
        eqGlide_ = onePoleCoefficient(kEqGlideSeconds, sampleRate, gate::kEqChunk);
        reportedLatency_ = latencySamples();
        reset();
    }

    // Silent and closed; the next render() starts every ramp, the lookahead and
    // the key EQ at the parameters as they are then (whether a sidechain is
    // connected is only known there).
    void reset() override {
        for (int c = 0; c < kChannels; ++c) {
            audio_[c].reset();
            keyDelay_[c].reset();
            keyIn_[c].reset();
            eqState_[c].reset();
            eqOldState_[c].reset();
        }
        keyPeaks_.reset();
        for (dsp::SlidingMax& peak : peaks_) peak.reset();
        open_ = false;
        held_ = 0;
        openness_ = 0.0;
        fadeLeft_ = typeFadeLeft_ = 0;
        warmLag_ = 0;
        eqRunning_ = eqStarting_ = false;
        meterCount_ = 0;
        peakIn_ = peakOut_ = peakKey_ = 0.f;
        passSum_ = 0.0;
        fresh_ = true;
    }

protected:
    void render(const ProcessContext&, float* const* ch, int numChannels, int numFrames) override {
        const int n = std::min(numChannels, kChannels);
        if (n <= 0) return;
        if (n != channels_) {  // a channel that wasn't processed has no history to go on from
            for (int c = channels_; c < n; ++c) {
                audio_[c].reset();
                keyDelay_[c].reset();
            }
            channels_ = n;
        }

        const float thresholdDb = param(Threshold);
        const float openLevel = std::pow(10.f, thresholdDb / 20.f);                                 // opens at or above
        const float closeLevel = std::pow(10.f, gate::closeDb(thresholdDb, param(Return)) / 20.f);  // closes below
        const double attackStep = 1.0 / std::max(1.0, param(Attack) * 0.001 * sampleRate_);
        const double releaseStep = 1.0 / std::max(1.0, param(Release) * 0.001 * sampleRate_);
        const int holdSamples = static_cast<int>(std::lround(param(Hold) * 0.001 * sampleRate_));

        // With a sidechain the key is what it hears (silence while solo leaves its
        // source out), blended with the device's own input by S/C Mix; without one,
        // its own input (the blend glides there, so nothing steps while listening).
        const bool keyed = sidechainConnected();
        const float* scL = keyed ? sidechain(0) : nullptr;
        const float* scR = keyed ? sidechain(1) : nullptr;
        if (scR == nullptr) scR = scL;

        floor_.setTarget(gate::floorGain(param(Floor)));
        flip_.setTarget(isOn(Flip) ? 1.f : 0.f);
        listen_.setTarget(isOn(ScListen) ? 1.f : 0.f);
        scGain_.setTarget(dbToGain(param(ScGain)));
        scMix_.setTarget(keyed ? std::clamp(param(ScMix) / 100.f, 0.f, 1.f) : 0.f);
        lookaheadTarget_ = std::clamp(choiceIndex(Lookahead), 0, gate::kLookaheads - 1);
        typeTarget_ = static_cast<gate::KeyFilter>(std::clamp(choiceIndex(ScEqType), 0, gate::kKeyFilters - 1));

        // The key EQ: switched on (or on in the first stretch, after silence), it starts at its settings;
        // after sound it fades in once its filter has caught up with the key.
        const bool fresh = fresh_;
        const bool eqOn = isOn(ScEq);
        if (eqOn || eqRunning_) {
            eqTargetLogFreq_ = std::log(std::clamp<double>(param(ScEqFreq), gate::kKeyFreqMin, gate::kKeyFreqMax));
            eqTargetLogQ_ = std::log(std::clamp<double>(param(ScEqQ), gate::kKeyQMin, gate::kKeyQMax));
            eqTargetGainDb_ = std::clamp<double>(param(ScEqGain), gate::kKeyGainMinDb, gate::kKeyGainMaxDb);
        }
        if (eqOn && !eqRunning_) startEq(!fresh);
        eqMix_.setTarget(eqOn && !eqStarting_ ? 1.f : 0.f);

        if (fresh) {  // the first stretch since reset(): everything starts at its settings, nothing ramps
            for (SmoothedValue* s : {&floor_, &flip_, &listen_, &scGain_, &scMix_, &eqMix_}) s->snapTo(s->target());
            lookahead_ = lookaheadTarget_;
            fadeLeft_ = 0;
            refillPeaks();
            fresh_ = false;
        }
        // The key EQ runs while it is on or fading out; nothing of it is computed otherwise.
        eqRunning_ = eqOn || eqMix_.isSmoothing() || eqMix_.current() > 0.f;
        if (!eqRunning_) eqType_ = typeTarget_;

        // The state the loop changes, in locals (stores to the audio buffers could
        // otherwise be the members', so the compiler would reload them every sample).
        bool open = open_;
        int held = held_;
        double openness = openness_;
        SmoothedValue floorGain = floor_, flip = flip_, listen = listen_;
        float peakIn = peakIn_, peakOut = peakOut_, peakKey = peakKey_;
        double passSum = passSum_;
        int meterCount = meterCount_;

        for (int i = 0; i < numFrames; ++i) {
            // (1) The key: the input, or its blend with the sidechain, then the EQ.
            const float in0 = ch[0][i], in1 = n > 1 ? ch[1][i] : in0;
            float kL = in0, kR = in1;
            if (keyed || scMix_.isSmoothing()) {  // (without a sidechain the mix rests at 0: the input as it is)
                const float g = scGain_.next(), m = scMix_.next();
                const float sL = scL != nullptr ? audible(scL[i]) : 0.f, sR = scR != nullptr ? audible(scR[i]) : 0.f;
                kL = in0 + m * (g * sL - in0);
                kR = in1 + m * (g * sR - in1);
            }
            keyIn_[0].push(kL);  // (kept whether the EQ runs or not: it starts warm from them)
            keyIn_[1].push(kR);
            if (eqRunning_) filterKey(kL, kR);
            const float keyPeak = std::max(std::abs(kL), std::abs(kR));

            // (2) The level: the key's largest over the lookahead window, so the gate opens
            //     the lookahead before a transient reaches the output and stays open until
            //     the lookahead after it has fallen. A new lookahead starts its crossfade here.
            if (fadeLeft_ == 0 && lookahead_ != lookaheadTarget_) {
                fadeFrom_ = lookaheads_[lookahead_];
                lookahead_ = lookaheadTarget_;
                fadeLeft_ = fadeLength_;
                refillPeaks();
            }
            const int delay = lookaheads_[lookahead_];
            const float level = delay == 0 ? keyPeak : peaks_[lookahead_].push(keyPeak, delay + 1);
            keyPeaks_.push(keyPeak);

            // (3) Open at the threshold; held open while at or above the closing level,
            //     then for the hold, then closed. The first loud sample opens it and takes
            //     the first attack step; the first after the hold the first release step.
            //     The hold counts up from the fall, so a Hold changed while it counts
            //     applies at once (a shorter one already passed closes it there).
            if (!open) {
                if (level >= openLevel) {
                    open = true;
                    held = 0;
                }
            } else if (level >= closeLevel) {
                held = 0;
            } else if (held < holdSamples) {
                ++held;
            } else {
                open = false;
            }
            if (open) {  // (the ramps land exactly on 1 and 0, whatever the steps add up to)
                openness += attackStep;
                if (openness >= 1.0 - 1e-9) openness = 1.0;
            } else {
                openness -= releaseStep;
                if (openness <= 1e-9) openness = 0.0;
            }

            // (4) The gain: the openness eased, flipped, between the floor and unity.
            const float pass = gate::pass(static_cast<float>(openness), flip.next());
            const float gain = gate::gain(pass, floorGain.next());

            // (5) The audio through the lookahead, times the gain; listening, the key
            //     (delayed alike) instead (not listening, nothing of the key is in it). A
            //     lookahead changing crossfades between taps.
            const float li = listen.next();
            for (int c = 0; c < n; ++c) {
                audio_[c].push(ch[c][i]);
                keyDelay_[c].push(c == 0 ? (n == 1 ? 0.5f * (kL + kR) : kL) : kR);
                float a = audio_[c].tap(delay), k = keyDelay_[c].tap(delay);
                if (fadeLeft_ > 0) {
                    const float t = sCurve(1.f - static_cast<float>(fadeLeft_) / static_cast<float>(fadeLength_));
                    const float a0 = audio_[c].tap(fadeFrom_), k0 = keyDelay_[c].tap(fadeFrom_);
                    a = a0 + t * (a - a0);
                    k = k0 + t * (k - k0);
                }
                float out = a * gain;
                if (li == 1.f)
                    out = k;
                else if (li > 0.f)
                    out += li * (k - out);
                ch[c][i] = out;
                peakIn = std::max(peakIn, std::abs(a));
                peakOut = std::max(peakOut, std::abs(out));
            }
            if (fadeLeft_ > 0) --fadeLeft_;

            peakKey = std::max(peakKey, keyPeak);
            passSum += pass;
            if (++meterCount == gate::kDisplaySamples) {
                publish(InputLevel, toDb(peakIn));
                publish(OutputLevel, toDb(peakOut));
                publish(KeyLevel, toDb(peakKey));
                publish(Open, static_cast<float>(passSum / gate::kDisplaySamples));
                meterCount = 0;
                peakIn = peakOut = peakKey = 0.f;
                passSum = 0.0;
            }
        }

        open_ = open;
        held_ = held;
        openness_ = openness;
        floor_ = floorGain;
        flip_ = flip;
        listen_ = listen;
        peakIn_ = peakIn;
        peakOut_ = peakOut;
        peakKey_ = peakKey;
        passSum_ = passSum;
        meterCount_ = meterCount;
    }

private:
    // The key through the EQ: a glide step every kEqChunk frames, a filter starting
    // catching up, a type change's crossfade, and the EQ's own fade in or out.
    void filterKey(float& kL, float& kR) noexcept {
        if (--eqChunkLeft_ <= 0) {  // (counted per frame: the same pace however automation splits the block)
            eqChunkLeft_ = gate::kEqChunk;
            glideEq();
        }
        if (typeFadeLeft_ == 0 && warmLag_ == 0 && eqType_ != typeTarget_) {  // the old filter goes on, its state kept
            eqOld_ = eq_;
            eqOldState_[0] = eqState_[0];
            eqOldState_[1] = eqState_[1];
            eqType_ = typeTarget_;
            eq_ = designEq();
            startWarm();
            typeFadeLeft_ = fadeLength_;
        }
        float fL = kL, fR = kR;
        bool live = true;  // the filter is level with the key: heard
        if (warmLag_ == 0) {
            fL = eqState_[0].process(eq_, kL);
            fR = eqState_[1].process(eq_, kR);
        } else {
            live = catchUp(fL, fR);
            if (live && eqStarting_) {  // switched on, and warm now: it fades in from here
                eqStarting_ = false;
                eqMix_.setTarget(1.f);
            }
        }
        if (typeFadeLeft_ > 0) {  // the old filter, fading out from when the new one is heard
            const float oL = eqOldState_[0].process(eqOld_, kL), oR = eqOldState_[1].process(eqOld_, kR);
            const float t =
                live ? sCurve(1.f - static_cast<float>(typeFadeLeft_--) / static_cast<float>(fadeLength_)) : 0.f;
            fL = oL + t * (fL - oL);
            fR = oR + t * (fR - oR);
        }
        const float e = eqMix_.next();
        kL += e * (fL - kL);
        kR += e * (fR - kR);
    }

    // The sliding maximum for the lookahead now, from the key's peaks it has missed
    // (each lookahead has its own, exactly as long as its window, so it never searches).
    void refillPeaks() noexcept {
        const int window = lookaheads_[lookahead_] + 1;
        dsp::SlidingMax& peak = peaks_[lookahead_];
        peak.reset();
        for (int d = window - 2; d >= 0; --d) peak.push(keyPeaks_.tap(d), window);
    }

    // The EQ switched on: its glides and type at the parameters, its filter warm
    // from the key before this stretch (after reset(), that was silence: from rest,
    // heard at once).
    void startEq(bool warmed) noexcept {
        for (int c = 0; c < kChannels; ++c) {
            eqState_[c].reset();
            eqOldState_[c].reset();
        }
        typeFadeLeft_ = 0;
        eqType_ = typeTarget_;
        eqLogFreq_ = eqTargetLogFreq_;
        eqLogQ_ = eqTargetLogQ_;
        eqGainDb_ = eqTargetGainDb_;
        eq_ = designEq();
        warmLag_ = 0;
        if (warmed) startWarm();
        eqStarting_ = warmLag_ > 0;
        eqChunkLeft_ = gate::kEqChunk;
    }

    // The filter eq_ starts warm: from rest, it is run over the key's frames before
    // this one (keyIn_), as many as its slowest pole takes to fall 60 dB (so where
    // it started no longer shows), at most kWarmSeconds' worth, and then goes on as
    // if it had been running all along; carrying on from another filter's state,
    // or from rest, its first tens of milliseconds would overshoot (+5 dB, a low
    // shelf at 30 Hz) and could open the gate. It runs over them kWarmPace frames a
    // frame (catchUp()), so no one block pays for all of them (up to 19 200 frames
    // a channel at 192 kHz): most filters catch up within a few dozen frames, the
    // slowest in kWarmSeconds / (kWarmPace - 1), 6.7 ms. Until then what it
    // replaces (the old filter, or the key unfiltered) is heard.
    void startWarm() noexcept {
        for (dsp::Biquad& state : eqState_) state.reset();
        warmLag_ = gate::warmFrames(eq_, warmMost_);
    }
    // One frame of the filter catching up: it runs over up to kWarmPace frames of
    // the key it hasn't heard, oldest first, this one last once it gets to it.
    // Whether it has (fL, fR are then its output for this frame).
    bool catchUp(float& fL, float& fR) noexcept {
        const int count = std::min(warmLag_ + 1, gate::kWarmPace);
        for (int d = warmLag_; d > warmLag_ - count; --d) {
            fL = eqState_[0].process(eq_, keyIn_[0].tap(d));
            fR = eqState_[1].process(eq_, keyIn_[1].tap(d));
        }
        warmLag_ += 1 - count;
        return warmLag_ == 0;
    }

    // One glide step of Freq, Q (both in log) and Gain (dB); the filter designed again if they moved.
    void glideEq() noexcept {
        bool moved = glide(eqLogFreq_, eqTargetLogFreq_, 1e-4);
        moved = glide(eqLogQ_, eqTargetLogQ_, 1e-4) || moved;
        moved = glide(eqGainDb_, eqTargetGainDb_, 1e-3) || moved;
        if (moved) eq_ = designEq();
    }
    bool glide(double& value, double target, double landed) const noexcept {
        if (value == target) return false;
        value = target + eqGlide_ * (value - target);
        if (std::abs(value - target) < landed) value = target;
        return true;
    }
    dsp::BiquadCoefficients designEq() const noexcept {
        return gate::keyFilter(eqType_, std::exp(eqLogFreq_), std::exp(eqLogQ_), eqGainDb_, sampleRate_);
    }

    static float toDb(float peak) noexcept { return std::max(gate::kDisplayFloorDb, gainToDb(peak)); }

    static const std::vector<ParamInfo>& infos() {
        static const std::vector<ParamInfo> kInfos = [] {
            const std::vector<std::string>& kOnOff = offOnLabels();
            static const std::vector<std::string> kLookaheads = {"0 ms", "1 ms", "10 ms"};
            static const std::vector<std::string> kTypes = {"Low Shelf", "Bell", "High Shelf",
                                                            "Low-pass", "Band-pass", "High-pass"};
            std::vector<ParamInfo> list = {
                {"threshold", "Threshold", "dB", gate::kThresholdMinDb, gate::kThresholdMaxDb, -12.f},
                {"return", "Return", "dB", 0.f, gate::kReturnMaxDb, 3.f},
                {"attack", "Attack", "ms", 0.02f, 150.f, 3.5f, true},
                {"hold", "Hold", "ms", 1.f, 1500.f, 10.f, true},
                {"release", "Release", "ms", 0.1f, 3000.f, 15.f, true},
                {"floor", "Floor", "dB", gate::kFloorMinDb, 0.f, -40.f},
                {"lookahead", "Lookahead", "", 0.f, 2.f, 1.f, false, kLookaheads},
                {"flip", "Flip", "", 0.f, 1.f, 0.f, false, kOnOff},
                {"sc_gain", "S/C Gain", "dB", -70.f, 24.f, 0.f},
                {"sc_mix", "S/C Mix", "%", 0.f, 100.f, 100.f},
                {"sc_listen", "S/C Listen", "", 0.f, 1.f, 0.f, false, kOnOff},
                {"sc_eq", "S/C EQ On", "", 0.f, 1.f, 0.f, false, kOnOff},
                {"sc_eq_type", "S/C EQ Type", "", 0.f, 5.f, 5.f, false, kTypes},
                {"sc_eq_freq", "S/C EQ Freq", "Hz", gate::kKeyFreqMin, gate::kKeyFreqMax, 80.f, true},
                {"sc_eq_q", "S/C EQ Q", "", gate::kKeyQMin, gate::kKeyQMax, 0.71f, true},
                {"sc_eq_gain", "S/C EQ Gain", "dB", gate::kKeyGainMinDb, gate::kKeyGainMaxDb, 0.f},
            };
            list[Lookahead].automatable = false;  // (it is the device's latency)
            list[ScListen].automatable = false;   // (a way to listen, not part of the sound; as Live's SideListen)
            return list;
        }();
        return kInfos;
    }

    double sampleRate_ = 48000.0;
    int reportedLatency_ = 0;  // (main thread: idle())
    bool fresh_ = true;        // reset() since the last render(): snap, don't ramp
    int channels_ = kChannels;

    // The audio and the key, through the lookahead; the key's peaks, and their
    // largest over the lookahead's window (one for each choice; 0 ms's, a window
    // of the sample alone, is never pushed: the peak is the level as it is).
    dsp::DelayLine audio_[kChannels], keyDelay_[kChannels], keyPeaks_;
    dsp::SlidingMax peaks_[gate::kLookaheads];
    int lookaheads_[gate::kLookaheads] = {};  // each choice in samples (prepare())
    int lookahead_ = 0, lookaheadTarget_ = 0;           // the choice now, and as set
    int fadeFrom_ = 0, fadeLeft_ = 0;                   // a lookahead change's crossfade from the old tap (samples)
    int fadeLength_ = 480;

    // The gate.
    bool open_ = false;
    int held_ = 0;           // samples the level has been below the closing level while open
    double openness_ = 0.0;  // 0 closed .. 1 open, linear in time
    SmoothedValue floor_, flip_, listen_, scGain_, scMix_, eqMix_;

    // The key EQ: the filter now (and the old one during a type's crossfade), and its glides.
    gate::KeyFilter eqType_ = gate::KeyFilter::HighPass, typeTarget_ = gate::KeyFilter::HighPass;
    double eqLogFreq_ = 0.0, eqLogQ_ = 0.0, eqGainDb_ = 0.0;
    double eqTargetLogFreq_ = 0.0, eqTargetLogQ_ = 0.0, eqTargetGainDb_ = 0.0;
    double eqGlide_ = 0.0;  // (prepare())
    int eqChunkLeft_ = 0;  // frames to the next glide step (carried across stretches)
    dsp::BiquadCoefficients eq_, eqOld_;
    dsp::Biquad eqState_[kChannels], eqOldState_[kChannels];
    dsp::DelayLine keyIn_[kChannels];  // the key before the EQ, its last kWarmSeconds (to start a filter warm from)
    int warmMost_ = 1;       // kWarmSeconds in frames
    int warmLag_ = 0;        // frames before this one a starting filter has still to run over (0: it is level)
    int typeFadeLeft_ = 0;   // (counted from when the new type's filter is level with the key)
    bool eqRunning_ = false;
    bool eqStarting_ = false;  // switched on, its filter catching up: the fade in waits

    // The displays' accumulators.
    int meterCount_ = 0;
    float peakIn_ = 0.f, peakOut_ = 0.f, peakKey_ = 0.f;
    double passSum_ = 0.0;
};

}  // namespace

SUB_REGISTER_BUILTIN(GateProcessor, AudioEffect);

}  // namespace sub
