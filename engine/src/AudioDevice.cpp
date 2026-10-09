#include "AudioDevice.h"

#include <stdexcept>

#include "backends/AudioBackends.h"

namespace sub {

AudioDevice::AudioDevice() : backends_(makeAudioBackends()) {}

AudioDevice::~AudioDevice() { close(); }

std::vector<std::string> AudioDevice::driverTypes() { return audioDriverNames(); }

AudioBackend& AudioDevice::backend(const std::string& driver) {
    for (auto& backend : backends_) {
        if (backend->name() == driver) return *backend;
    }
    if (driver == "ASIO") throw std::runtime_error("This build of SUBstation has no ASIO support (the ASIO SDK was missing)");
    throw std::runtime_error("Unknown driver type: " + driver);
}

std::vector<AudioDeviceInfo> AudioDevice::devices(const std::string& driver) { return backend(driver).devices(); }

void AudioDevice::open(const DeviceConfig& config, AudioCallback* callback) {
    AudioBackend& next = backend(config.driver);
    close();
    next.open(config, callback);
    current_ = &next;
    config_ = config;
    hasConfig_ = true;
}

void AudioDevice::start() {
    if (current_) current_->start();
}

void AudioDevice::close() {
    if (!current_) return;
    current_->close();
    current_ = nullptr;
}

bool AudioDevice::isOpen() const { return current_ && current_->isOpen(); }

DeviceState AudioDevice::state() const { return isOpen() ? current_->state() : DeviceState{}; }

DeviceConfig AudioDevice::resetConfig() {
    DeviceConfig config = config_;
    if (isOpen()) current_->adjustForReset(config);
    return config;
}

bool AudioDevice::showControlPanel() { return isOpen() && current_->showControlPanel(); }

bool AudioDevice::inControlPanel() const { return current_ && current_->inControlPanel(); }

void AudioDevice::refreshLatencies() {
    if (isOpen()) current_->refreshLatencies();
}

}  // namespace sub
