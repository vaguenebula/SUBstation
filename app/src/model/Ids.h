#pragma once
// Ids of tracks, clips, devices and rack chains: unique in the project.

#include <QString>

namespace sub::app {

// 12 hex digits of a random UUID.
QString newId();

}  // namespace sub::app
