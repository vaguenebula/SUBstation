#pragma once

// The Phaser-Flanger's graph: the response that plays, over the LFO's shape.
//
// - The response, on a 20 Hz..20 kHz log axis, +18..-30 dB: worked out from the
//   engine's own maths (sub::app::phaserCurvePoints, the application layer's
//   wrapper of PhaserDesign.h) at the sweep the engine publishes, so the notches
//   (or the comb) move as the sound does. A comb finer than the pixels (the
//   Doubler high up) is drawn as a band between its peaks and its notches, steady
//   as the delay sweeps, rather than a line aliasing into noise. The right
//   channel's curve, where it differs, is drawn thin in blue; the notches are
//   marked along the bottom; the envelope's level is a bar at the left; In and
//   Out meters at the right.
// - The LFO strip under it: the shape over a cycle with the phase's dot, its
//   comet trail and the right LFO's dot (the random shapes: a trace of their
//   values, scrolling), and a bar of the summed modulation the sweep follows.
// - The engine publishes its values a block at a time; DisplayPlayback plays
//   them back at their own pace, so the curve glides at the screen's rate.
//   Without values for kStaleSeconds (stopped, silent, bypassed), the curve
//   eases back to where the parameters put it and the dots dim.
// - Drag across for the Center (Flanger: the comb's first notch; Doubler: the
//   Time), up and down for the Spread (the delay modes: the Feedback), one undo
//   step per drag; double-click to reset them.
// - Expanded (LFO 2, the envelope and Safe Bass shown) is view state, kept per
//   device while the application runs, not saved.

#include "audio/PhaserResponse.h"
#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include <QElapsedTimer>
#include <QPointF>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <cmath>
#include <vector>

namespace sub::ui {

// Display values played back at the audio's own pace. The engine publishes them a block at a
// time (a 1024-frame block brings four at once, then nothing for 21 ms) while the screen ticks
// every 16.7 ms: drawing the latest value each tick would move the curve in uneven jerks. A
// playhead runs through the frames at their rate, about the largest recent batch behind the
// newest, interpolating between them; a little faster when further behind, a little slower when
// nearer (never backwards), and jumping only when it falls far behind (or waited at the newest
// for frames that then came). All the streams arrive together, so one playhead serves them all.
// Pure arithmetic on a fixed ring.
class DisplayPlayback {
public:
    static constexpr int kStreams = 11;
    static constexpr int kCapacity = 64;      // frames kept
    static constexpr int kMaxTarget = 16;     // the target lag's most, in frames
    static constexpr double kWindow = 0.5;    // seconds of batches the target lag looks back on
    using Frame = std::array<float, kStreams>;

    // A frame, in the order they arrived; then endBatch(how many came this tick, 0: none).
    void append(const Frame& frame);
    void endBatch(int batch);
    // Moves the playhead on by `dtSeconds` at `framesPerSecond`.
    void advance(double dtSeconds, double framesPerSecond);
    // A stream's value at the playhead: linear between the frames either side; a phase (0..1) the
    // short way round; in log (for sweeps, which are positive).
    double value(int stream) const;
    double phase(int stream) const;
    double logValue(int stream) const;
    void snapToNewest();
    void clear();

    bool empty() const { return count_ == 0; }
    qint64 newest() const { return count_ - 1; }  // the newest frame's number (from 0)
    double head() const { return head_; }         // where the playhead is, in frames
    double lag() const { return empty() ? 0.0 : double(newest()) - head_; }
    int target() const;                           // the lag it keeps to
    // Frame `index`'s value of `stream` (held to the frames kept).
    float at(qint64 index, int stream) const;

private:
    struct Batch {
        double age = 0.0;
        int size = 0;
    };
    template <typename Mix>
    double interpolate(int stream, Mix mix) const;

