#pragma once
// A take a recording made: the engine bridge reports them when a recording ends
// (EngineBridge::takesRecorded), and the editor makes them clips in one undo
// step (ProjectEditor::addRecordings).

#include <QMetaType>
#include <QString>
#include <QtGlobal>

#include <utility>
#include <vector>

namespace sub::app {

// A note of a MIDI take, in seconds on the timeline, within the take, and how
// it was bent as it played (MIDI 2.0's per-note pitch bend): (seconds from its
// start, semitones), in time order.
struct RecordedTakeNote {
    double start = 0.0;
    double end = 0.0;
    int pitch = 60;
    int velocity = 100;
    std::vector<std::pair<double, double>> bend;

    bool operator==(const RecordedTakeNote&) const = default;
};

// A recorded WAV file and where it starts: `startSec` from the timeline's start
// (negative: it began before it). A MIDI take has no file but `notes`.
struct RecordedTake {
    QString trackId;
    QString path;
    double startSec = 0.0;
    double durationSec = 0.0;
    std::vector<RecordedTakeNote> notes;
    bool midi = false;

    bool operator==(const RecordedTake&) const = default;
};

}  // namespace sub::app

// The bridge's takesRecorded signal carries them across queued connections.
Q_DECLARE_METATYPE(std::vector<sub::app::RecordedTake>)
