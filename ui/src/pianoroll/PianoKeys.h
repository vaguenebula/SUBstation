#pragma once

// The piano roll's keyboard (piano_roll.py's PianoKeys): black keys 60 % wide,
// the key sounding lit, C's labelled. Pressing a key selects every note on that
// pitch (Shift adds) and plays it; dragging over the keys plays each in turn;
// letting go stops it. Its wheel is the note grid's (Alt+wheel over the keys
// makes the rows taller or shorter).

#include "pianoroll/RollItem.h"

#include <QtQml/qqmlregistration.h>

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
    bool pressed_ = false;
};

}  // namespace sub::ui
