// MIDI input devices where the engine has no MIDI backend (platforms other than
// Windows): none are ever connected. Messages still arrive through
// Engine::sendMidiInput() (the computer keyboard, tests).

#include <algorithm>
#include <stdexcept>

#include "../MidiInput.h"

namespace sub {

struct MidiInputDevices::Device {
    std::string name;
};

MidiInputDevices::MidiInputDevices(Handler handler) : handler_(std::move(handler)) {}

MidiInputDevices::~MidiInputDevices() { closeAll(); }

std::vector<std::string> MidiInputDevices::available() { return {}; }

void MidiInputDevices::open(const std::string& name, uint16_t) {
    throw std::runtime_error("The MIDI input \"" + name + "\" is not connected");
}

void MidiInputDevices::close(const std::string& name) {
    open_.erase(std::remove_if(open_.begin(), open_.end(), [&](const auto& d) { return d->name == name; }),
                open_.end());
}

void MidiInputDevices::closeAll() { open_.clear(); }

std::vector<std::string> MidiInputDevices::openNames() const {
    std::vector<std::string> names;
    for (const auto& device : open_) names.push_back(device->name);
    return names;
}

}  // namespace sub
