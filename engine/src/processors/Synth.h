#pragma once
// Built-in "Synth" instrument: a simple polyphonic subtractive synthesizer.
// One oscillator per voice (sine, triangle, band-limited saw or square), an
// ADSR amplitude envelope and a resonant low-pass filter. Mono, on both channels.

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "Processor.h"
#include "rt/RtUtils.h"

namespace gil {

class SynthProcessor final : public Processor {
public:
    enum Param { Wave = 0, Attack, Decay, Sustain, Release, Cutoff, Resonance, Volume, NumParams };
    enum class Waveform : uint8_t { Sine, Triangle, Saw, Square };
    static constexpr int kMaxVoices = 16;

    SynthProcessor();

    std::string typeId() const override { return "builtin:synth"; }
    std::string name() const override { return "Synth"; }

    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    // Writes (does not add) the synth's output: it is the first device on a MIDI track.
    void process(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) override;
    int tailSamples() const override;

    const std::vector<ParamInfo>& params() const override;
    float getParam(int index) const override;
    void setParam(int index, float value) override;

private:
    enum class Stage : uint8_t { Attack, Decay, Release };  // Decay ends at, and holds, the sustain level

    struct Voice {
        bool active = false;
        bool held = true;  // no note-off yet
        uint8_t key = 0;
        uint64_t started = 0;  // note counter, for stealing the oldest voice
        float gain = 0.f;      // velocity
        double phase = 0.0;    // 0..1
        double increment = 0.0;
        Stage stage = Stage::Attack;
        float level = 0.f;     // envelope
        float ic1 = 0.f, ic2 = 0.f;  // filter state
    };

    // Per-block settings derived from the parameters.
    struct Envelope {
        float attackStep;
        float decayCoef;
        float sustain;
        float releaseCoef;
    };

    void noteOn(uint8_t key, uint8_t velocity);
    void noteOff(uint8_t key);
    void renderVoices(int from, int to, Waveform wave, const Envelope& env);
    Envelope envelope() const;
    float param(Param p) const { return values_[p].load(std::memory_order_relaxed); }

    std::array<std::atomic<float>, NumParams> values_{};
    std::array<Voice, kMaxVoices> voices_{};
    uint64_t noteCounter_ = 0;
    double sampleRate_ = 48000.0;

    // Filter coefficients per sample of the block (the cutoff glides), and the mix.
    std::vector<float> a1_, a2_, a3_, mix_;
    float cutoff_ = 0.f;  // smoothed, Hz
    float cutoffGlide_ = 0.f;
    SmoothedValue volume_;
};

}  // namespace gil
