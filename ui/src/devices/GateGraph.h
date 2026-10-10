#pragma once

// The Gate's display, as Ableton's: the last kHistorySeconds of the levels
// scrolling right to left, the input in light grey and the output over it in a
// darker grey with a white outline (so what the gate takes away shows as the
// light part above the dark), shaded faintly blue where the gate let sound
// through; the key in green while it isn't the plain input (a sidechain, or the
// EQ on it). Across it the threshold (a blue line) and where an open gate closes
// again (Return: an orange dashed line below it, the band between them tinted),
// both dragged up and down (one undo step per drag, relative to where the press
// was; Shift finely), double-clicked back to their defaults, right-clicked for
// their parameters' menus. An LED and "Open" or "Closed" at the top left ("Idle"
// while no values come: the device off, the engine stopped), the newest key level
// as a dot on the right edge (blue at or above the threshold, a ring rippling out
// as the gate opens), and two meters in wells, captioned, both on the plot's dB
// axis: the input's level ("In") and how far the gate turns it down ("Gate").
//
// It reads the device's displays "input", "output", "key" and "open" (one value
// per 256 samples, pushed together) by absolute index into a ring per stream, so
// they line up whatever each read got. It scrolls smoothly: the drawing moves on
// by the time since the last tick, a fraction of a pixel at a time, a steady
// 30 ms behind the newest value (which absorbs the audio's block-sized bursts),
// and stops where the values stop. Every animation steps in refreshDisplays()
// (about 60 times a second) by the time since the last; it repaints only while
// something moves or changes what is drawn (a history of silence scrolling by
// changes nothing).

#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include <QElapsedTimer>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <vector>

namespace sub::ui {

class GateGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double levelIn READ levelIn NOTIFY levelsChanged)    // dB: the loudest "input" of the last tick
    Q_PROPERTY(double levelOut READ levelOut NOTIFY levelsChanged)  // dB: the loudest "output" of the last tick
    Q_PROPERTY(double levelKey READ levelKey NOTIFY levelsChanged)  // dB: the loudest "key" of the last tick
    Q_PROPERTY(double passing READ passing NOTIFY levelsChanged)    // the latest "open", 0..1
    Q_PROPERTY(bool open READ isOpen NOTIFY levelsChanged)          // passing >= 0.5
    Q_PROPERTY(bool idle READ idle NOTIFY levelsChanged)            // no values for a while: nothing passes
    Q_PROPERTY(bool keyed READ keyed NOTIFY sidechainChanged)       // a sidechain is chosen
    Q_PROPERTY(QString sidechainName READ sidechainName NOTIFY sidechainChanged)  // its track's name, or ""
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)  // a line is being dragged

public:
    static constexpr int kWidth = 280;
    static constexpr int kMinimumHeight = 100;
    static constexpr double kFloorDb = -72.0;  // the level axis
    static constexpr double kCeilingDb = 6.0;
    static constexpr double kHistorySeconds = 2.5;
    static constexpr int kCapacity = 2048;  // values kept per stream (2.5 s at 192 kHz is 1875)
    static constexpr double kLineGrab = 6.0;     // px either side of a line that picks it up
    static constexpr double kRightStrip = 62.0;  // the dB figures and the two meters, captioned
    enum Stream { Input = 0, Output, Key, Open, kStreams };

    explicit GateGraph(QQuickItem* parent = nullptr);

    double levelIn() const { return levelIn_; }
    double levelOut() const { return levelOut_; }
    double levelKey() const { return levelKey_; }
    // The key's level now (the tick's, not the dot's falling one) at or above the threshold: the dot is blue.
    bool keyAbove() const { return levelKey_ >= thresholdDb_; }
    double keyDotDb() const { return keyMeter_.level; }  // the dot's level as drawn (falling smoothly)
    double passing() const { return passing_; }
    bool isOpen() const { return passing_ >= 0.5; }
    bool idle() const { return idle_; }
    bool keyed() const { return keyed_; }
    QString sidechainName() const { return sidechainName_; }
    bool dragging() const { return drag_ != Line::None; }

    QRectF plot() const;
    double yOf(double db) const;  // held to the plot
    double dbAt(double y) const;  // held to kFloorDb..kCeilingDb
    double thresholdY() const { return yOf(threshold_.value); }  // as drawn (eased)
    double returnY() const { return yOf(return_.value); }
    double scroll() const { return scroll_; }    // where the plot's right edge is, in values (absolute index)
    qint64 newest() const { return newest_; }    // the index after the latest value all four streams have
    int historySize() const;                     // values held
    float historyAt(int stream, qint64 index) const;
    // Not at rest: the last tick asked for a repaint, or a meter is still falling (or its peak held).
    bool animating() const { return animating_; }
    QRectF inMeter() const;    // the meters' wells
    QRectF gateMeter() const;
    // Whether Floor at `floorDb` is silence (its bottom: the editor shows it as "−inf dB").
    Q_INVOKABLE bool floorIsSilent(double floorDb) const;

