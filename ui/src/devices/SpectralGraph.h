#pragma once

// The Spectral Compressor's display: the spectrum going in (filled) and coming
// out (a line), on a 20 Hz..20 kHz log axis against the engine's
// pink-referenced level (pink noise reads its own level at every frequency), with
// what the device does there as it plays: the cut at each frequency hanging from
// the top (with the recent deepest held), the lift rising from the bottom, and,
// glowing orange, where the level compared stands over the threshold. Over it the
// threshold (orange, from the engine's own maths: sub::app::spectralThresholdDb)
// with its pivot at 1 kHz and tilt handles at 100 Hz and 10 kHz, the Below
// threshold (green, while Upward is on) with its handle at 300 Hz, the Focus
// band's edges (outside them dimmed by the engine's own Focus weights:
// sub::app::spectralFocusWeights), and In/Out meters at the right edge.
//
// Drag the threshold (its pivot or anywhere on it) up and down, its end handles
// to tilt both thresholds about 1 kHz, the green line for Below, the Focus edges
// sideways; Shift drags finely (from where it is pressed on, as the knobs do), a
// double-click resets, each drag is one undo step, and the value being dragged
// (or hovered) reads in the header.
//
// The engine publishes a frame of 128 values per hop for each spectral display,
// in step with what is heard; frames are reassembled by the values' absolute
// index (FrameAssembler). Everything moves in refreshDisplays() (eased or with
// meter ballistics, by DeviceCanvas::tickSeconds()) and paint() only reads
// members; nothing repaints while nothing moves and no data comes.

#include "audio/SpectralResponse.h"
#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include <QLineF>
#include <QList>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <limits>
#include <vector>

class QHoverEvent;
class QMouseEvent;

namespace sub::ui {

class SpectralGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool keyed READ keyed NOTIFY keyedChanged)  // a sidechain keys it
    // The handle under the mouse (or dragged): a Handle.
    Q_PROPERTY(int hoveredHandle READ hoveredHandle NOTIFY hoverChanged)
    Q_PROPERTY(QString readout READ readout NOTIFY hoverChanged)    // the header's text for it
    Q_PROPERTY(double maxCutDb READ maxCutDb NOTIFY levelsChanged)  // ≥ 0, eased, as the header shows
    Q_PROPERTY(double maxLiftDb READ maxLiftDb NOTIFY levelsChanged)

