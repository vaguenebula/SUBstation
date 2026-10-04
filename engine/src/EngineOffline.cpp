// Engine: offline rendering, WAV export, and rendering one track (freezing it).
#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
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

void Engine::prepareOfflineLocked(Renderer& offline, OfflineLines& lines, double startBeat) {
    // Fresh stretchers, as many as live playback has, so an offline render
    // starts from a clean state and leaves the live voices alone. Likewise
    // delay-compensation lines.
    for (int c = 0; c < kNumStretchConfigs; ++c) {
        for (size_t i = 0; i < snapshotHold_->warpVoices[c].size(); ++i) {
            lines.voices[c].push_back(std::make_shared<WarpVoice>(static_cast<StretchConfig>(c), sampleRate_));
        }
    }
    const auto line = [](int samples) { return samples > 0 ? std::make_shared<DelayLine>(samples + 1) : nullptr; };
    for (const EdgeRender& edge : snapshotHold_->edges) {
        lines.delays.push_back(line(edge.compensation));
        lines.deviceDelays.push_back(line(edge.deviceDelay));
    }
    for (const int samples : snapshotHold_->chainDelays) lines.chainDelays.push_back(line(samples));
    offline.setScheduler(scheduler_.get());
    offline.setCostOrdering(renderer_.costOrdering());
    offline.prepare(sampleRate_);
    offline.setWarpVoices(&lines.voices);
    offline.setDelayLines(&lines.delays, &lines.deviceDelays, &lines.chainDelays);
    offline.syncTempo(*snapshotHold_);
    offline.setPosition(std::llround(std::max(0.0, startBeat) * snapshotHold_->samplesPerBeat()));
    offline.setPlaying(true);
}

void Engine::renderOfflineLocked(double startBeat, int64_t frames, float* out, bool loop, bool metronome) {
    ScopedNoDenormals noDenormals;
    suspendLiveLocked();
    resetProcessorsLocked();
    ScopeExit resume([this] {
        resetProcessorsLocked();
        resumeLiveLocked();
    });
    Renderer offline;
    OfflineLines lines;
    prepareOfflineLocked(offline, lines, startBeat);
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
    if (endBeat <= startBeat) throw std::invalid_argument("Export range is empty");
    ma_format format;
    switch (bitDepth) {
        case 16: format = ma_format_s16; break;
        case 24: format = ma_format_s24; break;
        case 32: format = ma_format_f32; break;
        default: throw std::invalid_argument("Bit depth must be 16, 24 or 32");
    }

    std::lock_guard lock(mutex_);
    const double spb = snapshotHold_->samplesPerBeat();
    const int64_t total = std::llround((endBeat - startBeat) * spb);

    ma_encoder_config config =
        ma_encoder_config_init(ma_encoding_format_wav, format, 2, static_cast<ma_uint32>(sampleRate_));
    ma_encoder encoder;
    if (ma_encoder_init_file_w(widen(path).c_str(), &config, &encoder) != MA_SUCCESS) {
        throw std::runtime_error("Could not create " + path);
    }

    ScopedNoDenormals noDenormals;
    suspendLiveLocked();
    resetProcessorsLocked();
    ScopeExit resume([this] {
        resetProcessorsLocked();
        resumeLiveLocked();
    });
    Renderer offline;
    OfflineLines lines;
    prepareOfflineLocked(offline, lines, startBeat);

    constexpr int64_t kChunk = 16384;
    std::vector<float> rendered(kChunk * 2);
    for (int64_t lag = snapshotHold_->outputLatency(); lag > 0;) {  // see renderOfflineLocked()
        const int64_t n = std::min(kChunk, lag);
        offline.renderOffline(*snapshotHold_, rendered.data(), n);
        lag -= n;
    }
    std::vector<uint8_t> converted(kChunk * 2 * sizeof(float));
    const ma_dither_mode dither = bitDepth == 16 ? ma_dither_mode_triangle : ma_dither_mode_none;
    for (int64_t done = 0; done < total;) {
        const int64_t n = std::min(kChunk, total - done);
        offline.renderOffline(*snapshotHold_, rendered.data(), n);
        ma_pcm_convert(converted.data(), format, rendered.data(), ma_format_f32, static_cast<ma_uint64>(n * 2), dither);
        ma_encoder_write_pcm_frames(&encoder, converted.data(), static_cast<ma_uint64>(n), nullptr);
        done += n;
    }
    ma_encoder_uninit(&encoder);
}

