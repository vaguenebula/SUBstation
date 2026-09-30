#pragma once
// ASIO, Steinberg's low-latency driver model. Compiled only when the engine is
// built with the ASIO SDK (GILSTUDIO_HAS_ASIO; see CMakeLists.txt).

#include <memory>

#include "../AudioDevice.h"

namespace gil {

std::unique_ptr<AudioBackend> createAsioBackend();

}  // namespace gil
