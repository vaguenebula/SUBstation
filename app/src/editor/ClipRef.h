#pragma once
// A clip by where it is: its track's id and its own.

#include <QHashFunctions>
#include <QList>
#include <QString>

#include <tuple>

namespace sub::app {

struct ClipRef {
    QString trackId;
    QString clipId;

    friend bool operator==(const ClipRef&, const ClipRef&) = default;
    friend bool operator<(const ClipRef& a, const ClipRef& b) {
        return std::tie(a.trackId, a.clipId) < std::tie(b.trackId, b.clipId);
    }
};

// Clips in an order (the editor's results: in the order they were made).
using ClipRefs = QList<ClipRef>;

inline size_t qHash(const ClipRef& ref, size_t seed = 0) noexcept { return qHashMulti(seed, ref.trackId, ref.clipId); }

}  // namespace sub::app
