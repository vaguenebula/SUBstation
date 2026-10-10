#pragma once

// The Saturator's shaper curve: input level across (-1..1, ±0 dBFS before
// Drive), output up, worked out by the engine's own functions
// (sub::app::saturatorCurve: Drive, the curve and Post Clip), so the curve
// drawn is the shaping that plays. Over it the signal: two mirrored dots at the
// input's peak level (display "in_peak"), the stretch of the curve between them
// lit with a glow that turns from amber to red as the curve bends away from its
// straight line there (how hard it saturates), and an afterglow where the
// signal has just been. Red bars flash at the sides when the input passes full
// scale. Under the curve the In strip (the dots' level on the same x axis, the
// held peak a tick), at its right the Out strip (display "out_peak", the
// device's real output, on the y axis). A new setting morphs the curve from the
// old shape to the new.
// Drag up and down for Drive, and across for the Bass Shaper's Threshold or the
// Waveshaper's Curve, one undo step per drag; double-click sets Drive to 0 dB.
// Everything moves in refreshDisplays(); when nothing moves it stops repainting.

#include "audio/SaturatorResponse.h"
#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include <QtQml/qqmlregistration.h>

#include <vector>

namespace sub::ui {

class SaturatorCurve : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double inputLevel READ inputLevel NOTIFY levelsChanged)  // the latest in_peak (linear)
    // For the editor, from the engine: the Type list's Bass Shaper and Waveshaper entries, Hi-Quality's latency.
    Q_PROPERTY(int bassShaperType READ bassShaperType CONSTANT)
    Q_PROPERTY(int waveshaperType READ waveshaperType CONSTANT)
    Q_PROPERTY(int hqLatency READ hqLatency CONSTANT)

public:
    static constexpr int kWidth = 160;
    static constexpr int kMinimumHeight = 96;
    static constexpr double kStrip = 5.0;               // the In and Out strips' thickness
    static constexpr double kGap = 3.0;                 // between them and the plot
    static constexpr double kOutputRange = 1.15;        // the y axis: ±this over the plot (full scale shows inside it)
    static constexpr double kDrivePerPixel = 0.25;      // dB (Shift: kFine of it)
    static constexpr double kThresholdPerPixel = 0.25;  // dB
    static constexpr double kCurvePerPixel = 0.5;       // %
    static constexpr double kFine = 0.2;
    static constexpr double kMorphSeconds = 0.04;     // a new shape eases in with this time constant
    static constexpr double kDotFallSeconds = 0.15;   // the dots fall back with this one
    static constexpr double kGlowHoldSeconds = 0.3;   // the afterglow holds the highest dot this long,
    static constexpr double kGlowFallSeconds = 0.25;  // then falls with this
    static constexpr double kOverSeconds = 0.3;       // the over-full-scale flash fades with this
    static constexpr double kHoldSeconds = 0.1;       // longer without values than a block's gap: silence
    static constexpr double kMeterFloorDb = -90.0;
    static constexpr double kThresholdMarkerGap = 3.0;  // px: the Bass Shaper's markers nearer the middle: not drawn

    explicit SaturatorCurve(QQuickItem* parent = nullptr);

    // The input levels the curve is worked out at, the curve there (the parameters' shape), and the
    // curve as drawn now (on its way there while it morphs).
    const std::vector<double>& inputs() const { return inputs_; }
    const std::vector<double>& targetCurve() const { return target_; }
    const std::vector<double>& drawnCurve() const { return drawn_; }
    bool morphing() const { return morph_.value != morph_.target; }

    double dotLevel() const { return dot_; }                // where the dots are (input level, falling back)
    double inputLevel() const { return latestIn_; }         // the latest in_peak (held over a block's gap)
    double outputLevel() const { return latestOut_; }       // the latest out_peak (the same)
    double outputBar() const { return outLevel_; }          // the Out strip's bar (falling back as the dots)
    double saturation() const { return sat_.value; }        // how far the curve bends at the dot, eased (0..1)
    double saturationTarget() const { return satTarget_; }  // the same, now
    double overFlash() const { return over_; }              // the over-full-scale flash (1 lit, fading to 0)
    double glowLevel() const { return glow_; }              // the afterglow's reach
    bool settled() const { return settled_; }               // nothing moves: no repainting

    static int bassShaperType();
    static int waveshaperType();
    static int hqLatency();

    QRectF plot() const;
    double xOf(double input) const;
    double yOf(double output) const;

Q_SIGNALS:
    void levelsChanged();

protected:
    void sync() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void makeInputs();
    std::vector<double> curveOf(const sub::app::SaturatorShape& shape) const;
    double curveAt(double input) const;                               // the target curve, between its points
    double drawnAt(double input) const;                               // the drawn one
    std::vector<QPointF> pointsWithin(double from, double to) const;  // the drawn curve over from..to
    bool bass() const { return shape_.type == bassShaperType(); }
    bool waveshaper() const { return shape_.type == waveshaperType(); }

    std::vector<double> inputs_;
    sub::app::SaturatorShape shape_;
    double mix_ = 100.0;
    bool hq_ = false;
    bool synced_ = false;
    double slope_ = 1.0;           // the curve's at 0
    double thresholdInput_ = 0.0;  // the Bass Shaper's, as an input level
    std::vector<double> target_, from_, drawn_;
    Eased morph_;

    double latestIn_ = 0.0, latestOut_ = 0.0;
    double quietFor_ = 0.0;  // seconds without values
    double dot_ = 0.0;
    double outLevel_ = 0.0;
    Eased dotAlpha_;
    Eased sat_;
    double satTarget_ = 0.0;
    double glow_ = 0.0, glowHeld_ = 0.0;
    double over_ = 0.0;
    MeterBallistics in_, out_;
    bool settled_ = true;

    QString gesture_;  // the drag's merge key ("": none)
    QString across_;   // what dragging across sets (the Bass Shaper's Threshold, the Waveshaper's Curve; "": nothing)
    // Where the mouse last was, and the values dragged so far (unrounded): each move adds its own
    // distance, so Shift can come and go mid-drag without a jump. And how far it went each way.
    QPointF lastAt_;
    double dragDrive_ = 0.0, dragThreshold_ = -18.0, dragCurve_ = 50.0;
    sub::app::SaturatorRange driveRange_, thresholdRange_, curveRange_;  // what they stay within (the engine's)
    double movedAcross_ = 0.0, movedUp_ = 0.0;
};

}  // namespace sub::ui
