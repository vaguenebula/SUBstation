// Engine: offline rendering, WAV export, and rendering one track (freezing it),
// in the caller's thread or in the background (RenderJob.h).
#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "PathUtils.h"
#include "miniaudio.h"

namespace sub {
namespace {

// Runs a callable when the scope ends (used to resume live output).
template <typename F>
class ScopeExit {
public:
    explicit ScopeExit(F f) : f_(std::move(f)) {}
    ~ScopeExit() { f_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    F f_;
};

// Frames rendered at a time in the background: a cancel is heard this soon.
constexpr int64_t kJobChunk = 4096;

// A stereo WAV file being written. Unless kept, it is deleted when this goes:
// a render cancelled, or failed, leaves nothing behind.
class WavWriter {
public:
    WavWriter(std::string path, ma_format format, double sampleRate) : path_(std::move(path)), format_(format) {
        ma_encoder_config config =
            ma_encoder_config_init(ma_encoding_format_wav, format, 2, static_cast<ma_uint32>(sampleRate));
        if (ma_encoder_init_file_w(widen(path_).c_str(), &config, &encoder_) != MA_SUCCESS) {
            throw std::runtime_error("Could not create " + path_);
        }
    }
    ~WavWriter() {
        close();
        if (!kept_) {
            std::error_code ignored;
            std::filesystem::remove(pathFromUtf8(path_), ignored);
        }
    }
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    // Interleaved float frames, in the file's format (dithered to 16 bits). A
    // short write (a full disk) throws: the file would be shorter than the render.
    void write(const float* samples, int64_t frames) {
        const void* data = samples;
        if (format_ != ma_format_f32) {
            converted_.resize(static_cast<size_t>(frames) * 2 * ma_get_bytes_per_sample(format_));
            ma_pcm_convert(converted_.data(), format_, samples, ma_format_f32, static_cast<ma_uint64>(frames * 2),
                           format_ == ma_format_s16 ? ma_dither_mode_triangle : ma_dither_mode_none);
            data = converted_.data();
        }
        ma_uint64 written = 0;
        if (ma_encoder_write_pcm_frames(&encoder_, data, static_cast<ma_uint64>(frames), &written) != MA_SUCCESS ||
            written != static_cast<ma_uint64>(frames)) {
            throw std::runtime_error("Could not write " + path_);
        }
    }
    void keep() {  // done: the file stays
        close();
        kept_ = true;
    }

private:
    void close() {
        if (open_) ma_encoder_uninit(&encoder_);
        open_ = false;
    }

