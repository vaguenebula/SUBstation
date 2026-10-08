#pragma once

// The piano roll's keyboard: black keys 60 % wide, the key sounding lit, C's
// labelled. Pressing a key selects every note on that pitch (Shift adds) and
// plays it; dragging over the keys selects every note on the keys from the one
// pressed to the one under the mouse, and plays each key in turn; letting go
// stops it. Its
// wheel is the note grid's (Alt+wheel over the keys makes the rows taller or
// shorter).

#include "pianoroll/NoteSet.h"
#include "pianoroll/RollItem.h"

#include <QtQml/qqmlregistration.h>

#include <vector>

namespace sub::ui {

class PianoKeys : public RollItem {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit PianoKeys(QQuickItem* parent = nullptr);

protected:
    void paint(SgPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void wheelEvent(QWheelEvent* event) override;
    void rollConnected(PianoRoll* roll) override;

private:
    void selectKeys(int pitch);

    bool pressed_ = false;
    int pressPitch_ = 0;
    std::vector<ClipNote> base_;  // selected before the press (with Shift)
};

}  // namespace sub::ui
