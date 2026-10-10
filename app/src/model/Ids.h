#pragma once
// Ids of tracks, clips, devices and rack chains: unique in the project.

#include <QString>

#include <optional>

namespace sub::app {

// 12 hex digits of a random UUID.
QString newId();

// An id, or none for "" (no chain: a track's own; no group; no track).
inline std::optional<QString> optionalId(const QString& id) {
    return id.isEmpty() ? std::nullopt : std::optional<QString>(id);
}

}  // namespace sub::app
