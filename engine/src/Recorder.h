#pragma once
// Recording. The audio thread copies the input of the tracks being recorded
// into lock-free rings; a disk-writer thread empties them into WAV files. The
// audio thread never touches files, allocates, frees or waits.
//
// Overruns (the writer fell behind and a ring was full): the audio thread drops
// that input, but counts it and tells the writer where the gap is, so the
// writer puts silence there and the rest of the take stays in time. The take
// reports how many frames were lost.
//
// The live waveform comes from here too, not from the file: the audio thread
// sends each take's peaks (min/max of every kPeakFrames frames, all channels)
// through a queue the UI drains.
//
// MIDI tracks record the notes played into them (MidiRecordingTake): the audio
// thread sends each note-on and note-off, at the timeline position where it
// played, through a queue; the edit side pairs them into notes. Nothing is
// written to disk: the notes go into a MIDI clip.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rt/RtUtils.h"

namespace gil {

// The rings' indices sit on separate cache lines (MSVC warns about the padding).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

// Single-producer / single-consumer ring of samples, sized on the edit side.
class SampleRing {
public:
    explicit SampleRing(size_t minCapacity);
    size_t capacity() const noexcept { return data_.size(); }
    // Real-time (producer). All of `count` samples or none.
    bool write(const float* samples, size_t count) noexcept;
    // Consumer. Up to `count` samples; returns how many.
    size_t read(float* out, size_t count) noexcept;
    size_t available() const noexcept;

private:
    std::vector<float> data_;
    size_t mask_ = 0;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

// One track's take while it records.
struct RecordingTake {
    static constexpr int64_t kNotStarted = std::numeric_limits<int64_t>::min();
    static constexpr int kPeakFrames = 128;  // frames per live peak (AudioSource's level 1)

    struct Peak {
        float min = 0.f;
        float max = 0.f;
    };
    struct Gap {
        int64_t at = 0;      // frames into the take
        int64_t frames = 0;  // of silence
    };

    RecordingTake(uint32_t trackId, std::string path, int inputLeft, int inputRight, size_t ringFrames);

    const uint32_t trackId;
    const std::string path;
    const int channels;  // 1 or 2
    const int inputs[2];  // indices into the device's open inputs (callback order)

    SampleRing ring;                   // interleaved samples, audio thread -> writer
    SpscQueue<Gap, 256> gaps;          // overruns, audio thread -> writer
    SpscQueue<Peak, 1 << 16> peaks;    // audio thread -> UI

    // Written by the audio thread.
    std::atomic<int64_t> start{kNotStarted};  // timeline sample of its first frame (before placement)
    std::atomic<int64_t> frames{0};           // frames taken, lost ones included
    std::atomic<int64_t> dropped{0};          // frames lost to overruns

    // Audio-thread-only state.
    Peak pendingPeak;
    int peakFill = 0;
    int64_t gapAt = -1;  // an overrun not yet told to the writer
    int64_t gapFrames = 0;

    // Writer-thread state (read by the edit side once the writer has finished).
    void* encoder = nullptr;  // ma_encoder
    int64_t written = 0;      // frames in the file
    Gap nextGap;              // taken from `gaps`, not reached yet
    bool hasNextGap = false;
    std::string error;

    // Real-time: one stretch of input (planar, `count` frames each), as the
    // next frames of the take.
    void push(const float* left, const float* right, int count, float* scratch) noexcept;
};

// A recorded note, in timeline samples.
struct RecordedNote {
    int64_t start = 0;
    int64_t end = -1;  // -1: still held
    uint8_t key = 60;
    uint8_t velocity = 100;
    uint8_t channel = 0;
};

// One MIDI track's take while it records.
struct MidiRecordingTake {
    struct Event {
        int64_t time = 0;  // timeline sample where it played (before placement)
        uint8_t channel = 0;
        uint8_t key = 0;
        uint8_t velocity = 0;  // 0: a note-off
    };

