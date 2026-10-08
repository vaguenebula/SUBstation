#pragma once

// The song's chords along the top of the note grid (PianoRoll::chords()): a
// see-through band of colour for each chord over the part the clip plays,
// coloured by its root around the circle of fifths (related chords in related
// colours), darker for minor and diminished chords, named at its start. Shown
// while the harmony is (C). It takes no clicks: they go to the notes under it.

#include "pianoroll/RollItem.h"

#include <QColor>
#include <QtQml/qqmlregistration.h>

namespace sub::ui {

class ChordLane : public RollItem {
    Q_OBJECT
    QML_ELEMENT

public:
    static constexpr int kHeight = 18;

    explicit ChordLane(QQuickItem* parent = nullptr);

    // A chord's colour, opaque.
    static QColor chordColor(int root, bool minor);

protected:
    void paint(SgPainter& painter) override;
    void rollConnected(PianoRoll* roll) override;
};

}  // namespace sub::ui
