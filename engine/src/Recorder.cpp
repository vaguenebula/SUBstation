#include "Recorder.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <stdexcept>

#include "PathUtils.h"
#include "miniaudio.h"

namespace sub {

// ---------------------------------------------------------------------------
// SampleRing

SampleRing::SampleRing(size_t minCapacity) {
    size_t capacity = 1;
    while (capacity < minCapacity) capacity <<= 1;
    data_.assign(capacity, 0.f);
    mask_ = capacity - 1;
}

bool SampleRing::write(const float* samples, size_t count) noexcept {
    const size_t head = head_.load(std::memory_order_relaxed);
    if (data_.size() - (head - tail_.load(std::memory_order_acquire)) < count) return false;
    const size_t start = head & mask_;
    const size_t first = std::min(count, data_.size() - start);
    std::copy_n(samples, first, data_.data() + start);
    std::copy_n(samples + first, count - first, data_.data());
    head_.store(head + count, std::memory_order_release);
    return true;
}

size_t SampleRing::read(float* out, size_t count) noexcept {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    count = std::min(count, head_.load(std::memory_order_acquire) - tail);
    const size_t start = tail & mask_;
    const size_t first = std::min(count, data_.size() - start);
    std::copy_n(data_.data() + start, first, out);
    std::copy_n(data_.data(), count - first, out + first);
    tail_.store(tail + count, std::memory_order_release);
    return count;
}

size_t SampleRing::available() const noexcept {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// RecordingTake

RecordingTake::RecordingTake(uint32_t trackId_, std::string path_, int inputLeft, int inputRight, size_t ringFrames,
                             int64_t placement_)
    : trackId(trackId_),
      path(std::move(path_)),
      channels(inputRight >= 0 && inputRight != inputLeft ? 2 : 1),
      inputs{inputLeft, inputRight >= 0 ? inputRight : inputLeft},
      source(Source::Device),
      sourceTrackId(0),
      placement(placement_),
      ring(ringFrames * static_cast<size_t>(channels)) {}

RecordingTake::RecordingTake(uint32_t trackId_, std::string path_, Source source_, uint32_t sourceTrackId_,
                             size_t ringFrames, int64_t placement_)
    : trackId(trackId_),
      path(std::move(path_)),
      channels(2),
      inputs{-1, -1},
      source(source_),
      sourceTrackId(sourceTrackId_),
      placement(placement_),
      ring(ringFrames * static_cast<size_t>(channels)) {}

void RecordingTake::push(const float* left, const float* right, int count, float* scratch) noexcept {
    if (channels == 1) {
        std::copy_n(left, count, scratch);
    } else {
        for (int i = 0; i < count; ++i) {
            scratch[2 * i] = left[i];
            scratch[2 * i + 1] = right[i];
        }
    }
    for (int i = 0; i < count; ++i) {
        const float lo = channels == 2 ? std::min(left[i], right[i]) : left[i];
        const float hi = channels == 2 ? std::max(left[i], right[i]) : left[i];
        if (peakFill == 0) {
            pendingPeak = {lo, hi};
        } else {
            pendingPeak.min = std::min(pendingPeak.min, lo);
            pendingPeak.max = std::max(pendingPeak.max, hi);
        }
        if (++peakFill == kPeakFrames) {
            peaks.push(pendingPeak);  // a full queue (nobody draws) only loses the picture
            peakFill = 0;
        }
    }

    // A gap goes to the writer before the input after it, so the writer sees it
    // in time (it looks for gaps after seeing how much input there is).
    const int64_t at = frames.load(std::memory_order_relaxed);
    bool ok = true;
    if (gapAt >= 0) {
        if (gaps.push({gapAt, gapFrames})) {
            gapAt = -1;
            gapFrames = 0;
        } else {
            ok = false;
        }
    }
    if (!ok || !ring.write(scratch, static_cast<size_t>(count) * channels)) {
        if (gapAt < 0) gapAt = at;
        gapFrames += count;
        dropped.fetch_add(count, std::memory_order_relaxed);
    }
    frames.store(at + count, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// RecordingSession

RecordingSession::RecordingSession(std::vector<std::unique_ptr<RecordingTake>> takes,
                                   std::vector<std::unique_ptr<MidiRecordingTake>> midiTakes, double sampleRate,
                                   int64_t midiPlacement)
    : takes_(std::move(takes)),
      midiTakes_(std::move(midiTakes)),
      sampleRate_(sampleRate),
      midiPlacement_(midiPlacement) {
    try {
        for (auto& take : takes_) {
            const std::filesystem::path path = pathFromUtf8(take->path);
            std::error_code ignored;
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ignored);
            auto* encoder = new ma_encoder;
            const ma_encoder_config config = ma_encoder_config_init(
                ma_encoding_format_wav, ma_format_f32, static_cast<ma_uint32>(take->channels),
                static_cast<ma_uint32>(sampleRate));
            if (ma_encoder_init_file_w(widen(take->path).c_str(), &config, encoder) != MA_SUCCESS) {
                delete encoder;
                throw std::runtime_error("Could not create " + take->path);
            }
            take->encoder = encoder;
        }
    } catch (...) {
        for (auto& take : takes_) {
            if (auto* encoder = static_cast<ma_encoder*>(take->encoder)) {
                ma_encoder_uninit(encoder);
                delete encoder;
                take->encoder = nullptr;
                std::error_code ignored;
                std::filesystem::remove(pathFromUtf8(take->path), ignored);
            }
        }
        throw;
    }
    writer_ = std::thread([this] { writerLoop(); });
}

RecordingSession::~RecordingSession() { finish(); }

void RecordingSession::writerLoop() {
    std::vector<float> buffer(size_t{1} << 15);
    std::unique_lock lock(mutex_);
    while (!stop_) {
        lock.unlock();
        for (auto& take : takes_) drain(*take, buffer);
        lock.lock();
        wake_.wait_for(lock, std::chrono::milliseconds(10), [this] { return stop_; });
    }
}

void RecordingSession::drain(RecordingTake& take, std::vector<float>& buffer) {
    if (!take.encoder) return;
    const auto channels = static_cast<size_t>(take.channels);
    for (;;) {
        // How much input there is first: any gap before it is visible after this.
        size_t available = take.ring.available() / channels;
        if (!take.hasNextGap) take.hasNextGap = take.gaps.pop(take.nextGap);
        if (take.hasNextGap && take.nextGap.at <= take.written) {
            writeSilence(take, take.nextGap.frames, buffer);
            take.hasNextGap = false;
            continue;
        }
        if (take.hasNextGap) available = std::min<size_t>(available, static_cast<size_t>(take.nextGap.at - take.written));
        const size_t frames = std::min(available, buffer.size() / channels);
        if (frames == 0) break;
        take.ring.read(buffer.data(), frames * channels);
        writeFrames(take, buffer.data(), static_cast<int64_t>(frames));
    }
}

void RecordingSession::writeSilence(RecordingTake& take, int64_t frames, std::vector<float>& buffer) {
    std::fill(buffer.begin(), buffer.end(), 0.f);
    const auto chunk = static_cast<int64_t>(buffer.size()) / take.channels;
    for (int64_t left = frames; left > 0;) {
        const int64_t n = std::min(left, chunk);
        writeFrames(take, buffer.data(), n);
        left -= n;
    }
}

void RecordingSession::writeFrames(RecordingTake& take, const float* samples, int64_t frames) {
    auto* encoder = static_cast<ma_encoder*>(take.encoder);
    ma_uint64 done = 0;
    if (ma_encoder_write_pcm_frames(encoder, samples, static_cast<ma_uint64>(frames), &done) != MA_SUCCESS ||
        done != static_cast<ma_uint64>(frames)) {
        if (take.error.empty()) take.error = "Could not write " + take.path + " (is the disk full?)";
    }
    take.written += frames;  // in time even if the disk failed: the error says so
}

std::vector<RecordedTake> RecordingSession::finish() {
    {
        std::lock_guard lock(mutex_);
        if (finished_) return {};
        finished_ = stop_ = true;
    }
    wake_.notify_all();
    if (writer_.joinable()) writer_.join();

    std::vector<float> buffer(size_t{1} << 15);
    std::vector<RecordedTake> results;
    for (auto& take : takes_) {
        drain(*take, buffer);
        auto* encoder = static_cast<ma_encoder*>(take->encoder);
        if (!encoder) continue;
        ma_encoder_uninit(encoder);
        delete encoder;
        take->encoder = nullptr;

        RecordedTake result;
        result.trackId = take->trackId;
        result.path = take->path;
        result.channels = take->channels;
        result.sampleRate = sampleRate_;
        result.droppedFrames = take->dropped.load();
        result.error = take->error;
        const int64_t start = take->start.load();
        result.frames = start == RecordingTake::kNotStarted ? 0 : take->written;
        result.startSample = start == RecordingTake::kNotStarted ? 0 : start - take->placement;
        if (result.frames == 0) {
            std::error_code ignored;
            std::filesystem::remove(pathFromUtf8(take->path), ignored);
        }
        results.push_back(std::move(result));
    }
    for (auto& take : midiTakes_) {
        RecordedTake result;
        result.trackId = take->trackId;
        result.sampleRate = sampleRate_;
        result.midi = true;
        result.notes = midiNotes(*take);
        const int64_t start = take->start.load();
        if (start != RecordingTake::kNotStarted) {
            result.startSample = start;
            result.frames = take->frames.load();
        }
        const int64_t end = result.startSample + result.frames;
        std::erase_if(result.notes, [&](const RecordedNote& note) { return note.start >= end; });
        for (RecordedNote& note : result.notes) {
            if (note.end < 0 || note.end > end) note.end = end;  // still held: it ends with the take
        }
        if (const int64_t lost = take->dropped.load(); lost > 0) {
            result.error = std::to_string(lost) + " MIDI events were lost while recording";
        }
        results.push_back(std::move(result));
    }
    return results;
}

std::vector<RecordedNote> RecordingSession::midiNotes(MidiRecordingTake& take) {
    take.collect();
    // Where each note was heard against the timeline; none before the take's start.
    const int64_t start = take.start.load();
    std::vector<RecordedNote> notes;
    notes.reserve(take.notes.size());
    for (RecordedNote note : take.notes) {
        note.start = std::max(note.start - midiPlacement_, start);
        if (note.end >= 0) note.end = std::max(note.end - midiPlacement_, note.start + 1);
        notes.push_back(note);
    }
    return notes;
}

// ---------------------------------------------------------------------------
// MidiRecordingTake

void MidiRecordingTake::collect() {
    Event event;
    while (events.pop(event)) {
        if (event.velocity > 0) {
            notes.push_back({event.time, -1, event.key, event.velocity, event.channel});
            continue;
        }
        // A note-off ends the earliest note held on its key (one whose note-on
        // came before the take began has none).
        for (RecordedNote& note : notes) {
            if (note.end < 0 && note.key == event.key && note.channel == event.channel) {
                note.end = std::max(event.time, note.start + 1);
                break;
            }
        }
    }
}

}  // namespace sub
