#include "AudioDevice.h"

#include <cstring>
#include <stdexcept>

#include "miniaudio.h"

namespace gil {

struct DeviceCallbacks {
    static void data(ma_device* device, void* output, const void*, ma_uint32 frames) {
        auto* self = static_cast<AudioDevice*>(device->pUserData);
        self->callback_->audioCallback(static_cast<float*>(output), frames, device->playback.channels);
    }

    static void notification(const ma_device_notification* notification) {
        auto* self = static_cast<AudioDevice*>(notification->pDevice->pUserData);
        if (self->closing_.load()) return;
        switch (notification->type) {
            case ma_device_notification_type_stopped: self->callback_->deviceEvent(DeviceEvent::Stopped); break;
            case ma_device_notification_type_rerouted: self->callback_->deviceEvent(DeviceEvent::Rerouted); break;
            default: break;
        }
    }
};

AudioDevice::AudioDevice() : context_(std::make_unique<ma_context>()) {
    const ma_backend backends[] = {ma_backend_wasapi};
    contextReady_ = ma_context_init(backends, 1, nullptr, context_.get()) == MA_SUCCESS;
    if (!contextReady_) contextReady_ = ma_context_init(nullptr, 0, nullptr, context_.get()) == MA_SUCCESS;
}

AudioDevice::~AudioDevice() {
    close();
    if (contextReady_) ma_context_uninit(context_.get());
}

std::vector<OutputDeviceInfo> AudioDevice::outputDevices() {
    std::vector<OutputDeviceInfo> result;
    if (!contextReady_) return result;
    ma_device_info* playback = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(context_.get(), &playback, &count, nullptr, nullptr) != MA_SUCCESS) return result;
    for (ma_uint32 i = 0; i < count; ++i) result.push_back({playback[i].name, playback[i].isDefault != 0});
    return result;
}

void AudioDevice::init(const std::string& name, uint32_t sampleRate, uint32_t bufferFrames, bool exclusive,
                       AudioCallback* callback) {
    close();
    if (!contextReady_) throw std::runtime_error("No audio backend is available");

    ma_device_id id{};
    bool haveId = false;
    if (!name.empty()) {
        ma_device_info* playback = nullptr;
        ma_uint32 count = 0;
        if (ma_context_get_devices(context_.get(), &playback, &count, nullptr, nullptr) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < count; ++i) {
                if (name == playback[i].name) {
                    id = playback[i].id;
                    haveId = true;
                    break;
                }
            }
        }
        if (!haveId) throw std::runtime_error("Audio device not found: " + name);
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.pDeviceID = haveId ? &id : nullptr;
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.playback.shareMode = exclusive ? ma_share_mode_exclusive : ma_share_mode_shared;
    config.sampleRate = sampleRate;
    config.periodSizeInFrames = bufferFrames;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.noPreSilencedOutputBuffer = MA_TRUE;  // the engine writes every frame
    config.noFixedSizedCallback = MA_TRUE;       // the renderer handles any block size
    config.wasapi.usage = ma_wasapi_usage_pro_audio;
    config.dataCallback = &DeviceCallbacks::data;
    config.notificationCallback = &DeviceCallbacks::notification;
    config.pUserData = this;

    callback_ = callback;
    closing_.store(false);
    auto device = std::make_unique<ma_device>();
    const ma_result result = ma_device_init(context_.get(), &config, device.get());
    if (result != MA_SUCCESS) {
        throw std::runtime_error(std::string("Could not open audio device: ") + ma_result_description(result));
    }
    device_ = std::move(device);
    exclusive_ = exclusive;

    char deviceName[MA_MAX_DEVICE_NAME_LENGTH + 1] = {};
    ma_device_get_name(device_.get(), ma_device_type_playback, deviceName, sizeof(deviceName), nullptr);
    name_ = deviceName;
}

void AudioDevice::start() {
    if (!device_) return;
    const ma_result result = ma_device_start(device_.get());
    if (result != MA_SUCCESS) {
        close();
        throw std::runtime_error(std::string("Could not start audio device: ") + ma_result_description(result));
    }
}

void AudioDevice::close() {
    if (!device_) return;
    closing_.store(true);
    ma_device_uninit(device_.get());  // blocks until the audio thread has exited
    device_.reset();
    name_.clear();
}

std::string AudioDevice::backendName() const {
    if (!contextReady_) return "none";
    return ma_get_backend_name(context_->backend);
}

uint32_t AudioDevice::sampleRate() const { return device_ ? device_->sampleRate : 0; }
uint32_t AudioDevice::channels() const { return device_ ? device_->playback.channels : 0; }
uint32_t AudioDevice::bufferFrames() const { return device_ ? device_->playback.internalPeriodSizeInFrames : 0; }

double AudioDevice::latencySeconds() const {
    if (!device_ || device_->playback.internalSampleRate == 0) return 0.0;
    return static_cast<double>(device_->playback.internalPeriodSizeInFrames) * device_->playback.internalPeriods /
           device_->playback.internalSampleRate;
}

}  // namespace gil
