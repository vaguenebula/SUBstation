// MIDI input where the engine has no MIDI backend yet (platforms other than
// Windows): no input is ever listed, so none is opened (see MidiDriver.h).
// Messages still arrive through Engine::sendMidiInput() (the computer
// keyboard, tests).

#include <stdexcept>

#include "backends/MidiDriver.h"

namespace sub::midi {

std::vector<std::string> systemInputNames() { return {}; }

std::unique_ptr<InputConnection> openSystemInput(size_t, const std::string& name, uint16_t,
                                                 const MidiInputDevices::Handler&) {
    throw std::runtime_error("The MIDI input \"" + name + "\" is not connected");
}

}  // namespace sub::midi
