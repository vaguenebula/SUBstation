// Engine: the audio device, and the callback it drives.
#include "Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace gil {

// ---------------------------------------------------------------------------
// Device

std::vector<AudioDeviceInfo> Engine::devices(const std::string& driver) {
    std::lock_guard lock(mutex_);
    return device_.devices(driver);
}

void Engine::openDevice(const DeviceConfig& config) {
    std::lock_guard lock(mutex_);
    openDeviceLocked(config);
}

void Engine::reopenDevice() {
    std::lock_guard lock(mutex_);
    if (!device_.hasConfig()) throw std::runtime_error("No audio device has been opened");
    openDeviceLocked(device_.resetConfig());  // asks the open driver first, so before closing it
}

void Engine::openDeviceLocked(const DeviceConfig& config) {
    // A driver's control panel may run a message loop that calls us back: its
    // driver has to stay until the panel is closed.
    if (device_.inControlPanel()) throw std::runtime_error("Close the driver's control panel first");
    closeDeviceLocked();
    device_.open(config, this);
    try {
        const DeviceState state = device_.state();
        const double rate = state.sampleRate;
        if (rate != sampleRate_) {
            sampleRate_ = rate;
            reloadSourcesLocked();
            warpVoices_ = {};  // stretchers are sized for the old rate; the next snapshot makes new ones
        }
        openInputs_ = static_cast<uint32_t>(state.inputChannels.size());
        openInputChannels_ = state.inputChannels;
        for (auto& peak : shared_.inputPeaks) peak.store(0.f, std::memory_order_relaxed);
        // The audio thread is not running, so the preview can be dropped directly.
        shared_.previewSource.store(nullptr, std::memory_order_seq_cst);
        shared_.previewSerial.fetch_add(1, std::memory_order_seq_cst);
        shared_.previewActive.store(false);
        previewHold_.reset();

        renderer_.prepare(sampleRate_);
        for (auto& [id, entry] : processors_) entry.processor->prepare(sampleRate_, Renderer::kMaxBlock);
        // MIDI input plays a device buffer after it arrives (MidiInput.h).
        shared_.midiInputDelay.store(state.bufferFrames > 0 ? static_cast<int>(state.bufferFrames) : 512);
        shared_.midiSampleRate.store(sampleRate_);
        rebuildSnapshotLocked();
        serviceTransportIfIdleLocked();
        pendingDeviceEvents_.store(0);  // about the device just closed
        device_.start();
        deviceRunning_ = true;
    } catch (...) {
        device_.close();
        deviceRunning_ = false;
        openInputs_ = 0;
        openInputChannels_.clear();
        throw;
    }
}

void Engine::closeDevice() {
    std::lock_guard lock(mutex_);
    if (device_.inControlPanel()) throw std::runtime_error("Close the driver's control panel first");
    closeDeviceLocked();
}

void Engine::closeDeviceLocked() {
    if (device_.isOpen()) device_.close();  // waits for the audio thread to exit
    shared_.clock.stop();  // MIDI input is dropped from here on
    deviceRunning_ = false;
    openInputs_ = 0;
    openInputChannels_.clear();
    finishRecordingLocked();  // a device change (or a new sample rate) ends a recording
    collectGarbageLocked();
    serviceTransportIfIdleLocked();
}

DeviceStatus Engine::deviceStatus() {
    std::lock_guard lock(mutex_);
    DeviceStatus status;
    status.open = deviceRunning_;
    if (device_.isOpen()) {
        const DeviceState state = device_.state();
        status.name = state.name;
        status.backend = state.driver;
        status.sampleRate = state.sampleRate;
        status.bufferFrames = state.bufferFrames;
        if (state.sampleRate > 0) {
            status.latencyMs = state.outputLatency * 1000.0 / state.sampleRate;
            status.inputLatencyMs = state.inputLatency * 1000.0 / state.sampleRate;
        }
        status.inputChannels = state.inputChannels;
        status.outputChannels = state.outputChannels;
        status.exclusive = state.exclusive;
    }
    return status;
}

DeviceCaps Engine::deviceCapabilities() {
    std::lock_guard lock(mutex_);
    return device_.state().capabilities;
}

