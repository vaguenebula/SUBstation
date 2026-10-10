#pragma once

// The Gate's sidechain EQ: the key filter's response on a 20 Hz..20 kHz log
// axis, ± kRangeDb, worked out at the engine's sample rate from the engine's own
// filter (sub::app::gateKeyFilterDb, the application layer's wrapper of
// sub::gate::keyFilter), so the curve is what keys the gate. Its dot is where
// the filter is tuned: drag across for the frequency, up and down for the gain
// (the shelves and the bell) or the Q (the pass filters: doubling every
// kQPixels), Shift finely; one undo step per drag; double-click puts the three
// back to their defaults. With the EQ off the curve is grey and unfilled,
// easing to the accent as it comes on.

#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include <QElapsedTimer>
#include <QtQml/qqmlregistration.h>

#include <vector>

namespace sub::ui {

class GateKeyGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool active READ active NOTIFY curveChanged)  // the EQ is on

public:
    static constexpr double kLow = 20.0;  // Hz across the graph
    static constexpr double kHigh = 20000.0;
    static constexpr double kRangeDb = 18.0;  // above and below 0 dB
    static constexpr double kQPixels = 60.0;  // dragged up this far, the Q doubles
    static constexpr int kMinimumHeight = 36;
    static constexpr double kFreqMin = 30.0, kFreqMax = 15000.0;  // the parameters' ranges (the engine's)
    static constexpr double kQMin = 0.1, kQMax = 12.0;
    static constexpr double kGainMin = -15.0, kGainMax = 15.0;

    explicit GateKeyGraph(QQuickItem* parent = nullptr);

    bool active() const { return active_; }
    QRectF plot() const;
    LogAxis frequencyAxis() const;
    double xOf(double hz) const;
    double freqAt(double x) const;
    double yOf(double db) const;  // held to the plot
    double dbAt(double y) const;
    // The curve as drawn: a frequency per column, and the response there.
    const std::vector<double>& frequencies() const { return frequencies_; }
    const std::vector<double>& responseDb() const { return response_; }
    // Where its dot is: at the frequency, at the gain (shelves, bell) or the response there.
    QPointF dot() const;

Q_SIGNALS:
    void curveChanged();

protected:
    void sync() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;

private:
    void updateCurve();
    bool onDot(const QPointF& pos) const;
    void endDrag();

    bool active_ = false;
    int type_ = 5;  // high-pass
    double freq_ = 80.0;
    double q_ = 0.71;
    double gain_ = 0.0;
    bool usesGain_ = false;
    std::vector<double> frequencies_;
    std::vector<double> response_;
    QMetaObject::Connection bridgeConnection_;  // the bridge's deviceChanged: a new sample rate

    QElapsedTimer clock_;
    Eased activeEase_;  // 0: off (grey) .. 1: on (the accent, filled)
    Eased hoverEase_;   // the ring round the dot
    bool hovered_ = false;

    QString gesture_;  // the drag's merge key ("": none)
    QPointF lastPos_;
    double dragFreqX_ = 0.0;  // the dragged values as the drag has them (unclamped)
    double dragQ_ = 1.0;
    double dragGain_ = 0.0;
    std::vector<QPointF> points_;  // paint()'s scratch
};

}  // namespace sub::ui
