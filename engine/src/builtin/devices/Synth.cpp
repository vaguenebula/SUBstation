// Built-in "Synth" instrument: a simple polyphonic subtractive synthesizer.
// One oscillator per voice (sine, triangle, band-limited saw or square), an
// ADSR amplitude envelope and a resonant low-pass filter. Mono, on both channels.
// Each voice follows its note's bend (MIDI 2.0's per-note pitch bend).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/Dsp.h"
#include "NoteBend.h"
#include "rt/RtUtils.h"

namespace sub {
namespace {

class SynthProcessor final : public BuiltinProcessor {
public:
    enum Param { Wave = 0, Attack, Decay, Sustain, Release, Cutoff, Resonance, Volume, NumParams };
    enum class Waveform : uint8_t { Sine, Triangle, Saw, Square };
    static constexpr int kMaxVoices = 16;

    SynthProcessor();

    std::string typeId() const override { return "builtin:synth"; }
    std::string name() const override { return "Synth"; }
    bool acceptsMidi() const override { return true; }

    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    int tailSamples() const override;

protected:
    // Writes (does not add) the synth's output: it is the first device on a MIDI track.
    void render(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) override;

private:
    enum class Stage : uint8_t { Attack, Decay, Release };  // Decay ends at, and holds, the sustain level

    struct Voice {
        bool active = false;
        bool held = true;  // no note-off yet
        uint8_t key = 0;
        int32_t noteId = -1;   // its note's (-1: known by its key)
        uint64_t started = 0;  // note counter, for stealing the oldest voice
        float gain = 0.f;      // velocity
        double phase = 0.0;    // 0..1
        double increment = 0.0;
        double keyIncrement = 0.0;  // at its key, unbent
        double bend = 0.0;          // semitones, gliding to bendTarget
        double bendTarget = 0.0;
        Stage stage = Stage::Attack;
        float level = 0.f;     // envelope
        dsp::Svf filter;
    };

    // Per-block settings derived from the parameters.
    struct Envelope {
        float attackStep;
        float decayCoef;
        float sustain;
        float releaseCoef;
    };

    void noteOn(const ProcessEvent& event);
    void noteOff(const ProcessEvent& event);
    void noteBend(const ProcessEvent& event);
    // The voice an event about a note is for: by its id, else by its key (the
    // newest held one, else the newest still sounding). Null: none sounds.
    Voice* voiceOf(const ProcessEvent& event, bool held);
    void renderVoices(int from, int to, Waveform wave, const Envelope& env);
    Envelope envelope() const;

    std::array<Voice, kMaxVoices> voices_{};
    uint64_t noteCounter_ = 0;
    double sampleRate_ = 48000.0;

