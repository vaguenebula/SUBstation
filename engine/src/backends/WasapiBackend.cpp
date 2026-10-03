#include "WasapiBackend.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "miniaudio.h"

namespace sub {

struct WasapiCallbacks {
    static void data(ma_device* device, void* output, const void*, ma_uint32 frames) {
        auto* self = static_cast<WasapiBackend*>(device->pUserData);
        self->process(static_cast<float*>(output), frames, device->playback.channels);
    }

    static void notification(const ma_device_notification* notification) {
        auto* self = static_cast<WasapiBackend*>(notification->pDevice->pUserData);
        if (self->closing_.load()) return;
        switch (notification->type) {
            case ma_device_notification_type_stopped: self->callback_->deviceEvent(DeviceEvent::Stopped); break;
            case ma_device_notification_type_rerouted: self->callback_->deviceEvent(DeviceEvent::Rerouted); break;
            default: break;
        }
    }
};

WasapiBackend::WasapiBackend() : context_(std::make_unique<ma_context>()) {
    const ma_backend backends[] = {ma_backend_wasapi};
    contextReady_ = ma_context_init(backends, 1, nullptr, context_.get()) == MA_SUCCESS;
    if (!contextReady_) contextReady_ = ma_context_init(nullptr, 0, nullptr, context_.get()) == MA_SUCCESS;
    scratch_.assign(static_cast<size_t>(kChunk) * outputs_.size(), 0.f);
    for (size_t c = 0; c < outputs_.size(); ++c) outputs_[c] = scratch_.data() + c * kChunk;
}

WasapiBackend::~WasapiBackend() {
    close();
    if (contextReady_) ma_context_uninit(context_.get());
}

std::vector<AudioDeviceInfo> WasapiBackend::devices() {
    std::vector<AudioDeviceInfo> result;
    if (!contextReady_) return result;
    ma_device_info* playback = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(context_.get(), &playback, &count, nullptr, nullptr) != MA_SUCCESS) return result;
    for (ma_uint32 i = 0; i < count; ++i) result.push_back({playback[i].name, playback[i].isDefault != 0});
    return result;
}

void WasapiBackend::open(const DeviceConfig& config, AudioCallback* callback) {
    close();
    if (!contextReady_) throw std::runtime_error("No audio backend is available");

    ma_device_id id{};
    bool haveId = false;
    if (!config.name.empty()) {
        ma_device_info* playback = nullptr;
        ma_uint32 count = 0;
        if (ma_context_get_devices(context_.get(), &playback, &count, nullptr, nullptr) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < count; ++i) {
                if (config.name == playback[i].name) {
                    id = playback[i].id;
                    haveId = true;
                    break;
                }
            }
        }
        if (!haveId) throw std::runtime_error("Audio device not found: " + config.name);
    }

    // miniaudio mixes the stereo output into the device's own channel layout.
    ma_device_config maConfig = ma_device_config_init(ma_device_type_playback);
    maConfig.playback.pDeviceID = haveId ? &id : nullptr;
    maConfig.playback.format = ma_format_f32;
    maConfig.playback.channels = 2;
    maConfig.playback.shareMode = config.exclusive ? ma_share_mode_exclusive : ma_share_mode_shared;
    maConfig.sampleRate = config.sampleRate;
    maConfig.periodSizeInFrames = config.bufferFrames;
    maConfig.performanceProfile = ma_performance_profile_low_latency;
    maConfig.noPreSilencedOutputBuffer = MA_TRUE;  // the engine writes every frame
    maConfig.noFixedSizedCallback = MA_TRUE;       // the renderer handles any block size
    maConfig.wasapi.usage = ma_wasapi_usage_pro_audio;
    maConfig.dataCallback = &WasapiCallbacks::data;
    maConfig.notificationCallback = &WasapiCallbacks::notification;
    maConfig.pUserData = this;

    callback_ = callback;
    closing_.store(false);
    sampleTime_ = 0;
    auto device = std::make_unique<ma_device>();
    const ma_result result = ma_device_init(context_.get(), &maConfig, device.get());
    if (result != MA_SUCCESS) {
        throw std::runtime_error(std::string("Could not open audio device: ") + ma_result_description(result));
    }
    device_ = std::move(device);
    exclusive_ = config.exclusive;

    char deviceName[MA_MAX_DEVICE_NAME_LENGTH + 1] = {};
    ma_device_get_name(device_.get(), ma_device_type_playback, deviceName, sizeof(deviceName), nullptr);
    name_ = deviceName;
}

void WasapiBackend::start() {
    if (!device_) return;
    const ma_result result = ma_device_start(device_.get());
    if (result != MA_SUCCESS) {
        close();
        throw std::runtime_error(std::string("Could not start audio device: ") + ma_result_description(result));
    }
}

void WasapiBackend::close() {
    if (!device_) return;
    closing_.store(true);
    ma_device_uninit(device_.get());  // blocks until the audio thread has exited
    device_.reset();
    name_.clear();
}

void WasapiBackend::process(float* interleaved, uint32_t frames, uint32_t channels) noexcept {
    const int64_t hostTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch()).count();
    const uint32_t used = std::min<uint32_t>(channels, static_cast<uint32_t>(outputs_.size()));
    for (uint32_t done = 0; done < frames;) {
        const uint32_t n = std::min(kChunk, frames - done);
        AudioIO io;
        io.outputs = outputs_.data();
        io.numOutputs = used;
        io.frames = n;
        io.sampleTime = sampleTime_;
        io.hostTimeNs = hostTime;
        callback_->audioCallback(io);
        float* dst = interleaved + static_cast<size_t>(done) * channels;
        for (uint32_t i = 0; i < n; ++i) {
            float* frame = dst + static_cast<size_t>(i) * channels;
            for (uint32_t c = 0; c < used; ++c) frame[c] = outputs_[c][i];
            for (uint32_t c = used; c < channels; ++c) frame[c] = 0.f;
        }
        sampleTime_ += n;
        done += n;
    }
}

DeviceState WasapiBackend::state() const {
    DeviceState state;
    if (!device_) return state;
    state.driver = name();
    state.name = name_;
    state.sampleRate = device_->sampleRate;
    state.bufferFrames = device_->playback.internalPeriodSizeInFrames;
    if (device_->playback.internalSampleRate > 0) {
        // WASAPI buffers `internalPeriods` periods at the device's own rate.
        const double seconds = static_cast<double>(device_->playback.internalPeriodSizeInFrames) *
                               device_->playback.internalPeriods / device_->playback.internalSampleRate;
        state.outputLatency = static_cast<uint32_t>(seconds * device_->sampleRate + 0.5);
    }
    for (uint32_t c = 0; c < device_->playback.channels; ++c) state.outputChannels.push_back(static_cast<int>(c));
    state.capabilities.outputNames = {"Left", "Right"};
    state.exclusive = exclusive_;
    return state;
}

}  // namespace sub
