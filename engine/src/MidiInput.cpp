#include "MidiInput.h"

#include <algorithm>
#include <stdexcept>

#include "backends/MidiDriver.h"

namespace sub {

namespace {

// The system's inputs by the names users see: a second device of the same
// name gets " #2", a third " #3"...
std::vector<std::string> numbered(std::vector<std::string> system) {
    std::vector<std::string> names;
    names.reserve(system.size());
    for (std::string& name : system) {
        const auto same = std::count_if(names.begin(), names.end(), [&](const std::string& n) {
            return n == name || n.rfind(name + " #", 0) == 0;
        });
        if (same > 0) name += " #" + std::to_string(same + 1);
        names.push_back(std::move(name));
    }
    return names;
}

}  // namespace

struct MidiInputDevices::Device {
    std::string name;
    std::unique_ptr<midi::InputConnection> connection;
};

MidiInputDevices::MidiInputDevices(Handler handler) : handler_(std::move(handler)) {}

MidiInputDevices::~MidiInputDevices() { closeAll(); }

std::vector<std::string> MidiInputDevices::available() { return numbered(midi::systemInputNames()); }

void MidiInputDevices::open(const std::string& name, uint16_t port) {
    if (std::any_of(open_.begin(), open_.end(), [&](const auto& d) { return d->name == name; })) return;
    const auto names = available();
    const auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end()) throw std::runtime_error("The MIDI input \"" + name + "\" is not connected");
    auto device = std::make_unique<Device>();
    device->name = name;
    device->connection = midi::openSystemInput(static_cast<size_t>(found - names.begin()), name, port, handler_);
    open_.push_back(std::move(device));
}

void MidiInputDevices::close(const std::string& name) {
    const auto it = std::find_if(open_.begin(), open_.end(), [&](const auto& d) { return d->name == name; });
    if (it != open_.end()) open_.erase(it);  // (its connection closes: the driver no longer calls back)
}

void MidiInputDevices::closeAll() {
    while (!open_.empty()) open_.pop_back();  // the last opened first
}

std::vector<std::string> MidiInputDevices::openNames() const {
    std::vector<std::string> names;
    for (const auto& device : open_) names.push_back(device->name);
    return names;
}

}  // namespace sub
