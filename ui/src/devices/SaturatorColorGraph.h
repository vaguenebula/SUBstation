#pragma once

// The Saturator's Color: the EQ applied before the curve (and inverted after
// it), on a 20 Hz..20 kHz log axis, ±36 dB, over the spectra of the device's
// input (filled) and output (a line), the displays "input" and "output" through
// the EQ's analyzer (analysis::EqAnalyzer). The curve is worked out by the
// engine's own design (sub::app::saturatorColorDb) and eases to new settings; it
// glows in the accent colour while Color is on and is grey while it is off. Two
// handles: Base (the low shelf, on its plateau at 60 Hz), up and down; the peak,
// across for its Frequency and up and down for its Depth. Dragging either
// switches Color on, all in one undo step; double-clicking one sets its gain to
// 0 dB. Everything moves in refreshDisplays(); when nothing moves it stops
// repainting.

#include "analysis/Spectrum.h"
#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include <QElapsedTimer>
#include <QtQml/qqmlregistration.h>

#include <vector>

namespace sub::ui {

class SaturatorColorGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT

public:
    static constexpr int kWidth = 220;
    static constexpr int kMinimumHeight = 52;
    static constexpr double kLow = 20.0;  // Hz across the graph
    static constexpr double kHigh = 20000.0;
    static constexpr double kRangeDb = 36.0;       // the EQ's scale, up and down
    static constexpr double kBaseHandleHz = 60.0;  // on the shelf's plateau
    static constexpr double kHandleHit = 8.0;      // px
    static constexpr double kEaseSeconds = 0.04;
    static constexpr double kFine = 0.2;  // Shift

    enum class Handle { None, Base, Peak };

    explicit SaturatorColorGraph(QQuickItem* parent = nullptr);

    // The frequencies the curve is worked out at (one per pixel across) and the curve there (dB).
    const std::vector<double>& columns() const { return columns_; }
    const std::vector<double>& curveDb() const { return curveDb_; }
    bool live() const;                         // a spectrum shows
    bool settled() const { return settled_; }  // nothing moves: no repainting
    QPointF baseHandle() const;
    QPointF peakHandle() const;

    QRectF plot() const;
    LogAxis frequencyAxis() const;
    double xOf(double hz) const;
    double yOfDb(double db) const;

protected:
    void sync() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void makeColumns();
    // The curve and the handles from the eased values (`exact`: from the parameters as they are).
    void updateCurve(bool exact);
    Handle handleAt(const QPointF& pos) const;
    void feed(sub::app::analysis::EqAnalyzer::Channel channel, const std::vector<float>& samples);

    // The parameters as they are set, and as drawn (eased; the frequency in log).
    double base_ = 0.0, freq_ = 1000.0, width_ = 50.0, depth_ = 0.0;
    bool on_ = false;
    Eased baseEased_, logFreqEased_, widthEased_, depthEased_, onEased_;
    bool synced_ = false;

    std::vector<double> columns_;
    std::vector<double> curveDb_;
    double baseHandleDb_ = 0.0, peakHandleDb_ = 0.0, peakHandleHz_ = 1000.0;

    sub::app::analysis::EqAnalyzer analyzer_;
    std::vector<double> inCols_, outCols_;
    bool inLive_ = false, outLive_ = false;
    int zeros_[2] = {0, 0};  // the latest values that were all 0, per channel (all of its window: skip them)
    QMetaObject::Connection bridgeConnection_;  // the bridge's deviceChanged: a new sample rate
    QElapsedTimer clock_;
    bool settled_ = true;

    QString gesture_;  // the drag's merge key ("": none)
    Handle handle_ = Handle::None;
    Handle hovered_ = Handle::None;
    // Where the mouse last was, and the values dragged so far (the frequency as a position across,
    // unrounded): each move adds its own distance, so a press a little off the handle doesn't jump
    // it, and Shift can come and go mid-drag.
    QPointF lastAt_;
    double dragBase_ = 0.0, dragFreqX_ = 0.0, dragDepth_ = 0.0;
};

}  // namespace sub::ui
