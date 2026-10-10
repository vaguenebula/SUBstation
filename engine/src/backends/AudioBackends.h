#pragma once
// The audio driver types an engine built for this system has, for
// AudioDevice. One file per system: AudioBackendsWin32.cpp (WASAPI, and ASIO
// with the ASIO SDK), AudioBackendsPosix.cpp (miniaudio's "System"; PipeWire on
// Linux and Core Audio on macOS go there, before it).

#include <memory>
#include <string>
#include <vector>

#include "../AudioDevice.h"

namespace sub {

// The driver types' names, in the order the audio settings list them. The
// first is kDefaultDriver.
std::vector<std::string> audioDriverNames();

// A backend of each driver type, in that order (made in whatever order the
// system needs).
std::vector<std::unique_ptr<AudioBackend>> makeAudioBackends();

}  // namespace sub