    std::array<Frame, kCapacity> frames_{};
    qint64 count_ = 0;     // frames appended
    double head_ = 0.0;    // the playhead (a frame number)
    bool waiting_ = true;  // at the newest, waiting for more
    std::array<Batch, 32> batches_{};  // the recent batches (a ring)
    int batchCount_ = 0, batchNext_ = 0;
};

class PhaserGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    // LFO 2, the envelope and Safe Bass shown (view state, per device, not saved).
    Q_PROPERTY(bool expanded READ expanded WRITE setExpanded NOTIFY expandedChanged)
    // Values coming from the engine (the curve is the engine's; else the parameters').
    Q_PROPERTY(bool live READ live NOTIFY displaysChanged)
    // The sweep as drawn: the left stages' centre (Hz) or the delay (ms).
    Q_PROPERTY(double sweep READ sweepLeft NOTIFY sweepChanged)
    // The Flanger's first notch as text ("Notch 200 Hz"; "" in the other modes).
    Q_PROPERTY(QString notchText READ notchText NOTIFY sweepChanged)
    Q_PROPERTY(int mode READ mode NOTIFY curveChanged)

public:
    static constexpr int kWidth = 240;
    static constexpr int kMinimumHeight = 100;
    static constexpr double kLow = 20.0;  // Hz across the plot
    static constexpr double kHigh = 20000.0;
    static constexpr double kTopDb = 18.0;  // dB up the plot
    static constexpr double kBottomDb = -30.0;
    static constexpr int kHeader = 16;          // the text over the plot
    static constexpr int kStripHeight = 34;     // the LFO strip at the bottom: its name over the shape
    static constexpr int kMeterWidth = 4;       // In and Out, right of the plot
    static constexpr double kSpreadPixels = 150.0;    // dragged up this far: Spread +100 %
    static constexpr double kFeedbackPixels = 150.0;  // the same for Feedback
    static constexpr double kStaleSeconds = 0.3;      // no display values this long: not live
    static constexpr int kTrail = 10;                 // the dot's comet trail, in ticks
    static constexpr int kTrace = 512;                // the random shapes' trace: 2.7 s of values
    static constexpr int kMarkerSpacing = 8;          // px: comb markers closer than this are left out
    static constexpr double kBandPeriod = 8.0;        // px: a comb whose cycle is shorter is drawn as a band
    static constexpr double kMinFlangeMs = 0.1, kMaxFlangeMs = 20.0;  // the times' ranges (the parameters')
    static constexpr double kMinDoublerMs = 20.0, kMaxDoublerMs = 150.0;

    explicit PhaserGraph(QQuickItem* parent = nullptr);

    QRectF plot() const;    // the response
    QRectF strip() const;   // the LFO
    QRectF meters() const;  // In and Out, right of the plot
    LogAxis frequencyAxis() const;
    double xOf(double freq) const;
    double freqAt(double x) const;
    double yOf(double db) const;
    // The delay modes' drag across: Flanger, the comb's first notch under the mouse (500 / freqAt(x)
    // ms, held to 0.1..20); Doubler, log across 20..150 ms, the longest at the left.
    double xOfTime(double ms) const;
    double timeAt(double x) const;

    // What is drawn: the left's curve (phaserCurvePoints at the drawn sweep), the right's (empty
    // while it is the left's), the notch markers (Hz) and their alphas.
    const sub::app::PhaserCurvePoints& curveLeft() const { return left_; }
    const sub::app::PhaserCurvePoints& curveRight() const { return right_; }
    const std::vector<double>& notchMarkers() const { return markers_; }
    // The curve's settings bar the sweep (what the parameters give), for the tests.
    sub::app::PhaserCurve curveSettings() const;

    double sweepLeft() const { return std::exp2(sweepL_.value); }
    double sweepRight() const { return std::exp2(sweepR_.value); }
    double qLeft() const { return std::exp2(qL_.value); }
    double qRight() const { return std::exp2(qR_.value); }
    double lfoPhase() const { return lfoPhase_; }
    double lfoPhaseRight() const { return lfoPhaseRight_; }
    double lfoValue() const { return lfoValue_; }
    double modulation() const { return modulation_; }
    double envelope() const { return envBar_.value; }
    double levelIn() const { return in_.level; }  // the meters (with ballistics), dB
    double levelOut() const { return out_.level; }
    double dotOpacity() const { return dotOpacity_.value; }
    const DisplayPlayback& playback() const { return playback_; }
    int mode() const { return mode_; }
    bool live() const { return live_; }
    QString notchText() const;
    bool expanded() const;
    void setExpanded(bool on);
    // Whether the last tick asked to be drawn again (something moved).
    bool moving() const { return moving_; }

Q_SIGNALS:
    void expandedChanged();
    void displaysChanged();
    void curveChanged();
    void sweepChanged();

protected:
    void sync() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    struct Dot {
        double phase = 0.0, value = 0.0;
    };
    // A curve as drawn, in pixels: its line (where a comb is finer than kBandPeriod, the comb's top)
    // and the runs of those columns, a band each between the comb's peaks and its notches.
    struct CurveLayout {
        struct Band {
            double x = 0.0;
            std::vector<float> tops, bottoms;
        };
        std::vector<QPointF> line;
        std::vector<Band> bands;
    };
    CurveLayout layOut(const sub::app::PhaserCurvePoints& curve, bool withBands) const;

