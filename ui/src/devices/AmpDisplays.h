#pragma once

// Reading the Amp's displays, as its editor's items do (AmpPanel, AmpDriveGraph).
// A read hands over everything that came since the last one, which for an
// editor just made or shown again is seconds of backlog: history, not the level
// now. So a tick reads them with DeviceCanvas::readRecent(id, recentSeconds(dt)):
// what came since the last tick (at most the 0.1 s a tick is held to), at least
// the last kRecentSeconds of it. The displays' floor is the device's, through
// the application layer (their rate readRecent takes from the device itself).

#include "audio/AmpResponse.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sub::ui::ampDisplays {

constexpr double kRecentSeconds = 0.05;

// The audio a tick's read covers, `dt` seconds after the last tick.
inline double recentSeconds(double dt) { return std::max(kRecentSeconds, dt); }

// What the displays read at silence (dB): the floor the editor's levels fall to.
inline double floorDb() { return sub::app::ampDisplayFloorDb(); }

// The loudest of a display's values (dB, no lower than `floorDb`), or nothing.
inline bool loudest(const std::vector<float>& values, double floorDb, double& into) {
    if (values.empty())
        return false;
    double most = floorDb;
    for (const float v : values) {
        if (std::isfinite(v))
            most = std::max(most, double(v));
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
