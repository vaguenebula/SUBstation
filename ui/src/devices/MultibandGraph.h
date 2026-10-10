#pragma once

// Multiband Dynamics' display, after Ableton's: a lane per band (High on top),
// a level axis from -80 to +6 dB across them. In each lane the Below region is
// a block from the left edge to the Below threshold, the Above region one from
// the Above threshold to the right edge, tinted by what it does (orange: the
// level is pulled down, teal: up) and hatched more densely the further its
// ratio is from 1:1. The band's level before the dynamics is a thin bar, after
// them a thick one in the meters' colours, with the gain change between the two
// in orange or teal, a marker where the static curve is taking it, and the
// change in figures. Drag a block's edge for its threshold, inside a block up or
// down for its ratio (the block's level follows the mouse): one undo step per
// gesture. Ctrl: every band at once; Alt: both thresholds of a band together;
// Shift: finely. Double-click resets; the wheel steps (a run of it is one
// gesture). Ctrl+Alt and Shift+wheel are the device chain's (it scrolls).
//
// It reads the device's displays (`<band>_in`, `_out`, `_gain`) each display
// tick, the recent ones only (a stall's backlog is old audio), and eases
// everything it draws there: meters with ballistics, the gain change (once the
// audio stops, going down with the meters), each side's glow while it works,
// highlights under the mouse, lanes dimmed when switched off or muted by a solo.
// Once all of it has settled, it stops repainting.

#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include <QElapsedTimer>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <optional>
#include <utility>

namespace sub::ui {

class MultibandGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double rowHeight READ rowHeight NOTIFY layoutChanged)
    Q_PROPERTY(bool sidechained READ sidechained NOTIFY sidechainedChanged)  // a sidechain source is chosen

public:
    enum Band { Low = 0, Mid = 1, High = 2 };  // as the engine's; drawn High on top
    enum Side { Below = 0, Above = 1 };
    static constexpr int kBands = 3;
    static constexpr int kWidth = 268, kHeaderHeight = 16, kMinRowHeight = 38;
    static constexpr double kFloorDb = -80.0, kCeilingDb = 6.0;  // the level axis, linear
    static constexpr double kHandleGrab = 5.0;    // px either side of a threshold's line
    static constexpr double kRatioPixels = 30.0;  // dragged this far, a ratio doubles (or halves)
    static constexpr double kFineFactor = 0.2;    // with Shift
    static constexpr double kRatioDetent = 0.03;  // within 3 % of 1:1 a dragged ratio is 1:1
    static constexpr double kWheelGesture = 0.6;  // s: wheel notches this close are one undo step
    static constexpr double kWheelStill = 5.0;    // px: the mouse moved further between notches ends a run
    static constexpr double kTick = 0.016;        // s per display tick, for the animation
    static constexpr int kHoldTicks = 15;         // ticks without values before the meters let go
    // s of display values a tick looks at, the latest: a whole buffer's arrive at once (2048 samples at 44.1 kHz,
    // 46 ms), and older ones are a stall's backlog (a hidden editor shown again, an offline render).
    static constexpr double kRecentSpan = 0.1;
    static constexpr int kMeterSamples = 256;     // audio per display value (the device's)

    explicit MultibandGraph(QQuickItem* parent = nullptr);

    double rowHeight() const;
    // The band's lane: x 1..width-1, y row+2 .. row+rowHeight-2.
    QRectF lane(int band) const;
    double xOfDb(double db) const;  // held to the axis
    double dbAtX(double x) const;
    double pixelsPerDb() const;
    // A threshold's handle: its line's x, the lane's middle.
    QPointF aboveHandle(int band) const;
    QPointF belowHandle(int band) const;
    // A point inside each block, clear of its handle.
    QPointF aboveBlockPoint(int band) const;
    QPointF belowBlockPoint(int band) const;

    // The meters as drawn (ballistics applied), dB.
    double inLevel(int band) const { return bands_[index(band)].in.level; }
    double outLevel(int band) const { return bands_[index(band)].out.level; }
    double outPeak(int band) const { return bands_[index(band)].out.peak; }
    double gainShown(int band) const { return bands_[index(band)].gain.value; }   // eased, dB
    double gainTarget(int band) const { return bands_[index(band)].gain.target; }  // the latest read, dB
    // Where a steady level `inDb` comes out of the band's dynamics: inDb + multibandGainDb(its settings).
    double staticOutDb(int band, double inDb) const;
    bool bandOn(int band) const { return bands_[index(band)].on; }  // Mid always; High and Low their switch
    // The gain change drawn on a lane's bar, dB: from where the level would be without it (never past the level
    // before it: the out meter stops at the floor, a change can go on far under it) to the out level.
    std::pair<double, double> changeSpan(int band) const;
    // Where the static curve is taking the band, dB, by what the displays last reported: none while the
    // dynamics have settled there (within 0.5 dB, a level under the floor as at it) or nothing sounds.
    std::optional<double> targetMarkerDb(int band) const;
    bool sidechained() const { return sidechained_; }

    // The animation's state (each eased per tick).
    double glow(int band, int side) const { return bands_[index(band)].glow[side == Above].value; }
    double laneOpacity(int band) const { return bands_[index(band)].opacity.value; }
    double highlight(int band, int side, bool handle) const {
        const BandView& v = bands_[index(band)];
        return (handle ? v.handleLight : v.blockLight)[side == Above].value;
    }
    bool animating() const { return animating_; }  // the last tick moved something (and repainted)
    // A switched-off band's "→ Mid" label: its box, and how much it shows (with the lane's dimming; it gives way to
    // a drag's bubble over it).
    QRectF offLabelRect(int band) const;
    double offLabelOpacity(int band) const;
    QRectF bubbleRect() const;  // the drag's value, over the handle or at the mouse; empty without a drag
    // The gain change in figures at the lane's top right, and the static curve's readout beside the hairline under
    // the mouse (clear of the figure): their boxes, empty while not drawn.
    QRectF gainLabelRect(int band) const;
    QRectF hoverLabelRect(int band) const;

    Q_INVOKABLE QString ratioText(double ratio) const;           // "1:4.00", "1:0.500"
    Q_INVOKABLE double parseRatio(const QString& text) const;   // 0: unreadable
    Q_INVOKABLE QVariant parseTime(const QString& text) const;  // ms ("250 ms", "1.5 s", "80"); null: unreadable

