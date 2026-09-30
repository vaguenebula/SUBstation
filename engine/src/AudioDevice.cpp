#include "AudioDevice.h"

#include <stdexcept>

#include "backends/WasapiBackend.h"
#if GILSTUDIO_HAS_ASIO
#include "backends/AsioBackend.h"
#endif

namespace gil {

AudioDevice::AudioDevice() {
#if GILSTUDIO_HAS_ASIO
    // First: ASIO needs this thread in a single-threaded COM apartment, which
    // miniaudio would otherwise make multithreaded.
    auto asio = createAsioBackend();
#endif
    backends_.push_back(std::make_unique<WasapiBackend>());
#if GILSTUDIO_HAS_ASIO
    backends_.push_back(std::move(asio));
#endif
}

AudioDevice::~AudioDevice() { close(); }

std::vector<std::string> AudioDevice::driverTypes() {
#if GILSTUDIO_HAS_ASIO
    return {"WASAPI", "ASIO"};
#else
    return {"WASAPI"};
#endif
}

AudioBackend& AudioDevice::backend(const std::string& driver) {
    for (auto& backend : backends_) {
        if (backend->name() == driver) return *backend;
    }
    if (driver == "ASIO") throw std::runtime_error("This build of GIL Studio has no ASIO support (the ASIO SDK was missing)");
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

}  // namespace gil
