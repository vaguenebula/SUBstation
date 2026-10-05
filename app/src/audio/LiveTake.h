#pragma once
// A take while it records, as the arrangement draws it (EngineBridge::liveTakes):
// an audio take's peaks, or a MIDI take's notes so far.

#include <QString>
#include <QtGlobal>

#include <vector>

namespace sub::app {

// A note of a MIDI take while it records, in timeline samples.
struct LiveNote {
    qint64 start = 0;
    qint64 end = -1;  // -1: still held
    int key = 60;
    int velocity = 100;
    int channel = 0;

    friend bool operator==(const LiveNote&, const LiveNote&) = default;
};

struct LiveTake {
    // Frames each live peak stands for (the engine's Engine::kRecordPeakFrames).
    static constexpr int kPeakFrames = 128;

    QString trackId;
    qint64 startSample = 0;  // timeline sample of its first frame
    bool started = false;
    qint64 frames = 0;
    // (min, max) per kPeakFrames frames, all channels: 2 floats a peak.
    std::vector<float> peaks;
    bool midi = false;
    std::vector<LiveNote> notes;  // a MIDI take's notes so far

    qint64 peakCount() const { return static_cast<qint64>(peaks.size() / 2); }
    float peakMin(qint64 index) const { return peaks[static_cast<size_t>(2 * index)]; }
    float peakMax(qint64 index) const { return peaks[static_cast<size_t>(2 * index + 1)]; }

    // More peaks ((min, max) pairs); the buffer grows by doubling.
    void addPeaks(const std::vector<float>& more);
};

}  // namespace sub::app