    std::string path_;
    ma_format format_;
    ma_encoder encoder_{};
    bool open_ = true;
    bool kept_ = false;
    std::vector<uint8_t> converted_;
};

ma_format exportFormat(int bitDepth) {
    switch (bitDepth) {
        case 16: return ma_format_s16;
        case 24: return ma_format_s24;
        case 32: return ma_format_f32;
        default: throw std::invalid_argument("Bit depth must be 16, 24 or 32");
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Offline rendering

void Engine::suspendLiveLocked() {
    finishRecordingLocked();  // its input would have a hole
    if (!deviceRunning_) return;
    liveSuspended_.store(true, std::memory_order_seq_cst);
    // Once the epoch advances, any callback that started before the flag was
    // visible has finished; later callbacks output silence.
    waitForCallbackLocked();
}

void Engine::resumeLiveLocked() { liveSuspended_.store(false, std::memory_order_seq_cst); }

void Engine::resetProcessorsLocked() {
    // Offline renders share the processors with live playback. Resetting them
    // before keeps live notes (and a plug-in's reverb tail) out of the render;
    // after, keeps the render's last notes from hanging in live playback.
    for (auto& [id, entry] : processors_) {
        entry.processor->resetOffline();
        entry.processor->requestReset();
    }
}

void Engine::checkNotRenderingLocked() const {
    if (job_ != nullptr) throw std::runtime_error("Wait for the render to finish");
}

void Engine::prepareOfflineLocked(const RenderSnapshot& snap, Renderer& offline, OfflineLines& lines,
                                  double startBeat) {
    // Fresh stretchers, as many as live playback has, so an offline render
    // starts from a clean state and leaves the live voices alone. Likewise
    // delay-compensation lines.
    for (int c = 0; c < kNumStretchConfigs; ++c) {
        for (size_t i = 0; i < snap.warpVoices[c].size(); ++i) {
            lines.voices[c].push_back(std::make_shared<WarpVoice>(static_cast<StretchConfig>(c), sampleRate_));
        }
    }
    const auto line = [](int samples) { return samples > 0 ? std::make_shared<DelayLine>(samples + 1) : nullptr; };
    for (const EdgeRender& edge : snap.edges) {
        lines.delays.push_back(line(edge.compensation));
        lines.deviceDelays.push_back(line(edge.deviceDelay));
    }
    for (const int samples : snap.chainDelays) lines.chainDelays.push_back(line(samples));
    offline.setScheduler(scheduler_.get());
    offline.setCostOrdering(renderer_.costOrdering());
    offline.prepare(sampleRate_);
    offline.setWarpVoices(&lines.voices);
    offline.setDelayLines(&lines.delays, &lines.deviceDelays, &lines.chainDelays);
    offline.syncTempo(snap);
    offline.setPosition(std::llround(std::max(0.0, startBeat) * snap.samplesPerBeat()));
    offline.setPlaying(true);
}

void Engine::renderOfflineLocked(double startBeat, int64_t frames, float* out, bool loop, bool metronome) {
    checkNotRenderingLocked();
    ScopedNoDenormals noDenormals;
    suspendLiveLocked();
    resetProcessorsLocked();
    ScopeExit resume([this] { endOfflineLocked(); });
    Renderer offline;
    OfflineLines lines;
    prepareOfflineLocked(*snapshotHold_, offline, lines, startBeat);
    // With delay compensation the output lags the timeline: render the lag first and drop it.
    if (const int64_t lag = snapshotHold_->outputLatency(); lag > 0) {
        std::vector<float> discarded(static_cast<size_t>(lag) * 2);
        offline.renderOffline(*snapshotHold_, discarded.data(), lag, loop, metronome);
    }
    offline.renderOffline(*snapshotHold_, out, frames, loop, metronome);
}

std::vector<float> Engine::renderOffline(double startBeat, int64_t frames, bool loop, bool metronome) {
    if (frames < 0) throw std::invalid_argument("frames must be >= 0");
    std::vector<float> out(static_cast<size_t>(frames) * 2);
    std::lock_guard lock(mutex_);
    renderOfflineLocked(startBeat, frames, out.data(), loop, metronome);
    return out;
}

void Engine::exportWav(const std::string& path, double startBeat, double endBeat, int bitDepth) {
    startExport(path, startBeat, endBeat, bitDepth)->finish();
}

// ---------------------------------------------------------------------------
// One track (freezing)

void Engine::renderTrackLocked(uint32_t trackId, double startBeat, int64_t frames,
                               const std::function<void(const float*, int64_t)>& sink) {
    checkNotRenderingLocked();
    arrangementTrackLocked(trackId);  // (throws for an unknown track)
    const RenderSnapshot& snap = *snapshotHold_;
    const auto found = std::find_if(snap.tracks.begin(), snap.tracks.end(),
                                    [trackId](const TrackRender& track) { return track.id == trackId; });
    if (found == snap.tracks.end()) throw std::invalid_argument("Unknown track id " + std::to_string(trackId));
    const int track = static_cast<int>(found - snap.tracks.begin());
    ScopedNoDenormals noDenormals;
    suspendLiveLocked();
    resetProcessorsLocked();
    ScopeExit resume([this] { endOfflineLocked(); });
    Renderer offline;
    OfflineLines lines;
    prepareOfflineLocked(snap, offline, lines, startBeat);
    // Its signal before its fader lags the timeline by what feeds it (a group's
    // bus hears its tracks that late) and its own devices: render that first and drop it.
    constexpr int64_t kChunk = 16384;
    std::vector<float> rendered(kChunk * 2);
    for (int64_t lag = found->inputLatency + found->latency; lag > 0;) {
        const int64_t n = std::min(kChunk, lag);
        offline.renderTrackOffline(snap, track, rendered.data(), n);
        lag -= n;
    }
    for (int64_t done = 0; done < frames;) {
        const int64_t n = std::min(kChunk, frames - done);
        offline.renderTrackOffline(snap, track, rendered.data(), n);
        sink(rendered.data(), n);
        done += n;
    }
}

std::vector<float> Engine::renderTrackOffline(uint32_t trackId, double startBeat, int64_t frames) {
    if (frames < 0) throw std::invalid_argument("frames must be >= 0");
    std::vector<float> out;
    out.reserve(static_cast<size_t>(frames) * 2);
    std::lock_guard lock(mutex_);
    renderTrackLocked(trackId, startBeat, frames,
                      [&out](const float* samples, int64_t n) { out.insert(out.end(), samples, samples + n * 2); });
    return out;
}

int64_t Engine::renderTrackToWav(uint32_t trackId, const std::string& path, double startBeat, double endBeat,
                                 double tailSeconds) {
    return startTrackRender(trackId, path, startBeat, endBeat, tailSeconds)->finish().value_or(0);
}

// ---------------------------------------------------------------------------
// In the background

std::shared_ptr<Engine::OfflineRender> Engine::beginOfflineLocked(double startBeat) {
    suspendLiveLocked();
    try {
        resetProcessorsLocked();
        auto render = std::make_shared<OfflineRender>();
        render->snapshot = snapshotHold_;
        prepareOfflineLocked(*render->snapshot, render->renderer, render->lines, startBeat);
        return render;
    } catch (...) {
        endOfflineLocked();
        throw;
    }
}

void Engine::endOfflineLocked() {
    resetProcessorsLocked();
    resumeLiveLocked();
}

std::shared_ptr<RenderJob> Engine::startJobLocked(const std::string& path, int64_t total,
                                                  std::function<std::optional<int64_t>(RenderJob&)> body) {
    std::shared_ptr<RenderJob> job(new RenderJob(this, path, total));
    try {
        job->start(std::move(body));
    } catch (...) {
        job->engine_ = nullptr;  // (nothing to give back: the render ends here)
        endOfflineLocked();
        throw;
    }
    job_ = job.get();
    return job;
}

bool Engine::isRendering() {
    std::lock_guard lock(mutex_);
    return job_ != nullptr;
}

void Engine::endJob(RenderJob& job) {
    std::lock_guard lock(mutex_);
    if (job_ == &job) job_ = nullptr;
    endOfflineLocked();
}

std::shared_ptr<RenderJob> Engine::startExport(const std::string& path, double startBeat, double endBeat,
                                               int bitDepth) {
    if (endBeat <= startBeat) throw std::invalid_argument("Export range is empty");
    const ma_format format = exportFormat(bitDepth);
    std::lock_guard lock(mutex_);
    checkNotRenderingLocked();
    auto writer = std::make_shared<WavWriter>(path, format, sampleRate_);
    auto render = beginOfflineLocked(startBeat);
    const int64_t total = std::llround((endBeat - startBeat) * render->snapshot->samplesPerBeat());
    return startJobLocked(path, total, [render, writer, total](RenderJob& job) -> std::optional<int64_t> {
        ScopedNoDenormals noDenormals;
        const RenderSnapshot& snap = *render->snapshot;
        std::vector<float> rendered(kJobChunk * 2);
        // With delay compensation the output lags the timeline: render the lag first and drop it.
        for (int64_t lag = snap.outputLatency(); lag > 0;) {
            const int64_t n = std::min(kJobChunk, lag);
            render->renderer.renderOffline(snap, rendered.data(), n);
            lag -= n;
        }
        for (int64_t done = 0; done < total;) {
            if (job.stopping()) return std::nullopt;
            const int64_t n = std::min(kJobChunk, total - done);
            render->renderer.renderOffline(snap, rendered.data(), n);
            writer->write(rendered.data(), n);
            done += n;
            job.advance(n);
        }
        writer->keep();
        return total;
    });
}

std::shared_ptr<RenderJob> Engine::startTrackRender(uint32_t trackId, const std::string& path, double startBeat,
                                                    double endBeat, double tailSeconds) {
    if (endBeat <= startBeat) throw std::invalid_argument("Render range is empty");
    std::lock_guard lock(mutex_);
    checkNotRenderingLocked();
    arrangementTrackLocked(trackId);  // (throws for an unknown track)
    auto writer = std::make_shared<WavWriter>(path, ma_format_f32, sampleRate_);
    auto render = beginOfflineLocked(startBeat);
    const RenderSnapshot& snap = *render->snapshot;
    const auto found = std::find_if(snap.tracks.begin(), snap.tracks.end(),
                                    [trackId](const TrackRender& track) { return track.id == trackId; });
    if (found == snap.tracks.end()) {
        endOfflineLocked();
        throw std::invalid_argument("Unknown track id " + std::to_string(trackId));
    }
    const int track = static_cast<int>(found - snap.tracks.begin());
    // Its signal before its fader lags the timeline by what feeds it (a group's
    // bus hears its tracks that late) and its own devices: that is rendered first and dropped.
    const int64_t lag = found->inputLatency + found->latency;
    const int64_t frames = std::llround((endBeat - startBeat) * snap.samplesPerBeat());
    const int64_t tail = std::llround(std::clamp(tailSeconds, 0.0, 600.0) * sampleRate_);
    return startJobLocked(path, frames + tail, [render, writer, track, lag, frames, tail](RenderJob& job)
                                                   -> std::optional<int64_t> {
        ScopedNoDenormals noDenormals;
        const RenderSnapshot& snap = *render->snapshot;
        std::vector<float> rendered(kJobChunk * 2);
        for (int64_t left = lag; left > 0;) {
            const int64_t n = std::min(kJobChunk, left);
            render->renderer.renderTrackOffline(snap, track, rendered.data(), n);
            left -= n;
        }
        // The range as it comes; the tail is kept back until it is known where it falls silent.
        std::vector<float> tailSamples;
        tailSamples.reserve(static_cast<size_t>(tail) * 2);
        for (int64_t done = 0; done < frames + tail;) {
            if (job.stopping()) return std::nullopt;
            const int64_t n = std::min(kJobChunk, frames + tail - done);
            render->renderer.renderTrackOffline(snap, track, rendered.data(), n);
            const int64_t inRange = std::clamp<int64_t>(frames - done, 0, n);
            if (inRange > 0) writer->write(rendered.data(), inRange);
            tailSamples.insert(tailSamples.end(), rendered.data() + inRange * 2, rendered.data() + n * 2);
            done += n;
            job.advance(n);
        }
        constexpr float kSilence = 1e-5f;  // -100 dB
        int64_t kept = static_cast<int64_t>(tailSamples.size() / 2);
        while (kept > 0 && std::abs(tailSamples[static_cast<size_t>(kept) * 2 - 2]) < kSilence &&
               std::abs(tailSamples[static_cast<size_t>(kept) * 2 - 1]) < kSilence) {
            --kept;
        }
        if (kept > 0) writer->write(tailSamples.data(), kept);
        writer->keep();
        return frames + kept;
    });
}

// ---------------------------------------------------------------------------
// RenderJob

void RenderJob::start(std::function<std::optional<int64_t>(RenderJob&)> body) {
    thread_ = std::thread([this, body = std::move(body)]() mutable {
        try {
            frames_ = body(*this);
        } catch (...) {
            error_ = std::current_exception();
        }
        body = nullptr;  // what it holds goes now (an unfinished file is deleted)
        rendered_.store(true, std::memory_order_release);
    });
}

double RenderJob::progress() const noexcept {
    if (total_ <= 0) return done() ? 1.0 : 0.0;
    return std::min(1.0, static_cast<double>(done_.load(std::memory_order_relaxed)) / static_cast<double>(total_));
}

void RenderJob::end() {
    if (thread_.joinable()) thread_.join();
    if (Engine* engine = std::exchange(engine_, nullptr)) engine->endJob(*this);
}

std::optional<int64_t> RenderJob::finish() {
    end();
    if (error_) std::rethrow_exception(std::exchange(error_, nullptr));
    return frames_;
}

RenderJob::~RenderJob() {
    cancel();
    try {
        end();
    } catch (...) {
    }
}

}  // namespace sub