// ---------------------------------------------------------------------------
// One track (freezing)

void Engine::renderTrackLocked(uint32_t trackId, double startBeat, int64_t frames,
                               const std::function<void(const float*, int64_t)>& sink) {
    arrangementTrackLocked(trackId);  // (throws for an unknown track)
    const RenderSnapshot& snap = *snapshotHold_;
    const auto found = std::find_if(snap.tracks.begin(), snap.tracks.end(),
                                    [trackId](const TrackRender& track) { return track.id == trackId; });
    if (found == snap.tracks.end()) throw std::invalid_argument("Unknown track id " + std::to_string(trackId));
    const int track = static_cast<int>(found - snap.tracks.begin());
    ScopedNoDenormals noDenormals;
    suspendLiveLocked();
    resetProcessorsLocked();
    ScopeExit resume([this] {
        resetProcessorsLocked();
        resumeLiveLocked();
    });
    Renderer offline;
    OfflineLines lines;
    prepareOfflineLocked(offline, lines, startBeat);
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
    if (endBeat <= startBeat) throw std::invalid_argument("Render range is empty");
    std::lock_guard lock(mutex_);
    arrangementTrackLocked(trackId);
    const int64_t frames = std::llround((endBeat - startBeat) * snapshotHold_->samplesPerBeat());
    const int64_t tail = std::llround(std::clamp(tailSeconds, 0.0, 600.0) * sampleRate_);

    ma_encoder_config config =
        ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 2, static_cast<ma_uint32>(sampleRate_));
    ma_encoder encoder;
    if (ma_encoder_init_file_w(widen(path).c_str(), &config, &encoder) != MA_SUCCESS) {
        throw std::runtime_error("Could not create " + path);
    }
    ScopeExit close([&encoder] { ma_encoder_uninit(&encoder); });
    // A short write (a full disk) fails the render: the file would be shorter than the frames returned.
    const auto write = [&](const float* samples, int64_t n) {
        ma_uint64 written = 0;
        if (ma_encoder_write_pcm_frames(&encoder, samples, static_cast<ma_uint64>(n), &written) != MA_SUCCESS ||
            written != static_cast<ma_uint64>(n)) {
            throw std::runtime_error("Could not write " + path);
        }
    };
    // The range as it comes; the tail is kept back until it is known where it falls silent.
    int64_t done = 0;
    std::vector<float> tailSamples;
    tailSamples.reserve(static_cast<size_t>(tail) * 2);
    renderTrackLocked(trackId, startBeat, frames + tail, [&](const float* samples, int64_t n) {
        const int64_t inRange = std::clamp<int64_t>(frames - done, 0, n);
        if (inRange > 0) write(samples, inRange);
        tailSamples.insert(tailSamples.end(), samples + inRange * 2, samples + n * 2);
        done += n;
    });
    constexpr float kSilence = 1e-5f;  // -100 dB
    int64_t kept = static_cast<int64_t>(tailSamples.size() / 2);
    while (kept > 0 && std::abs(tailSamples[static_cast<size_t>(kept) * 2 - 2]) < kSilence &&
           std::abs(tailSamples[static_cast<size_t>(kept) * 2 - 1]) < kSilence) {
        --kept;
    }
    if (kept > 0) write(tailSamples.data(), kept);
    return frames + kept;
}

}  // namespace sub