Q_SIGNALS:
    void levelsChanged();
    void sidechainChanged();
    void draggingChanged();
    // A line right-clicked: its parameter's menu ("threshold" or "return"; the editor opens it).
    void paramMenuRequested(const QString& paramId);

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

private:
    enum class Line { None, Threshold, Return };

    // Reads what the displays published since the last tick; whether anything came.
    bool readStreams(bool& restarted);
    void write(int stream, qint64 index, float value);
    qint64 historyBegin() const;
    int bucketSize() const;  // values per drawn column, so a column is at least a pixel wide
    void readSidechain();
    Line lineAt(const QPointF& pos) const;
    void hoverAt(const QPointF& pos);
    void endDrag();

    // The parameters, as sync() read them.
    double thresholdDb_ = -12.0;
    double returnDb_ = 3.0;
    double floorDb_ = -40.0;
    bool listening_ = false;
    bool showKey_ = false;  // the key isn't the plain input (a sidechain, or the EQ): it is drawn
    bool keyed_ = false;
    QString sidechainTrack_;
    QString sidechainName_;
    bool synced_ = false;  // sync() has run once (the lines start where they are)
    QMetaObject::Connection projectConnection_;  // the project's trackChanged: the source renamed

    // The history: a ring per stream, by absolute index.
    std::array<std::vector<float>, kStreams> rings_;
    std::array<qint64, kStreams> ends_{};    // after each stream's latest value (-1: not known)
    std::array<float, kStreams> last_{};     // each stream's latest value
    qint64 begin_ = 0;                       // the first index every stream has
    qint64 newest_ = 0;
    qint64 lastVaried_ = 0;  // the latest index where a value differs from the one before (the picture is
                             // the same either side of anything older)
    bool started_ = false;
    double rate_ = 48000.0 / 256.0;  // values a second
    double scroll_ = 0.0;
    float previousOpen_ = 0.f;

    // What the ticks move on.
    QElapsedTimer clock_;
    double sinceValues_ = 1e9;  // seconds since values last came
    double levelIn_ = kFloorDb, levelOut_ = kFloorDb, levelKey_ = kFloorDb;
    double passing_ = 0.0;
    bool idle_ = true;
    std::array<double, 4> emitted_{kFloorDb, kFloorDb, kFloorDb, 0.0};  // the levels levelsChanged last told of
    bool emittedIdle_ = true;
    double drawnIn_ = 0.0, drawnPeak_ = 0.0, drawnKey_ = 0.0;  // where the last repaint put the meter and the dot (y)
    bool drawnKeyAbove_ = false;                               // and whether the dot was blue
    MeterBallistics inMeter_;
    MeterBallistics keyMeter_;  // the key dot: rises at once, falls quickly (its colour is the level now)
    Eased led_;                 // how open, for the LED
    Eased reduction_;           // dB the gate turns it down, for its meter
    Eased threshold_, return_;  // the lines, in dB (the return line's is the level it closes below)
    Eased hoverThreshold_, hoverReturn_;
    double ping_ = -1.0;   // seconds since the gate last opened (< 0: no ring)
    double pulse_ = 0.0;   // seconds, for the listening label
    bool animating_ = false;

    // The mouse.
    Line drag_ = Line::None;
    Line hover_ = Line::None;
    QString gesture_;       // the drag's merge key
    double dragValue_ = 0;  // the dragged parameter as the drag has it (unclamped)
    double lastY_ = 0;

    // paint()'s scratch, sized once and reused.
    std::vector<float> inTops_, outTops_, keyTops_, bottoms_, shadeTops_, opens_;
    std::vector<QPointF> outline_, keyPoints_;
};

}  // namespace sub::ui
