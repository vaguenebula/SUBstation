#pragma once

// The piano roll's playhead while the arrangement plays inside the clip: a
// line over the notes and the velocities, a triangle at the ruler's foot
// (`ruler`). An item of its own above each of them, so following playback
// redraws only this, not the notes (the old roll repainted strips of each).

#include "pianoroll/RollItem.h"

#include <QtQml/qqmlregistration.h>

namespace sub::ui {

class RollPlayhead : public RollItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ruler READ ruler WRITE setRuler NOTIFY rulerChanged)

public:
    explicit RollPlayhead(QQuickItem* parent = nullptr);

    bool ruler() const { return ruler_; }
    void setRuler(bool ruler);

Q_SIGNALS:
    void rulerChanged();

protected:
    void paint(SgPainter& painter) override;
    void rollConnected(PianoRoll* roll) override;

private:
    bool ruler_ = false;
};

}  // namespace sub::ui
