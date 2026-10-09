#pragma once

// The Disperser's graph: how late each frequency comes out, the stages' group
// delay in milliseconds, on a 20 Hz..20 kHz log axis. It is worked out from the
// engine's own stages at the engine's sample rate (sub::app::disperserGroupDelayMs),
// so the curve is the delay that plays; the delay axis fits it (a 1, 2, 5 step
// at or above its peak). Its dot sits on the curve where the stages are tuned:
// drag across for the Frequency, up and down for the Pinch (doubling every
// kPinchPixels), one undo step per drag. Bypassed, the curve (what would play)
// is greyed; without stages it lies flat at 0.

#include "devices/DeviceCanvas.h"

#include <QtQml/qqmlregistration.h>

#include <vector>

namespace sub::ui {

class DispersionGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double peakMs READ peakMs NOTIFY curveChanged)    // the curve's highest
    Q_PROPERTY(double rangeMs READ rangeMs NOTIFY curveChanged)  // the delay axis' top

public:
    static constexpr int kWidth = 260;
    static constexpr int kMinimumHeight = 80;
    static constexpr double kLow = 20.0;  // Hz across the graph
    static constexpr double kHigh = 20000.0;
    static constexpr double kPinchPixels = 60.0;  // dragged up this far, the pinch doubles

    explicit DispersionGraph(QQuickItem* parent = nullptr);

    // The delay axis' top for a curve that peaks at `ms`: 1, 2 or 5 times a power of ten, at
    // least that (and at least 0.1 ms).
    static double niceRange(double ms);

    // The curve as drawn: frequencies (Hz, rising: one per column, and its peak) and its delay
    // there (ms).
    const std::vector<double>& frequencies() const { return frequencies_; }
    const std::vector<double>& delays() const { return delays_; }
    double peakMs() const { return peak_; }
    double rangeMs() const { return range_; }
    // Where its dot is: the frequency the stages are tuned to, and its delay there.
    QPointF dot() const;

    QRectF plot() const;
    double xOf(double freq) const;
    double freqAt(double x) const;
    double yOf(double ms) const;

Q_SIGNALS:
    void curveChanged();

protected:
    void sync() override;
    void paint(SgPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void updateCurve();
    void dragTo(const QPointF& pos);

    int stages_ = 0;
    double freq_ = 1000.0;   // the parameter
    double tuned_ = 1000.0;  // where the stages are tuned (below Nyquist)
    double pinch_ = 1.0;
    bool bypassed_ = false;
    std::vector<double> frequencies_;
    std::vector<double> delays_;
    double tunedDelay_ = 0.0;  // ms, at tuned_
    double peak_ = 0.0;
    double range_ = 1.0;
    QString gesture_;  // the drag's merge key ("": none)
    QMetaObject::Connection bridgeConnection_;  // the bridge's deviceChanged: a new sample rate
    QPointF pressedAt_;
    double pressedPinch_ = 1.0;
};

}  // namespace sub::ui