bool Engine::showDeviceControlPanel() {
    // Holds the lock: nothing may close the driver while its panel is up.
    std::lock_guard lock(mutex_);
    return device_.showControlPanel();
}

double Engine::sampleRate() {
    std::lock_guard lock(mutex_);
    return sampleRate_;
}

std::string Engine::takeDeviceEvent() {
    std::lock_guard lock(mutex_);
    const auto take = [this](DeviceEvent event) {
        const auto flag = static_cast<uint32_t>(event);
        return (pendingDeviceEvents_.fetch_and(~flag) & flag) != 0;
    };
    if (take(DeviceEvent::Stopped)) {
        pendingDeviceEvents_.store(0);  // the rest were about the device that is gone
        return "stopped";
    }
    if (!deviceRunning_) {
        pendingDeviceEvents_.store(0);
        return "";
    }
    if (!device_.inControlPanel() && take(DeviceEvent::ResetRequest)) return "reset";
    if (take(DeviceEvent::Rerouted)) return "rerouted";
    if (take(DeviceEvent::LatencyChanged)) {
        device_.refreshLatencies();
        return "latency";
    }
    return "";
}

std::vector<float> Engine::takeInputMeters() {
    std::lock_guard lock(mutex_);
    const size_t count = std::min<size_t>(openInputs_, SharedState::kMaxInputMeters);
    std::vector<float> peaks(count);
    for (size_t c = 0; c < count; ++c) peaks[c] = shared_.inputPeaks[c].exchange(0.f);
    return peaks;
}

std::vector<float> Engine::masterScope(size_t frames) const {
    frames = std::min(frames, SharedState::kScopeSize / 2);
    const uint64_t written = shared_.scopeWritten.load(std::memory_order_acquire);
    std::vector<float> samples(frames, 0.f);
    const size_t available = static_cast<size_t>(std::min<uint64_t>(written, frames));
    const uint64_t first = written - available;
    for (size_t i = 0; i < available; ++i) {
        samples[frames - available + i] =
            shared_.scope[(first + i) & (SharedState::kScopeSize - 1)].load(std::memory_order_relaxed);
    }
    return samples;
}

void Engine::deviceEvent(DeviceEvent event) noexcept {
    pendingDeviceEvents_.fetch_or(static_cast<uint32_t>(event));
}

void Engine::audioCallback(const AudioIO& io) noexcept {
    ScopedNoDenormals noDenormals;
    const auto started = std::chrono::steady_clock::now();
    shared_.clock.update(io.hostTimeNs, io.sampleTime);
    const uint32_t metered = std::min<uint32_t>(io.numInputs, SharedState::kMaxInputMeters);
    for (uint32_t c = 0; c < metered; ++c) {
        float peak = 0.f;
        for (uint32_t i = 0; i < io.frames; ++i) peak = std::max(peak, std::abs(io.inputs[c][i]));
        atomicStoreMax(shared_.inputPeaks[c], peak);
    }
    // seq_cst pairs with the store/epoch-load sequence in rebuildSnapshotLocked().
    const RenderSnapshot* snap = snapshot_.load(std::memory_order_seq_cst);
    RecordingSession* recording = liveRecording_.load(std::memory_order_seq_cst);
    if (!snap || liveSuspended_.load(std::memory_order_seq_cst)) {
        for (uint32_t c = 0; c < io.numOutputs; ++c) std::fill_n(io.outputs[c], io.frames, 0.f);
    } else {
        renderer_.processLive(*snap, shared_, io, recording);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const double budget = io.frames / snap->sampleRate;
        const float load = static_cast<float>(elapsed / budget);
        const float previous = shared_.cpuLoad.load(std::memory_order_relaxed);
        shared_.cpuLoad.store(previous + 0.1f * (load - previous), std::memory_order_relaxed);
    }
    audioEpoch_.fetch_add(1, std::memory_order_seq_cst);
}

void Engine::waitForCallbackLocked() {
    if (!deviceRunning_) return;
    const uint64_t start = audioEpoch_.load(std::memory_order_seq_cst);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (audioEpoch_.load(std::memory_order_seq_cst) <= start && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}  // namespace gil
