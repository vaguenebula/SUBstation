#pragma once

// The Chorus-Ensemble's display: each voice's delay as it moves, in step with
// the sound. Time runs right to left: the column 10 px in from the right edge is
// now, where a dot rides each voice's delay; the left edge is `windowCycles` of
// the modulation ago (one cycle at slow rates, up to eight at fast ones, so a
// fast rate shows tight cycles instead of strobing). The delay axis runs up over
// the layout's range at Amount 100 (12 % more each side), fixed per layout, so
// turning the Amount visibly grows the swing; the centre is dashed (in Auto it
// rises and falls with the Amount). Left voices are orange, right ones blue,
// each voice of a side a little lighter; Warmth tints them towards red, Feedback
// thickens them, a narrow Width fades the right side towards the left.
//
// Every delay is worked out by the engine's own maths (sub::app::chorusDelayMs,
// the application layer's wrapper of ChorusDesign.h), from the LFO's phase the
// engine publishes (display `phase`): an estimate that runs on at the rate each
// display tick and is pulled towards the engine's newest value (snapping to the
// first after a gap), so the traces scroll smoothly at the display's rate while
// the dots move as the delays do. The wet's level (display `level`), through
// meter ballistics, makes the traces and the dots glow as sound passes.
// Mode, Taps, Time, Amount, Shape and Offset changes ease (the old voices
// fading out as the new fade in, as the engine cross-fades them); the device off
// or nothing rendering, the traces stop and dim; silent, they rest. Nothing
// repaints once everything has settled and no sound passes.
//
// Drag up and down for the Rate (doubling every kRatePixels), across for the
// Amount (kAmountPixels for all of it), Shift four times as finely: one undo step per drag.

#include "audio/ChorusVoices.h"
#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include <QColor>
#include <QElapsedTimer>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <vector>

namespace sub::ui {

class ChorusGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double phase READ phase NOTIFY animated)  // the LFO's phase as drawn (0..1)
    Q_PROPERTY(double glow READ glow NOTIFY animated)    // 0..1
    Q_PROPERTY(bool frozen READ frozen NOTIFY animated)  // nothing rendering, or the device off
    Q_PROPERTY(QString readout READ readout NOTIFY curveChanged)  // the largest detune: "±22 ct"

public:
    static constexpr int kMinimumWidth = 200;
    static constexpr int kMinimumHeight = 80;
    static constexpr double kHeader = 14.0;     // the header over the plot: its name and the readout
    static constexpr double kNowInset = 10.0;   // the "now" column, this far in from the plot's right edge
    static constexpr double kFadeWidth = 36.0;  // the traces' oldest end fades out over this, under the figures
    static constexpr double kRatePixels = 40.0;     // dragged up this far, the Rate doubles
    static constexpr double kAmountPixels = 150.0;  // dragged across this far, the Amount moves 100 %
    static constexpr double kMargin = 0.12;       // the delay axis: the layout's range and 12 % of it each side
    static constexpr double kSnapSeconds = 0.3;   // values after a gap this long (or the first): snap to them
    static constexpr double kSilentDb = -80.0;    // the wet's level below which the traces rest
    static constexpr double kGlowFloorDb = -48.0;  // the glow starts here and is full kGlowRangeDb higher
    static constexpr double kGlowRangeDb = 42.0;
    static constexpr QColor kRightColour{0x6f, 0xc8, 0xff};  // the right voices (cool blue)

    explicit ChorusGraph(QQuickItem* parent = nullptr);

    double phase() const;
    double glow() const { return glow_.value; }
    bool frozen() const { return frozen_; }
    // Frozen, or no sound passing: the traces hold still.
    bool resting() const { return resting_; }
    QString readout() const { return readout_; }

    double windowCycles() const { return window_.value; }  // LFO cycles across the graph (eased)
    int voiceCount() const;                                // traces of the current layout: both sides'
    sub::app::ChorusLayout layout() const { return layout_; }
    double layoutFade() const { return fade_.value; }      // 0..1: the previous layout fading out
    // Where a voice's dot is now: its delay, and the point.
    double voiceDelayMs(int channel, int voice) const;
    QPointF voiceDot(int channel, int voice) const;
    // The current layout's, at the Amount shown.
    double centreMs() const;
    double swingMs() const;
    double axisLowMs() const { return axisLow_.value; }  // the layout's range at Amount 100 (eased)
    double axisHighMs() const { return axisHigh_.value; }
    double peakDetuneCents() const { return detuneCents_; }

    QRectF plot() const;
    double nowX() const;
    double yOf(double ms) const;
    double xOfCycles(double cyclesAgo) const;

Q_SIGNALS:
    void animated();
    void curveChanged();

protected:
    void sync() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;

private:
    int index(const QString& paramId) const;
    // A drag goes on from its last position and values (at the press, and when Shift changes).
    void startDrag(bool fine);
    QColor voiceColour(const sub::app::ChorusLayout& layout, int channel, int voice) const;
    void drawVoices(SgPainter& p, const sub::app::ChorusLayout& layout, double alpha);
    void drawDots(SgPainter& p);
    void drawAxis(SgPainter& p);

    // The parameters, as they are now.
    sub::app::ChorusLayout layout_;
    sub::app::ChorusLayout previous_;  // fading out while fade_ < 1
    double rate_ = 0.8;
    double amount_ = 50.0;
    double feedback_ = 0.0;
    double width_ = 100.0;
    double offset_ = 0.0;
    double shape_ = 0.0;
    double warmth_ = 0.0;
    double mix_ = 50.0;
    bool enabled_ = true;
    double detuneCents_ = 0.0;
    QString readout_;
    bool synced_ = false;  // the first sync with a device snaps everything

    // What eases each display tick.
    Eased window_;       // LFO cycles across
    Eased axisLow_;      // ms
    Eased axisHigh_;
    Eased amountShown_;  // %
    Eased shapeShown_;   // %
    Eased offsetShown_;  // degrees
    Eased fade_;         // the layout's cross-fade, 0..1
    Eased glow_;         // 0..1
    Eased dim_;          // 1, or 0.6 frozen
    MeterBallistics meter_;

    // The LFO's phase as drawn (cycles, 0..1), and the engine's values it follows.
    double estimate_ = 0.0;
    bool haveValues_ = false;
    double sinceValues_ = 0.0;  // seconds of ticks since the last values
    bool frozen_ = true;
    bool resting_ = true;
    QElapsedTimer clock_;

    // A drag: its merge key ("": none), where it started (or Shift last changed) and the values then, and
    // where it was last and what that set (held to the ranges, not yet rounded).
    QString gesture_;
    QPointF pressedAt_;
    double pressedRate_ = 0.8;
    double pressedAmount_ = 50.0;
    bool fine_ = false;  // Shift held: a quarter as far
    QPointF lastAt_;
    double lastRate_ = 0.8;
    double lastAmount_ = 50.0;

    std::vector<QPointF> points_;  // paint()'s, reused per trace (render thread only)
};

}  // namespace sub::ui
