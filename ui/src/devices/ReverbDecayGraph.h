#pragma once

// The Reverb's diffusion network: how long each frequency rings (the decay time
// to -60 dB, on a log axis from 40 ms to 100 s, over 20 Hz..20 kHz), worked out
// from the engine's own network (sub::app::reverbDecaySeconds: the loop of a
// line of the active lines' mean length with its shelves), so the curve is the
// decay that plays; behind it the tail's spectrum (the display "tail", falling
// 1 dB a refresh) as it dies away, and beside it a meter of the tail's level.
//
// Three handles: Decay (in the middle, up and down), and the shelves' (across
// for the frequency, up and down for how long that band rings: Decay times the
// shelf's gain; a dashed guide runs from each to its edge of the plot, where
// the curve settles onto it). A drag moves the handle under the mouse (else the
// one nearest across) relative to where it was pressed, one undo step per drag,
// Shift finely; double-clicking a shelf's handle switches the shelf. Hovering
// one grows it and reads it out.
//
// It moves in advance() (from refreshDisplays(), about 60 times a second): the
// meter's ballistics (falling at least as fast as the tail does), the curve's
// glow while the tail sounds, a ripple along the curve at the chorus's rate
// while it sounds, Freeze lifting the curve to the top in the frozen colour,
// and the switches (the shelves, the high filter's type, Density, Flat, Cut)
// easing the curve to its new shape; dragged values never animate. It repaints
// only while something moves.

#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include "analysis/Spectrum.h"
#include "audio/ReverbResponse.h"

#include <QPointF>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <vector>

namespace sub::ui {

class ReverbDecayGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double tailLevel READ tailLevel NOTIFY levelsChanged)      // dB: the tail meter's level
    Q_PROPERTY(bool frozen READ frozen NOTIFY curveChanged)               // the Freeze parameter
    Q_PROPERTY(double frozenShown READ frozenShown NOTIFY levelsChanged)  // 0..1, eased
    Q_PROPERTY(bool animating READ animating NOTIFY levelsChanged)        // the last tick asked for a repaint
    Q_PROPERTY(QString readout READ readout NOTIFY levelsChanged)        // the top right's text

public:
    static constexpr int kMinimumWidth = 200;  // (the editor gives it the width of the boxes under it)
    static constexpr int kMinimumHeight = 80;
    static constexpr int kMeterWidth = 6;
    static constexpr double kLow = 20.0;  // Hz across the graph
    static constexpr double kHigh = 20000.0;
    static constexpr double kMinSeconds = 0.04;  // the decay axis (log): its bottom
    static constexpr double kMaxSeconds = 100.0;  // and its top
    static constexpr double kHeader = 13.0;       // the captions' strip over the axis
    static constexpr double kMeterFloorDb = -72.0;
    static constexpr double kGrab = 9.0;  // px from a handle that picks it

    enum Handle { None = -1, Lo = 0, Hi = 1, Decay = 2 };

    explicit ReverbDecayGraph(QQuickItem* parent = nullptr);

    // The curve as computed for the settings (not as drawn while it eases): one frequency per column.
    const std::vector<double>& frequencies() const { return frequencies_; }
    const std::vector<double>& seconds() const { return seconds_; }
    double tailLevel() const { return meter_.level; }
    bool frozen() const { return settings_.freeze; }
    double frozenShown() const { return frozen_.value; }
    bool animating() const { return animating_; }
    QString readout() const;
    const sub::app::analysis::FallingSpectrum& tailSpectrum() const { return spectrum_; }
    // The drawn curve's y at each frequency now.
    std::vector<double> shownY() const;

    // The handles (graph coordinates: each held inside the plot), and the one hovered.
    Q_INVOKABLE QPointF loHandle() const { return handleAt(Lo); }
    Q_INVOKABLE QPointF hiHandle() const { return handleAt(Hi); }
    Q_INVOKABLE QPointF decayHandle() const { return handleAt(Decay); }
    QPointF handleAt(Handle handle) const;
    Handle hovered() const { return hovered_; }

    // Graph coordinates: the plot (the item less 1 px and the meter at the right), the decay axis in it
    // (under the captions), the axes.
    QRectF plot() const;
    QRectF axisRect() const;
    QRectF meterRect() const;
    Q_INVOKABLE double xOf(double freq) const;
    Q_INVOKABLE double freqAt(double x) const;
    Q_INVOKABLE double yOf(double seconds) const;

Q_SIGNALS:
    void curveChanged();
    void levelsChanged();

protected:
    void sync() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    // One tick of animation, `seconds` after the last.
    void advance(double seconds);
    void updateCurve();
    void updateTarget();
    void updateShown();
    void updateColumns();
    Handle handleNear(const QPointF& pos, double within) const;
    bool shelfOn(Handle handle) const;
    bool gainless(Handle handle) const;  // a shelf's handle with no gain to drag (off, or the Low-pass)
    void setHovered(Handle handle);
    void startDrag(const QPointF& pos);
    void dragTo(const QPointF& pos, Qt::KeyboardModifiers modifiers);
    void endDrag();
    double yOfLog(double logSeconds) const;
    static double logOf(double seconds);

    // The parameters, as sync() read them.
    sub::app::ReverbDecaySettings settings_;
    bool chorus_ = true;
    double chorusAmount_ = 20.0;
    bool dry_ = false;  // Dry/Wet at 0: none of it heard
    bool synced_ = false;

    // The curve: for the settings, and with Freeze the other way (log seconds of both, unfrozen and
    // frozen, are what is drawn between); the target in log seconds, as drawn now, and where a
    // switch's easing started.
    std::vector<double> frequencies_, seconds_, other_;
    std::vector<double> target_, shown_, from_;
    Eased switch_;  // 0 -> 1 from from_ to target_
    double lowpassAtHi_ = 1.0;  // s: the unfrozen curve at the high filter's frequency (its handle, Low-pass)

    // The tail.
    sub::app::analysis::FallingSpectrum spectrum_;
    std::vector<double> columns_;
    bool spectrumChanged_ = false;
    // The tick's loudest "diffuse" (kept a moment through ticks without one), and the seconds since a tick last
    // brought one.
    double diffuseDb_ = sub::app::kReverbMeterFloorDb;
    double stale_ = 0.0;
    double chorusPhase_ = 0.0;   // the newest "chorus" (the last while still)
    bool chorusMoved_ = false;
    MeterBallistics meter_;
    double paintedLevel_ = -999.0, paintedPeak_ = -999.0;

    // The animation.
    Eased frozen_, glow_, depth_;
    std::array<Eased, 3> grow_;  // each handle's size: 0 (4 px) .. 1 (6 px)
    bool animating_ = false;
    QMetaObject::Connection bridgeConnection_;  // the bridge's deviceChanged: a new sample rate

    // The mouse.
    Handle hovered_ = None;
    Handle pressed_ = None;
    QString gesture_;  // the drag's merge key ("": none)
    bool fine_ = false;
    QPointF anchor_;
    double startFreq_ = 0.0, startGain_ = 0.0, startDecay_ = 0.0;  // the values at the anchor
};

}  // namespace sub::ui
