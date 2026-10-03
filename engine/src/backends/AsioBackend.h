#pragma once
// ASIO, Steinberg's low-latency driver model. Compiled only when the engine is
// built with the ASIO SDK (SUBSTATION_HAS_ASIO; see CMakeLists.txt).

#include <memory>

#include "../AudioDevice.h"

namespace sub {

std::unique_ptr<AudioBackend> createAsioBackend();

}  // namespace sub
