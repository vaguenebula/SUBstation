#pragma once
// A decoded audio file, as the UI draws it (the arrangement's waveforms, the
// sampler's): a handle on the engine's AudioSource, which is immutable once
// decoded, so the handle can be kept and read from any thread. Its samples are
// at the engine's sample rate; its peaks are min/max pairs at a few levels of
// detail (level n: AudioSource's 32 * 4^n frames a peak).
//
// A null Waveform (isNull()) stands for a file that isn't decoded (yet).

#include <QMetaType>
#include <QString>

#include <memory>

namespace sub {
class AudioSource;
}

namespace sub::app {

class Waveform {
public:
    Waveform() = default;
    explicit Waveform(std::shared_ptr<const sub::AudioSource> source);

    bool isNull() const { return source_ == nullptr; }
    QString path() const;
    qint64 frames() const;
    int channels() const;
    int sampleRate() const;
    double duration() const;  // seconds

    // Samples of channel `channel`, `frames()` of them (planar storage).
    const float* channelData(int channel) const;

    int peakLevels() const;
    static int samplesPerPeak(int level);
    qint64 peakCount(int level) const;
    // Level `level`'s peaks: [peakCount][channels][2] floats, (min, max) pairs.
    const float* peaks(int level) const;

    // The engine's source itself (for the application layer; null for a null Waveform).
    const std::shared_ptr<const sub::AudioSource>& source() const { return source_; }

    friend bool operator==(const Waveform& a, const Waveform& b) { return a.source_ == b.source_; }

private:
    std::shared_ptr<const sub::AudioSource> source_;
};

}  // namespace sub::app

Q_DECLARE_METATYPE(sub::app::Waveform)