Q_SIGNALS:
    void layoutChanged();
    void sidechainedChanged();
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
    void wheelEvent(QWheelEvent* event) override;
    void hoverEnterEvent(QHoverEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    // A band's settings, as its parameters are now.
    struct Settings {
        double above = -20.0, aboveRatio = 1.0, below = -40.0, belowRatio = 1.0;
        bool solo = false;

        double threshold(int side) const { return side == Above ? above : below; }
        double ratio(int side) const { return side == Above ? aboveRatio : belowRatio; }
    };
    struct BandView {
        Settings settings;
        bool on = true;
        MeterBallistics in, out;
        Eased gain;
        // The displays' latest: in and out held a while, then the floor; the gain kept (lettingGoGain's).
        double inRead = kFloorDb, outRead = kFloorDb, gainRead = 0.0;
        std::array<Eased, 2> glow;         // [Below, Above]: the side is working
        std::array<Eased, 2> handleLight;  // under the mouse or dragged
        std::array<Eased, 2> blockLight;
        Eased opacity;  // 1, 0.5 (muted by a solo), 0.35 (switched off)
        Eased offLabel;  // 1; 0 while a drag's bubble covers it
    };
    // What the mouse is on: a threshold's handle or inside a block.
    struct Target {
        int band = Mid;
        int side = Above;
        bool handle = false;

        bool operator==(const Target&) const = default;
    };
    struct Drag {
        Target target;
        QString gesture;
        double lastX = 0.0, lastY = 0.0;  // where the mouse was last
        double dx = 0.0, dy = 0.0;        // moved since the press (Shift scaled: it may change mid-drag)
        bool every = false;               // Ctrl: every band
        bool both = false;                // Alt: both thresholds
        std::array<Settings, kBands> start;
        QPointF at;  // the mouse, for the bubble (a wheel run: at its last notch)
    };

    static std::size_t index(int band) { return std::size_t(std::clamp(band, 0, kBands - 1)); }
    static QString paramId(int band, const char* field);
    std::optional<Target> targetAt(const QPointF& pos) const;
    double sideGainDb(int band, int side, double levelDb) const;  // one side's static change
    void setTargets();  // the eased values' targets from the settings, levels and the mouse
    // The gain change shown once the audio has stopped and the meters fall: the change between them, so the bars
    // and the figure agree all the way down (or the last reading, where it goes on past a meter at the floor).
    static double lettingGoGain(const BandView& view);
    void setHover(const std::optional<Target>& target, const std::optional<QPointF>& at);
    void dragTo(const QPointF& pos, Qt::KeyboardModifiers modifiers);
    void writeThresholds(const Drag& drag, double deltaDb, const QString& gesture);
    void writeRatios(const Drag& drag, double factor, const QString& gesture);
    static double ratioStep(double ratio);  // held, 1:1 within the detent, 3 significant digits
    QString bubbleText() const;
    QString gainText(int band) const;                 // empty while not drawn
    std::optional<double> hoverLine(int band) const;  // the hairline's x, while the mouse is over the lane
    QString hoverText(int band) const;

    void paintLane(SgPainter& p, int band) const;
    void paintBubble(SgPainter& p) const;

    std::array<BandView, kBands> bands_;
    double amount_ = 100.0;  // %
    bool softKnee_ = false;
    bool sidechained_ = false;
    int quietTicks_ = 0;     // ticks in a row without display values
    bool animating_ = false;
    bool snap_ = true;  // the next sync shows the device as it is (no easing in)

    std::optional<Target> hover_;
    std::optional<QPointF> hoverAt_;  // the mouse over a lane (the static curve's hairline)
    std::optional<Drag> drag_;
    // A run of the wheel: one gesture on one target, worked out from where it started (as a drag is), so the
    // small steps of a high-resolution wheel or a touchpad add up rather than each being rounded away. It keeps its
    // target while the mouse stays where the last notch was (`at`), though a threshold it moves leaves the mouse.
    std::optional<Drag> wheel_;
    double wheelNotches_ = 0.0;
    QElapsedTimer wheelClock_;
};

}  // namespace sub::ui