public:
    enum Handle { None = 0, ThresholdHandle, TiltLow, TiltHigh, BelowHandle, FocusLowEdge, FocusHighEdge };
    Q_ENUM(Handle)

    static constexpr int kPoints = sub::app::kSpectralDisplayPoints;  // per display frame (the engine's)
    static constexpr int kWidth = 376;
    static constexpr int kMinimumHeight = 100;
    static constexpr double kLow = 20.0;  // Hz across the plot
    static constexpr double kHigh = 20000.0;
    static constexpr double kFloorDb = -78.0;  // the level axis (pink-referenced dB)
    static constexpr double kCeilingDb = 18.0;
    static constexpr double kGainScaleDb = 24.0;  // a cut or lift this deep reaches kCurtain of the plot's height
    static constexpr double kCurtain = 0.5;
    static constexpr double kHandleHit = 9.0;  // px: a handle, a line, a Focus edge within this
    static constexpr double kLineHit = 5.0;
    static constexpr double kEdgeHit = 4.0;
    static constexpr double kLowHandleHz = 100.0;  // where the tilt handles and Below's sit
    static constexpr double kHighHandleHz = 10000.0;
    static constexpr double kBelowHandleHz = 300.0;
    static constexpr double kMinFocusOctaves = 1.0 / 3.0;  // the editor keeps the Focus edges this far apart
    static constexpr double kFineDrag = 0.1;               // Shift
    static constexpr double kHeaderLeft = 64.0;            // the QML Sidechain badge's room in the header
    static constexpr double kMeterFloorDb = -60.0;
    static constexpr int kFocusStops = 256;  // the Focus dim's weights, across the plot
    static constexpr std::array<double, 4> kLevelFigures{0.0, -24.0, -48.0, -72.0};  // dB, figured on the grid

    explicit SpectralGraph(QQuickItem* parent = nullptr);

    bool keyed() const { return keyed_; }
    int hoveredHandle() const { return dragged_ != None ? dragged_ : hovered_; }
    QString readout() const { return readoutText_; }
    double maxCutDb() const { return maxCut_.value; }
    double maxLiftDb() const { return maxLift_.value; }

    QRectF plot() const;
    LogAxis frequencyAxis() const;
    double xOf(double hz) const;
    double freqAt(double x) const;
    double yOfLevel(double db) const;
    double dbPerPixel() const;
    // Where a handle is: 1 the pivot (1 kHz), 2 and 3 the tilt handles (100 Hz, 10 kHz), 4 Below's (300 Hz on
    // the green line), each on its line as drawn; where the line has left the plot by then, at the point it
    // leaves it (towards 1 kHz, where both lines are always inside). 5 and 6 the Focus edges' grips (at the
    // plot's bottom; an open edge's just inside the plot's border).
    QPointF handle(int which) const;
    // The lines as drawn (eased), dB.
    double thresholdAt(double hz) const;
    double belowAt(double hz) const;
    // The lines as drawn, straight across the plot from 20 Hz to 20 kHz: their ends where those levels map, beyond
    // the plot where a line leaves it (the plot clips it there).
    QLineF thresholdLine() const;
    QLineF belowLine() const;
    // How much of a level figure (kLevelFigures, inside the plot's left edge) is drawn, 0..1: one a threshold runs
    // through fades out.
    double figureShown(double db) const;
    // The Focus edges as drawn (eased), Hz.
    double focusLowShown() const;
    double focusHighShown() const;

    // For tests: the latest whole frame each spectral display gave (raw), the values drawn (eased), how many
    // frames came, the meters' levels.
    const std::array<float, kPoints>& latestGain() const { return gain_.latest; }
    const std::array<float, kPoints>& latestInput() const { return input_.latest; }
    const std::array<float, kPoints>& latestKey() const { return key_.latest; }
    const std::array<float, kPoints>& latestOutput() const { return output_.latest; }
    const std::vector<double>& shownGain() const { return shownGain_; }
    const std::vector<double>& shownInput() const { return shownInput_; }
    const std::vector<double>& shownOutput() const { return shownOutput_; }
    qint64 framesSeen() const { return gain_.frames; }
    double levelIn() const { return meterIn_.level; }
    double levelOut() const { return meterOut_.level; }
    double deltaShare() const { return deltaShown_.value; }  // how far the output line is tinted for Delta (0..1)
    double hotShare() const { return hotShown_.value; }      // how bright the glow over the threshold is (0..1)
    bool cutHeld() const { return anyHeld_; }                // the line of the recent deepest cut is drawn
    bool focusDimmed() const { return focusDimmed_; }        // the Focus band's dim is drawn

Q_SIGNALS:
    void keyedChanged();
    void hoverChanged();
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

