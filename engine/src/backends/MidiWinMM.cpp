// MIDI input devices through WinMM (Windows).

#include "../MidiInput.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <stdexcept>

namespace sub {

namespace {

std::string narrow(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

// The input devices present, by name, numbered where names repeat; index = WinMM device id.
std::vector<std::string> deviceNames() {
    std::vector<std::string> names;
    const UINT count = midiInGetNumDevs();
    for (UINT id = 0; id < count; ++id) {
        MIDIINCAPSW caps{};
        std::string name = midiInGetDevCapsW(id, &caps, sizeof(caps)) == MMSYSERR_NOERROR ? narrow(caps.szPname)
                                                                                        : std::string("MIDI Input");
        const auto same = std::count_if(names.begin(), names.end(), [&](const std::string& n) {
            return n == name || n.rfind(name + " #", 0) == 0;
        });
        if (same > 0) name += " #" + std::to_string(same + 1);
        names.push_back(std::move(name));
    }
    return names;
}

std::string errorText(MMRESULT result) {
    wchar_t text[MAXERRORLENGTH] = {};
    if (midiInGetErrorTextW(result, text, MAXERRORLENGTH) == MMSYSERR_NOERROR) return narrow(text);
    return "error " + std::to_string(result);
}

// Bytes in a short message with this status byte.
int messageSize(uint8_t status) {
    switch (status & 0xF0) {
        case 0xC0:
        case 0xD0: return 2;
        case 0xF0:
            switch (status) {
                case 0xF1:
                case 0xF3: return 2;
                case 0xF2: return 3;
                default: return 1;
            }
        default: return 3;
    }
}

}  // namespace

struct MidiInputDevices::Device {
    std::string name;
    uint16_t port = 0;
    HMIDIIN handle = nullptr;
    const Handler* handler = nullptr;

    static void CALLBACK callback(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
        if (message != MIM_DATA) return;  // MIM_LONGDATA (no buffers are given), MIM_ERROR, open/close
        const int64_t now = hostTimeNs();
        const auto* device = reinterpret_cast<const Device*>(instance);
        const auto packed = static_cast<uint32_t>(param1);
        const uint8_t bytes[3] = {static_cast<uint8_t>(packed & 0xFF), static_cast<uint8_t>((packed >> 8) & 0xFF),
                                  static_cast<uint8_t>((packed >> 16) & 0xFF)};
        if (!(bytes[0] & 0x80)) return;
        (*device->handler)(device->port, bytes, messageSize(bytes[0]), now);
    }
};

MidiInputDevices::MidiInputDevices(Handler handler) : handler_(std::move(handler)) {}

MidiInputDevices::~MidiInputDevices() { closeAll(); }

std::vector<std::string> MidiInputDevices::available() { return deviceNames(); }

void MidiInputDevices::open(const std::string& name, uint16_t port) {
    if (std::any_of(open_.begin(), open_.end(), [&](const auto& d) { return d->name == name; })) return;
    const auto names = deviceNames();
    const auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end()) throw std::runtime_error("The MIDI input \"" + name + "\" is not connected");
    auto device = std::make_unique<Device>();
    device->name = name;
    device->port = port;
    device->handler = &handler_;
    const auto id = static_cast<UINT>(found - names.begin());
    MMRESULT result = midiInOpen(&device->handle, id, reinterpret_cast<DWORD_PTR>(&Device::callback),
                                 reinterpret_cast<DWORD_PTR>(device.get()), CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR) {
        throw std::runtime_error("The MIDI input \"" + name + "\" could not be opened: " + errorText(result) +
                                 (result == MMSYSERR_ALLOCATED ? " (another application may be using it)" : ""));
    }
    result = midiInStart(device->handle);
    if (result != MMSYSERR_NOERROR) {
        midiInClose(device->handle);
        throw std::runtime_error("The MIDI input \"" + name + "\" could not be started: " + errorText(result));
    }
    open_.push_back(std::move(device));
}

void MidiInputDevices::close(const std::string& name) {
    const auto it = std::find_if(open_.begin(), open_.end(), [&](const auto& d) { return d->name == name; });
    if (it == open_.end()) return;
    // Once midiInClose() returns the driver no longer calls back.
    midiInStop((*it)->handle);
    midiInReset((*it)->handle);
    midiInClose((*it)->handle);
    open_.erase(it);
}

void MidiInputDevices::closeAll() {
    while (!open_.empty()) close(open_.back()->name);
}

std::vector<std::string> MidiInputDevices::openNames() const {
    std::vector<std::string> names;
    for (const auto& device : open_) names.push_back(device->name);
    return names;
}

}  // namespace sub