    // Filter coefficients per sample of the block (the cutoff glides), and the mix.
    std::vector<dsp::SvfCoefficients> filter_;
    std::vector<float> mix_;
    float cutoff_ = 0.f;  // smoothed, Hz
    float cutoffGlide_ = 0.f;
    double bendGlide_ = 0.0;  // how much of the way to its bend a voice goes in a sample
    SmoothedValue volume_;
};


constexpr double kPi = std::numbers::pi;
constexpr float kVoiceGain = 0.25f;  // one voice's peak at full velocity

// PolyBLEP: smooths an oscillator's step at phase 0 over one sample on each
// side, which removes most of the aliasing of a naive saw or square.
inline double polyBlep(double t, double dt) noexcept {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

const std::vector<ParamInfo>& infos() {
    static const std::vector<ParamInfo> kInfos = [] {
        std::vector<ParamInfo> list = {
            {"wave", "Wave", "", 0.f, 3.f, 2.f},
            {"attack", "Attack", "ms", 1.f, 5000.f, 3.f, true},
            {"decay", "Decay", "ms", 1.f, 5000.f, 300.f, true},
            {"sustain", "Sustain", "%", 0.f, 100.f, 70.f},
            {"release", "Release", "ms", 1.f, 10000.f, 200.f, true},
            {"cutoff", "Cutoff", "Hz", 20.f, 20000.f, 4000.f, true},
            {"resonance", "Resonance", "%", 0.f, 100.f, 10.f},
            {"volume", "Volume", "dB", -60.f, 6.f, 0.f},
        };
        list[SynthProcessor::Wave].valueLabels = {"Sine", "Triangle", "Saw", "Square"};
        return list;
    }();
    return kInfos;
}

}  // namespace

SynthProcessor::SynthProcessor() : BuiltinProcessor(infos()) {}

void SynthProcessor::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate;
    filter_.assign(static_cast<size_t>(maxBlockSize), dsp::SvfCoefficients{});
    mix_.assign(static_cast<size_t>(maxBlockSize), 0.f);
    cutoffGlide_ = static_cast<float>(1.0 - onePoleCoefficient(0.005, sampleRate));  // ~5 ms
    bendGlide_ = 1.0 - onePoleCoefficient(dsp::kBendGlideSeconds, sampleRate);
    volume_.reset(sampleRate, 0.02);
    reset();
}

void SynthProcessor::reset() {
    for (Voice& voice : voices_) voice.active = false;
    cutoff_ = param(Cutoff);
    volume_.snapTo(dbToGain(param(Volume)));
}

int SynthProcessor::tailSamples() const { return static_cast<int>(param(Release) * 0.001 * sampleRate_); }

SynthProcessor::Envelope SynthProcessor::envelope() const {
    const double samplesPerMs = sampleRate_ * 0.001;
    return {
        dsp::riseStep(param(Attack), samplesPerMs),
        dsp::fallCoefficient(param(Decay), samplesPerMs),
        std::clamp(param(Sustain) / 100.f, 0.f, 1.f),
        dsp::fallCoefficient(param(Release), samplesPerMs),
    };
}

void SynthProcessor::noteOn(const ProcessEvent& event) {
    // A free voice; else the quietest releasing one; else the oldest.
    Voice* target = nullptr;
    for (Voice& voice : voices_) {
        if (!voice.active) {
            target = &voice;
            break;
        }
    }
    if (!target) {
        for (Voice& voice : voices_) {
            if (voice.stage == Stage::Release && (!target || voice.level < target->level)) target = &voice;
        }
    }
    if (!target) {
        for (Voice& voice : voices_) {
            if (!target || voice.started < target->started) target = &voice;
        }
    }
    Voice& voice = *target;
    voice = Voice{};
    voice.active = true;
    voice.key = event.key();
    voice.noteId = event.noteId;
    voice.started = ++noteCounter_;
    voice.gain = kVoiceGain * static_cast<float>(event.velocity()) / 127.f;
    voice.keyIncrement = 440.0 * std::pow(2.0, (voice.key - 69) / 12.0) / sampleRate_;
    voice.increment = voice.keyIncrement;
}

SynthProcessor::Voice* SynthProcessor::voiceOf(const ProcessEvent& event, bool held) {
    if (event.noteId >= 0) {
        for (Voice& voice : voices_) {
            if (voice.active && voice.noteId == event.noteId && (!held || voice.held)) return &voice;
        }
    }
    // With the same key held twice, the older note ends first; a bend goes to the newest.
    Voice* target = nullptr;
    for (Voice& voice : voices_) {
        if (!voice.active || voice.key != event.key() || (held && !voice.held)) continue;
        if (event.noteId >= 0 && voice.noteId >= 0) continue;  // another note of that key
        if (!target || (held ? voice.started < target->started : voice.started > target->started)) target = &voice;
    }
    return target;
}

void SynthProcessor::noteOff(const ProcessEvent& event) {
    if (Voice* target = voiceOf(event, true)) {
        target->held = false;
        target->stage = Stage::Release;
    }
}

void SynthProcessor::noteBend(const ProcessEvent& event) {
    if (Voice* target = voiceOf(event, false)) {
        target->bendTarget = std::clamp(static_cast<double>(event.bend), -kMaxBendSemitones, kMaxBendSemitones);
    }
}

void SynthProcessor::renderVoices(int from, int to, Waveform wave, const Envelope& env) {
    for (Voice& voice : voices_) {
        if (!voice.active) continue;
        for (int i = from; i < to; ++i) {
            if (voice.bend != voice.bendTarget) {  // gliding to its note's bend
                voice.bend += (voice.bendTarget - voice.bend) * bendGlide_;
                if (std::abs(voice.bendTarget - voice.bend) < 1e-6) voice.bend = voice.bendTarget;
                voice.increment = voice.keyIncrement * std::exp2(voice.bend / 12.0);
            }
            const double dt = voice.increment;
            switch (voice.stage) {
                case Stage::Attack:
                    voice.level += env.attackStep;
                    if (voice.level >= 1.f) {
                        voice.level = 1.f;
                        voice.stage = Stage::Decay;
                    }
                    break;
                case Stage::Decay:  // then holds the sustain level (and follows it if it changes)
                    voice.level = env.sustain + (voice.level - env.sustain) * env.decayCoef;
                    break;
                case Stage::Release:
                    voice.level *= env.releaseCoef;
                    break;
            }

            const double t = voice.phase;
            double sample = 0.0;
            switch (wave) {
                case Waveform::Sine: sample = std::sin(2.0 * kPi * t); break;
                case Waveform::Triangle: sample = 1.0 - 4.0 * std::abs(t - 0.5); break;
                case Waveform::Saw: sample = 2.0 * t - 1.0 - polyBlep(t, dt); break;
                case Waveform::Square: {
                    const double half = t < 0.5 ? t + 0.5 : t - 0.5;
                    sample = (t < 0.5 ? 1.0 : -1.0) + polyBlep(t, dt) - polyBlep(half, dt);
                    break;
                }
            }
            voice.phase += dt;
            if (voice.phase >= 1.0) voice.phase -= 1.0;

            // Low-pass: a state-variable filter, stable at any cutoff and resonance.
            const float low = voice.filter.tick(filter_[static_cast<size_t>(i)], static_cast<float>(sample)).low;
            mix_[i] += low * voice.level * voice.gain;
        }
        const bool faded =
            voice.stage == Stage::Release || (voice.stage == Stage::Decay && env.sustain < dsp::kSilent);
        if (faded && voice.level < dsp::kSilent) voice.active = false;
    }
}

void SynthProcessor::render(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) {
    const bool silent = ctx.inEvents.count == 0 &&
                        std::none_of(voices_.begin(), voices_.end(), [](const Voice& voice) { return voice.active; });
    if (silent) {  // nothing playing: skip the work, and start the next note at the current settings
        cutoff_ = param(Cutoff);
        volume_.snapTo(dbToGain(param(Volume)));
        for (int c = 0; c < std::min(numChannels, 2); ++c) std::fill_n(channels[c], numFrames, 0.f);
        return;
    }

    const int frames = std::min(numFrames, static_cast<int>(mix_.size()));  // prepare() said no more
    const auto wave = static_cast<Waveform>(std::clamp(choiceIndex(Wave), 0, 3));
    const Envelope env = envelope();

    // Filter coefficients for each sample: the cutoff glides to its target so
    // turning the knob doesn't step; the resonance applies per block.
    const float target = param(Cutoff);
    const auto highest = static_cast<float>(sampleRate_ * 0.45);
    const float damping = 2.f - 1.9f * param(Resonance) / 100.f;  // 2 (none) .. 0.1 (Q = 10)
    for (int i = 0; i < frames; ++i) {
        cutoff_ += (target - cutoff_) * cutoffGlide_;
        const auto g = static_cast<float>(std::tan(kPi * std::min(cutoff_, highest) / sampleRate_));
        filter_[static_cast<size_t>(i)] = dsp::SvfCoefficients(g, damping);
    }

    std::fill_n(mix_.data(), frames, 0.f);
    dsp::renderBetweenNotes(
        ctx.inEvents, frames, [&](int from, int to) { renderVoices(from, to, wave, env); },
        [&](const ProcessEvent& event) { noteOn(event); }, [&](const ProcessEvent& event) { noteOff(event); },
        [&](const ProcessEvent& event) { noteBend(event); });

    volume_.setTarget(dbToGain(param(Volume)));
    float* left = channels[0];
    float* right = numChannels > 1 ? channels[1] : nullptr;
    for (int i = 0; i < numFrames; ++i) {
        const float out = i < frames ? mix_[i] * volume_.next() : 0.f;
        left[i] = out;
        if (right) right[i] = out;
    }
}

SUB_REGISTER_BUILTIN(SynthProcessor, Instrument);

}  // namespace sub
