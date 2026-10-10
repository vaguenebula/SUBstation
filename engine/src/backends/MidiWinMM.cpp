// MIDI input through WinMM (Windows): the system's side of MidiInputDevices
// (see MidiDriver.h).

#include "backends/MidiDriver.h"

#include <windows.h>
#include <mmsystem.h>

#include <stdexcept>

#include "platform/Unicode.h"

namespace sub::midi {

namespace {

std::string errorText(MMRESULT result) {
    wchar_t text[MAXERRORLENGTH] = {};
    if (midiInGetErrorTextW(result, text, MAXERRORLENGTH) == MMSYSERR_NOERROR) return platform::fromWide(text);
    return "error " + std::to_string(result);
}

// One open input; its index in systemInputNames() is its WinMM device id.
class WinMMInput final : public InputConnection {
public:
    WinMMInput(UINT id, const std::string& name, uint16_t port, const MidiInputDevices::Handler& handler)
        : port_(port), handler_(handler) {
        MMRESULT result = midiInOpen(&handle_, id, reinterpret_cast<DWORD_PTR>(&callback),
                                     reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
        if (result != MMSYSERR_NOERROR) {
            throw std::runtime_error("The MIDI input \"" + name + "\" could not be opened: " + errorText(result) +
                                     (result == MMSYSERR_ALLOCATED ? " (another application may be using it)" : ""));
        }
        result = midiInStart(handle_);
        if (result != MMSYSERR_NOERROR) {
            midiInClose(handle_);
            throw std::runtime_error("The MIDI input \"" + name + "\" could not be started: " + errorText(result));
        }
    }

    // Once midiInClose() returns the driver no longer calls back.
    ~WinMMInput() override {
        midiInStop(handle_);
        midiInReset(handle_);
        midiInClose(handle_);
    }

    WinMMInput(const WinMMInput&) = delete;
    WinMMInput& operator=(const WinMMInput&) = delete;

private:
    static void CALLBACK callback(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
        if (message != MIM_DATA) return;  // MIM_LONGDATA (no buffers are given), MIM_ERROR, open/close
        const int64_t now = hostTimeNs();
        const auto* input = reinterpret_cast<const WinMMInput*>(instance);
        const auto packed = static_cast<uint32_t>(param1);
        const uint8_t bytes[3] = {static_cast<uint8_t>(packed & 0xFF), static_cast<uint8_t>((packed >> 8) & 0xFF),
                                  static_cast<uint8_t>((packed >> 16) & 0xFF)};
        if (!(bytes[0] & 0x80)) return;
        input->handler_(input->port_, bytes, shortMessageSize(bytes[0]), now);
    }

    HMIDIIN handle_ = nullptr;
    const uint16_t port_;
    const MidiInputDevices::Handler& handler_;
};

}  // namespace

std::vector<std::string> systemInputNames() {
    std::vector<std::string> names;
    const UINT count = midiInGetNumDevs();
    for (UINT id = 0; id < count; ++id) {
        MIDIINCAPSW caps{};
        names.push_back(midiInGetDevCapsW(id, &caps, sizeof(caps)) == MMSYSERR_NOERROR ? platform::fromWide(caps.szPname)
                                                                                      : std::string("MIDI Input"));
    }
    return names;
}

std::unique_ptr<InputConnection> openSystemInput(size_t index, const std::string& name, uint16_t port,
                                                 const MidiInputDevices::Handler& handler) {
    return std::make_unique<WinMMInput>(static_cast<UINT>(index), name, port, handler);
}

}  // namespace sub::midi
