#include "audio/LiveTake.h"

#include "Engine.h"

#include <algorithm>

namespace sub::app {

static_assert(LiveTake::kPeakFrames == sub::Engine::kRecordPeakFrames, "live peaks are the engine's");

void LiveTake::addPeaks(const std::vector<float>& more) {
    if (peaks.size() + more.size() > peaks.capacity()) {
        peaks.reserve(std::max(2 * peaks.capacity(), peaks.size() + more.size()));
    }
    peaks.insert(peaks.end(), more.begin(), more.end());
}

}  // namespace sub::app