    void updateShape();
    void updateCurve();
    void dragTo(const QPointF& pos);
    // The sweep and Q where the parameters put them (no modulation), in log2.
    double staticSweepLog() const;
    double staticQLog() const;
    // The shape's value at `phase` (the deterministic shapes), as the strip draws it.
    double shapeAt(double phase) const;
    bool randomShape() const;
    QRectF lane() const;  // where the strip draws the shape
    // The frames the playhead passed since the last tick, into the random shapes' trace.
    void trace();
    // Whether the playhead's frame is of the mode shown (not one on its way before a mode change).
    bool frameFits() const;
    // The drawn sweep catching up with the engine's over a few ticks, from where it is now.
    void startCatchUp();
    void paintResponse(SgPainter& p);
    void paintStrip(SgPainter& p);

    // The parameters (sync()).
    int mode_ = 0;
    int notches_ = 4;
    double center_ = 1000.0, spread_ = 50.0, blend_ = 0.0;
    double flangeTime_ = 2.5, doublerTime_ = 30.0;
    double feedback_ = 50.0;
    bool fbInvert_ = false;
    double warmth_ = 0.0, mix_ = 50.0, safeBass_ = 5.0, output_ = 0.0;
    int wave_ = 1;
    double duty_ = 0.0, rateHz_ = 0.5;
    double phaseOffset_ = 0.5;  // Phase, in cycles
    bool spinOn_ = false;
    double lfo2Mix_ = 0.0;
    bool envOn_ = false;
    bool first_ = true;  // the first sync since the device came: everything snaps
    std::vector<float> shape_;  // the LFO's shape across the lane

    // What is drawn (refreshDisplays()): the sweeps and Q in log2.
    Eased sweepL_, sweepR_, qL_, qR_;
    Eased catchUp_;                     // 1 .. 0: how much of catchFrom_ is still in what is drawn
    std::array<double, 4> catchFrom_{};  // the sweeps and Q a catch-up started from
    bool fitted_ = false;               // the last tick's frame was of the mode shown
    double lfoPhase_ = 0.0, lfoPhaseRight_ = 0.5, lfoValue_ = 0.0, modulation_ = 0.0;
    Eased envBar_, dotOpacity_, oldFade_;
    MeterBallistics in_, out_;
    bool live_ = false;
    bool moving_ = false;
    double sinceArrival_ = 1e9;  // seconds without display values (the ticks' times)
    qint64 holdUntil_ = -1;      // a mode change: frames before this one may be the old mode's
    DisplayPlayback playback_;
    std::array<std::vector<float>, DisplayPlayback::kStreams> pending_;  // read, not yet whole frames
    std::array<Dot, kTrail> trail_{};
    int trailCount_ = 0, trailNext_ = 0, trailSettle_ = 0;
    std::vector<float> trace_;  // the random shapes' values, a ring of kTrace
    int traceNext_ = 0, traceCount_ = 0;
    qint64 traced_ = -1;  // the last frame put in the trace
    QElapsedTimer clock_;
    double drawnLeft_ = 0.0, drawnRight_ = 0.0, drawnQLeft_ = 0.0, drawnQRight_ = 0.0;  // the curve's

    // The curve.
    sub::app::PhaserCurvePoints left_, right_;
    CurveLayout leftLayout_, rightLayout_, oldLayout_;  // (old: the mode's before a change, fading out)
    std::vector<double> markers_, markerAlpha_;

    // Dragging.
    QString gesture_;  // the drag's merge key ("": none)
    QPointF pressedAt_;
    double pressedValue_ = 0.0;  // the Spread, or the Feedback
    QMetaObject::Connection bridgeConnection_;  // the bridge's deviceChanged: a new sample rate
};

}  // namespace sub::ui