private:
    // A spectral display's frames put together again: the engine writes 128 values a frame, the first at an
    // absolute index that is a multiple of 128, so a value's band is its index modulo 128 and a frame is whole
    // when its 128 bands came in order (a gap, a stalled reader skipping values, waits for the next frame).
    struct FrameAssembler {
        enum class Merge { Highest, Gain };  // frames since the last tick: the highest; a gain's deepest cut
        Merge merge = Merge::Highest;
        std::array<float, kPoints> building{};
        std::array<float, kPoints> latest{};  // the latest whole frame
        std::array<float, kPoints> merged{};  // every whole frame since the last take(), merged
        qint64 next = -1;  // the index expected next
        int filled = -1;   // bands of `building` filled in order (-1: waiting for a frame's start)
        qint64 frames = 0;
        bool fresh = false;  // a whole frame came since the last take()

        void add(qint64 first, const std::vector<float>& values);
        void take() { fresh = false; }
    };

    bool belowShown() const { return upward_ > 1.001; }
    // Which handle is at `pos` (Handle), as the mouse finds them.
    int hit(const QPointF& pos) const;
    double edgeX(int which) const;  // a Focus edge's line (its grip's x for an open edge)
    double lineY(double db) const;  // as yOfLevel(), not held to the plot (for the lines, which the plot clips)
    QPointF onLine(const QLineF& line, double hz) const;  // a handle's place on `line` (see handle())
    static QString levelFigure(double db);                // "0", "−24"...
    void setHovered(int which);
    void updateReadout();
    void updateLines();
    void dragTo(const QPointF& pos, Qt::KeyboardModifiers modifiers);
    QString paramOf(int which) const;

    // A parameter's range (the engine's, read once the device is there; none until then).
    struct Span {
        double low = -std::numeric_limits<double>::infinity(), high = std::numeric_limits<double>::infinity();
        double clamp(double value) const { return value < low ? low : (value > high ? high : value); }
    };
    void readSpans();

    // The parameters (as they are now).
    double threshold_ = -18.0, ratio_ = 2.0, below_ = -48.0, upward_ = 1.0, tilt_ = 0.0;
    double range_ = 24.0, focusLow_ = 20.0, focusHigh_ = 20000.0, mix_ = 100.0;
    Span thresholdSpan_, belowSpan_, tiltSpan_, rangeSpan_;
    bool spansRead_ = false;
    bool delta_ = false;
    bool keyed_ = false;
    bool active_ = true;  // anything is processed (else the threshold is drawn dim)
    bool primed_ = false;  // synced once: what is drawn starts where it is, not easing in

    // What is drawn, easing towards the parameters: dB; the Focus edges in log2 Hz; 0..1.
    Eased thresholdShown_, belowShown_, tiltShown_, focusLowShown_, focusHighShown_;
    Eased belowOpacity_, deltaShown_;
    Eased hotShown_;  // the glow over the threshold: 1 while anything above it is turned down
    std::array<Eased, 7> handleGrow_{};  // per Handle: 0 at rest, 1 hovered or dragged
    Eased readoutOpacity_;
    QString readoutText_;
    std::vector<double> thresholdCurve_;  // dB at each display point, as drawn
    QList<double> frequencies_;           // the display points (Hz)
    QList<double> focusFrequencies_;      // kFocusStops, evenly across the plot (Hz)
    QList<double> focusWeights_;          // the Focus band's weight there, from the edges as drawn
    bool focusDimmed_ = false;            // some weight there darkens the plot (the dim is drawn)

    // The displays.
    FrameAssembler input_, key_, output_, gain_;
    std::vector<double> targetInput_, targetKey_, targetOutput_, targetGain_;
    std::vector<double> shownInput_, shownKey_, shownOutput_, shownGain_;
    std::vector<double> heldCut_, heldFor_;  // the deepest recent cut per point (dB, ≥ 0), and how long it held
    bool anyHeld_ = false;                   // any point's held cut is deep enough to draw
    Eased maxCut_, maxLift_;
    MeterBallistics meterIn_, meterOut_;
    double meterDt_ = 0.0;     // seconds since the meters last had values
    double sinceFrame_ = 1.0;  // seconds since a whole frame came
    double sinceLevel_ = 1.0;

    // The mouse.
    int hovered_ = None;
    int dragged_ = None;
    QString gesture_;  // the drag's merge key ("": none)
    QPointF lastPos_;  // where the mouse was at the last move (each move adds its own distance, Shift or not)
    double dragValue_ = 0.0;  // the dragged parameter, unrounded (from the press: Below as drawn)
};

}  // namespace sub::ui
