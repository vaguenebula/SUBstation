#pragma once

// Stereo peak meter with a falling display (meter.py): -60 to +6 dB, falling
// kFallPerUpdate of the scale per update (updates come at about 30 Hz), with a
// clip light at full scale that a click clears. Levels (linear peaks) are
// pushed with setLevels() once per meter update, e.g. from the app layer's
// meters signal; the fall needs every update, even unchanged ones.

#include "sg/SgCanvas.h"

#include <QtQml/qqmlregistration.h>

namespace sub::ui {

class Meter : public SgCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(qreal leftLevel READ leftLevel NOTIFY levelsChanged)    // what shows, 0 to 1 of the scale
    Q_PROPERTY(qreal rightLevel READ rightLevel NOTIFY levelsChanged)
    Q_PROPERTY(bool clipped READ clipped NOTIFY clippedChanged)

public:
    static constexpr double kFloorDb = -60.0;
    static constexpr double kCeilingDb = 6.0;
    static constexpr double kFallPerUpdate = 0.035;  // fraction of the scale per update (~30 Hz)

    explicit Meter(QQuickItem* parent = nullptr);

    // A linear peak level as a fraction of the scale, 0 to 1.
    static double fraction(double level);

    qreal leftLevel() const { return display_[0]; }
    qreal rightLevel() const { return display_[1]; }
    bool clipped() const { return clipped_; }

    // One meter update: the peaks since the last (linear; 1.0 is full scale).
    Q_INVOKABLE void setLevels(qreal left, qreal right);
    Q_INVOKABLE void reset();
    Q_INVOKABLE void clearClip();

signals:
    void levelsChanged();
    void clippedChanged();

protected:
    void paint(SgPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    double display_[2] = {0.0, 0.0};
    bool clipped_ = false;
};

}  // namespace sub::ui
