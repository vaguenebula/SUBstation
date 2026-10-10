#pragma once
// What a system's MIDI input API gives MidiInputDevices (MidiInput.h): the
// inputs present, and a connection to one. One file per system:
// MidiWinMM.cpp (Windows); MidiNone.cpp where the engine has no MIDI backend
// yet (none are ever listed). MidiInputDevices does the rest the same way for
// every system: the names users see, which inputs are open, closing them.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../MidiInput.h"

namespace sub::midi {

// An open input: its messages go to the handler until it is destroyed. Once
// the destructor returns, the driver no longer calls back.
class InputConnection {
public:
    virtual ~InputConnection() = default;
};

// The names of the system's inputs present now, in its order (repeated names
// as they are: MidiInputDevices numbers them).
std::vector<std::string> systemInputNames();

// Opens the system's input at `index` in systemInputNames(); `name` is how the
// user knows it, for messages. The driver's thread calls `handler` (which
// outlives the connection) with `port` for every short message (channel and
// system messages; System Exclusive is ignored), stamped with hostTimeNs()
// first thing. Throws std::runtime_error with a message for the user (another
// application may have the input).
std::unique_ptr<InputConnection> openSystemInput(size_t index, const std::string& name, uint16_t port,
                                                 const MidiInputDevices::Handler& handler);

// Bytes in a short message with this status byte, for APIs that hand over bytes.
inline int shortMessageSize(uint8_t status) {
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

}  // namespace sub::midi
