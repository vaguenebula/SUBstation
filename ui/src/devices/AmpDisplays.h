#pragma once

// Reading the Amp's displays, as its editor's items do (AmpPanel, AmpDriveGraph).
// A read hands over everything that came since the last one, which for an
// editor just made or shown again is seconds of backlog: history, not the level
// now. So a tick counts only what came since the last tick (at most the 0.1 s a
// tick is held to) and at least the last kRecentSeconds. The displays' rate
// (audio per value) and floor are the device's, through the application layer.

#include "audio/AmpResponse.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace sub::ui::ampDisplays {

constexpr double kRecentSeconds = 0.05;

// How many of a read's last values count, `dt` seconds after the last tick.
inline size_t recentValues(double dt, double sampleRate) {
    return size_t(std::ceil(std::max(kRecentSeconds, dt) * sampleRate / sub::app::ampDisplaySamples()));
}

// What the displays read at silence (dB): the floor the editor's levels fall to.
inline double floorDb() { return sub::app::ampDisplayFloorDb(); }

// The loudest of a display's last `keep` values (dB, no lower than `floorDb`), or nothing.
inline bool loudest(const std::vector<float>& values, size_t keep, double floorDb, double& into) {
    if (values.empty())
        return false;
    double most = floorDb;
    for (size_t i = values.size() - std::min(keep, values.size()); i < values.size(); ++i) {
        if (std::isfinite(values[i]))
            most = std::max(most, double(values[i]));
    }
    into = most;
    return true;
}

// The display's latest finite value, or nothing.
inline bool latest(const std::vector<float>& values, double& into) {
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
        if (std::isfinite(*it)) {
            into = *it;
            return true;
        }
    }
    return false;
}

}  // namespace sub::ui::ampDisplays
