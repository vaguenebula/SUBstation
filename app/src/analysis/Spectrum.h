#pragma once
// Spectra of what devices' displays stream, for their editors' analyzers: the
// Delay's input behind its filter curve, the EQ's input and output behind its
// bands. Each keeps the latest samples, takes a Hann-windowed FFT of them as
// more come (a display refresh at a time), and smooths the levels over time.
// Plain maths on the UI thread: the editors' items draw the result.

#include <cstddef>
#include <vector>

namespace sub::app::analysis {

// The Delay's: the input's latest kFftSize samples, each bin's level in dB
// rising at once and falling kFallDb per refresh.
class FallingSpectrum {
public:
    static constexpr int kFftSize = 4096;
    static constexpr double kFloorDb = -90.0;
    static constexpr double kFallDb = 1.0;  // per display refresh (~60 a second)

    FallingSpectrum();

    // The input's samples since the last call: the spectrum follows them (and
    // falls back without any). Whether it changed.
    bool add(const float* samples, std::size_t count, double sampleRate);
    double sampleRate() const { return sampleRate_; }
    // dB per bin (kFftSize / 2 + 1).
    const std::vector<float>& levels() const { return spectrum_; }
    // The spectrum in dB under each of `columns` columns across `low`..`high` Hz
    // (log): the loudest bin under a column, or (low down, where a bin spans
    // columns) interpolated between bins.
    std::vector<double> columns(int columns, double low, double high) const;

private:
    double sampleRate_ = 48000.0;
    std::vector<float> samples_;
    std::vector<double> window_;
    double scale_ = 0.0;
    std::vector<float> spectrum_;
};

// The EQ's analyzer: spectra of the input and the output, a Hann-windowed FFT
// of the latest samples, rising fast and falling back slowly, as Pro-Q's does;
// shown tilted kTilt dB/octave so music looks about level.
class EqAnalyzer {
public:
    enum Channel { Input = 0, Output = 1 };
    static constexpr int kFftSize = 8192;
    static constexpr double kFloorDb = -96.0;  // dBFS (after the tilt) at the bottom of the graph
    static constexpr double kCeilDb = 6.0;     // and at the top
    static constexpr double kTilt = 4.5;       // dB/octave around 1 kHz, as Pro-Q's
    static constexpr double kRise = 0.55;      // per display refresh, of the way to the latest
    static constexpr double kFall = 0.09;
    static constexpr double kTiltFade = 18.0;  // dB above the floor over which the tilt comes in

    EqAnalyzer();

    double sampleRate() const { return sampleRate_; }
    // A channel's samples since the last refresh (none: it falls back).
    void feed(Channel channel, const float* samples, std::size_t count, double sampleRate);
    // Whether it shows anything above the floor.
    bool live(Channel channel) const;
    const std::vector<float>& levels(Channel channel) const { return levels_[channel]; }
    // The spectrum in dB (tilted) at each of the graph's column frequencies:
    // the loudest bin between a column and the next, or (low down, where a bin
    // spans columns) between bins. Tilted, but not near the floor: silence (or
    // a spectrum falling back to it) stays flat instead of the tilt lifting its
    // high end into view.
    std::vector<double> columns(Channel channel, const std::vector<double>& freqs) const;

private:
    double sampleRate_ = 48000.0;
    std::vector<double> window_;
    double scale_ = 0.0;
    std::vector<float> samples_[2];
    std::vector<float> levels_[2];
};

}  // namespace sub::app::analysis
