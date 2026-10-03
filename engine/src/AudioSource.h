#pragma once
// An audio file decoded into memory at the engine sample rate, plus min/max
// peak mipmaps for waveform drawing. Immutable once loaded, so it can be shared
// freely between the UI, snapshots and the audio thread.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sub {

struct AudioFileInfo {
    int64_t frames = 0;        // at the file's own sample rate
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    double duration = 0.0;     // seconds
};

class AudioSource : public std::enable_shared_from_this<AudioSource> {
public:
    static constexpr int kNumPeakLevels = 6;
    static constexpr int kBaseSamplesPerPeak = 32;   // level n holds 32 * 4^n frames per peak

    // Reads only what is needed to report length and format. Throws on failure.
    static AudioFileInfo probe(const std::string& utf8Path);

    // Decodes the whole file to float, resampled to `sampleRate`. Mono files stay
    // mono; anything with more than two channels is downmixed to stereo.
    static std::shared_ptr<AudioSource> load(const std::string& utf8Path, uint32_t sampleRate);

    const std::string& path() const { return path_; }
    uint32_t channels() const { return channels_; }
    int64_t frames() const { return frames_; }
    uint32_t sampleRate() const { return sampleRate_; }
    uint32_t fileSampleRate() const { return fileSampleRate_; }
    double duration() const { return sampleRate_ ? static_cast<double>(frames_) / sampleRate_ : 0.0; }

    // Planar storage: channel c occupies [c * frames, (c + 1) * frames).
    const float* data() const { return data_.data(); }
    const float* channelData(uint32_t channel) const { return data_.data() + static_cast<size_t>(channel) * frames_; }

    int numPeakLevels() const { return kNumPeakLevels; }
    static int samplesPerPeak(int level);
    int64_t numPeaks(int level) const;
    // Layout: [numPeaks][channels][2] with (min, max) pairs.
    const float* peaks(int level) const;

private:
    AudioSource() = default;
    void buildPeaks();

    std::string path_;
    uint32_t channels_ = 0;
    uint32_t sampleRate_ = 0;
    uint32_t fileSampleRate_ = 0;
    int64_t frames_ = 0;
    std::vector<float> data_;
    std::vector<std::vector<float>> peaks_;
};

}  // namespace sub