    explicit MidiRecordingTake(uint32_t trackId) : trackId(trackId) {}

    const uint32_t trackId;
    SpscQueue<Event, 1 << 14> events;  // audio thread -> edit side

    // Written by the audio thread.
    std::atomic<int64_t> start{RecordingTake::kNotStarted};  // timeline sample where it began
    std::atomic<int64_t> frames{0};                          // how long it has recorded
    std::atomic<int64_t> dropped{0};                         // events lost (nobody took them in time)

    // Edit side: the notes so far (before placement), in the order they began.
    std::vector<RecordedNote> notes;
    void collect();  // pairs the events that came in

    // Real-time.
    void push(const Event& event) noexcept {
        if (!events.push(event)) dropped.fetch_add(1, std::memory_order_relaxed);
    }
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

// What a finished take left: a WAV file, or a MIDI take's notes, and where it
// goes on the timeline.
struct RecordedTake {
    uint32_t trackId = 0;
    std::string path;         // empty for a MIDI take
    int64_t startSample = 0;  // timeline sample of its first frame (audio: latency-corrected; may be negative)
    int64_t frames = 0;       // 0: nothing was recorded (no file)
    int channels = 0;
    double sampleRate = 0.0;
    int64_t droppedFrames = 0;  // lost to overruns (silence in the file)
    std::string error;          // the file could not be written
    bool midi = false;
    // A MIDI take's notes, latency-corrected, in timeline samples, within the
    // take; notes still held when it ended end with it.
    std::vector<RecordedNote> notes;
};

// The tracks being recorded together, and their disk writer.
class RecordingSession {
public:
    // Creates the files (throws std::runtime_error with a message for the user).
    // `placement`: how much later than the timeline the input arrives (the output
    // lag, the output and the input latency), subtracted from the takes' starts.
    // `midiPlacement`: likewise for MIDI notes (the output lag, the output
    // latency and MIDI input's delay), subtracted from the notes' times. A MIDI
    // take itself spans the timeline where it recorded.
    RecordingSession(std::vector<std::unique_ptr<RecordingTake>> takes,
                     std::vector<std::unique_ptr<MidiRecordingTake>> midiTakes, double sampleRate,
                     int64_t placement, int64_t midiPlacement);
    ~RecordingSession();
    RecordingSession(const RecordingSession&) = delete;
    RecordingSession& operator=(const RecordingSession&) = delete;

    std::vector<std::unique_ptr<RecordingTake>>& takes() noexcept { return takes_; }
    std::vector<std::unique_ptr<MidiRecordingTake>>& midiTakes() noexcept { return midiTakes_; }
    int64_t placement() const noexcept { return placement_; }
    // Edit side: a MIDI take's notes so far, latency-corrected as
    // RecordedTake::notes has them (held notes keep end -1).
    std::vector<RecordedNote> midiNotes(MidiRecordingTake& take);
    double sampleRate() const noexcept { return sampleRate_; }

    // Real-time: the playhead jumped while recording, so the takes end there.
    void interrupt() noexcept { interrupted_.store(true, std::memory_order_relaxed); }
    bool interrupted() const noexcept { return interrupted_.load(std::memory_order_relaxed); }

    // Edit side, once the audio thread can no longer see the session: writes
    // what is left, closes the files, and says what they hold.
    std::vector<RecordedTake> finish();

private:
    void writerLoop();
    void drain(RecordingTake& take, std::vector<float>& buffer);
    static void writeFrames(RecordingTake& take, const float* samples, int64_t frames);
    static void writeSilence(RecordingTake& take, int64_t frames, std::vector<float>& buffer);

    std::vector<std::unique_ptr<RecordingTake>> takes_;
    std::vector<std::unique_ptr<MidiRecordingTake>> midiTakes_;
    double sampleRate_;
    int64_t placement_;
    int64_t midiPlacement_;
    std::atomic<bool> interrupted_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    bool finished_ = false;
    std::thread writer_;
};

}  // namespace gil
