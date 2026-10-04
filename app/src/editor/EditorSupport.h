#pragma once
// Helpers the editor's source files share (not for other code).

#include "model/Device.h"
#include "model/Track.h"

#include <QString>
#include <QUndoStack>

#include <algorithm>
#include <optional>
#include <vector>

namespace sub::app::editing {

// An undo macro, open while it lives: its steps are one undo step, closed even
// when an edit throws.
class Macro {
public:
    Macro(QUndoStack* stack, const QString& text) : stack_(stack) { stack_->beginMacro(text); }
    ~Macro() { stack_->endMacro(); }
    Macro(const Macro&) = delete;
    Macro& operator=(const Macro&) = delete;

private:
    QUndoStack* stack_;
};

// An id, or none for "" (no chain: a track's own; no group).
inline std::optional<QString> optionalId(const QString& id) {
    return id.isEmpty() ? std::nullopt : std::optional<QString>(id);
}

// An index into a list of `size` (< 0: past its end), held to it.
inline int clampIndex(int index, int size) { return index < 0 ? size : std::min(index, size); }

// A track with just what the group tree and the routing graph look at.
inline Track skeleton(const Track& track) {
    Track copy;
    copy.id = track.id;
    copy.name = track.name;
    copy.kind = track.kind;
    copy.parent = track.parent;
    copy.inputTrack = track.inputTrack;
    copy.sends = track.sends;
    return copy;
}

// The index of the device of this id in a list (-1: not there).
inline int indexOfDevice(const std::vector<Device>& devices, const QString& deviceId) {
    for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
        if (devices[i].id == deviceId) return i;
    }
    return -1;
}

}  // namespace sub::app::editing
