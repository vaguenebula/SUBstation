#include "MidiInput.h"

#include <chrono>

namespace sub {

int64_t hostTimeNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace sub
