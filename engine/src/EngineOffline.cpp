// Engine: offline rendering and WAV export.
#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "PathUtils.h"
#include "miniaudio.h"

namespace gil {
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

}  // namespace gil
