#pragma once

// The Limiter's display: the level over the last kHistorySeconds (1.5 s), the
// input (grey, red where it goes over the line) with the output inside it
// (light), and the gain reduction hanging from the top (orange; in Soft Clip
// the knee's share stacked under it, lighter); beside it In, GR and Out
// meters (two bars each, with ballistics and a held peak); across both, the
// line: the Ceiling, or with Maximize the Threshold. Drag the line up and down
// to set it (one undo step per drag, Shift finely), double-click it for its
// default. In Soft Clip a band shows where the knee rounds off (from 6 dB under
// the line to 3.5 dB over it, where it reaches the line); a badge names the
// mode (and MAX while Maximize is on).
//
// The history is drawn in the line's domain, where the device works (the
// application layer's LimiterResponse.h, from the engine's own maths): the
// input as the device hears it (after Gain; with Maximize the raw input), the
// output moved by what Maximize adds (Output - Threshold), so the loudest output
// meets the line. The In meter is the history's input; the Out meter and its
// figure are the output as it comes out, in dBFS, as the axis reads (without
// Maximize the two are the same). The device's seven displays (in_l, in_r,
// out_l, out_r, gr_a, gr_b, clip: one value per 128 samples, in step) are read
// by absolute index into rings, and the history is drawn from fixed bins of
// them (a column each), scrolling smoothly at a steady speed whatever the
// bursts they arrive in: the cursor (the index at the plot's right edge) moves
// on by the time since the last tick and eases towards the newest value,
// kLagSeconds behind it. The meters read what the cursor passes, so they move
// with the history's edge.
//
// Everything moves in advance() (from refreshDisplays(), about 60 times a
// second, with the time since the last tick): the cursor, the meters'
// ballistics, and the eased line, Soft Clip's band, the badges, the line's
// glow (it warms with the gain reduction) and the hover. It repaints only while
// something moves or changed: nothing when idle or silent.

#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <utility>
#include <vector>

namespace sub::ui {

class LimiterGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double lineDb READ lineDb NOTIFY lineChanged)          // the ceiling, or the threshold with Maximize
    Q_PROPERTY(bool maximize READ maximize NOTIFY lineChanged)
    Q_PROPERTY(double reduction READ reduction NOTIFY levelsChanged)  // dB, the latest tick's most (both channels)
    Q_PROPERTY(double clipping READ clipping NOTIFY levelsChanged)    // dB Soft Clip's knee took off, likewise
    Q_PROPERTY(double levelIn READ levelIn NOTIFY levelsChanged)      // dB in the line's domain, likewise
    Q_PROPERTY(double levelOut READ levelOut NOTIFY levelsChanged)    // dBFS, likewise
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)    // the line is held

public:
    static constexpr int kWidth = 340;
    static constexpr int kMinimumHeight = 120;
    static constexpr double kTopDb = 12.0;  // the level axis (fixed)
    static constexpr double kFloorDb = -36.0;
    // The gain reduction's axis, down from the plot's top to its bottom: twice the level's scale, so
    // 6, 12 and 18 dB fall on the level's grid lines (0, -12, -24).
    static constexpr double kGrRangeDb = 24.0;
    static constexpr double kHistorySeconds = 1.5;
    static constexpr double kLineGrab = 5.0;     // px either side of the line
    static constexpr double kLagSeconds = 0.04;  // the scroll stays this far behind the newest value
    static constexpr int kRing = 4096;           // display values kept per stream (1.5 s at 192 kHz is 2250)
    static constexpr int kStreams = 7;           // in_l, in_r, out_l, out_r, gr_a, gr_b, clip

    explicit LimiterGraph(QQuickItem* parent = nullptr);

    double lineDb() const { return lineDb_; }
    bool maximize() const { return maximize_; }
    double reduction() const { return reduction_; }
    double clipping() const { return clipping_; }
    double levelIn() const { return levelIn_; }
    double levelOut() const { return levelOut_; }
    bool dragging() const { return !gesture_.isEmpty(); }

    // Graph coordinates: the history's plot, the level axis on it (+12 dB at the top, -36 at the
    // bottom; the In and Out meters share it), and the gain reduction's (down from the top, 24 dB at
    // the bottom; the GR meters share it).
    Q_INVOKABLE QRectF plot() const;
    Q_INVOKABLE double yOf(double db) const;
    double grY(double reductionDb) const;
    // Where the line is drawn now (eased towards the parameter; on it while dragged), in px and dB.
    Q_INVOKABLE double lineY() const { return yOf(line_.value); }
    Q_INVOKABLE double shownLineDb() const { return line_.value; }
    // Soft Clip's band and the mode badge (0..1, eased), the line's glow (0..1: with the gain reduction).
    Q_INVOKABLE double softBand() const { return soft_.value; }
    double badge() const { return badge_.value; }
    double glow() const { return glow_.value; }
    double hover() const { return hover_.value; }  // the line lit by the mouse over it (0..1, eased)
    // The band's edges relative to the line (dB): where the knee starts and where it reaches the line.
    double softKneeDb() const { return softKneeDb_; }
    double softTopDb() const { return softTopDb_; }
    // The absolute display index drawn at the plot's right edge, and one past the newest value read.
    Q_INVOKABLE double historyCursor() const { return cursor_; }
    Q_INVOKABLE qint64 valuesRead() const { return end_; }
    // The meters' readings (dB on the level axis: In in the line's domain, Out in dBFS; GR and the
    // knee's share positive). Channel 0 is L (or M), 1 is R (or S).
    const MeterBallistics& meterIn(int c) const { return meterIn_[std::size_t(c & 1)]; }
    const MeterBallistics& meterOut(int c) const { return meterOut_[std::size_t(c & 1)]; }
    const MeterBallistics& meterGr(int c) const { return meterGr_[std::size_t(c & 1)]; }
    const MeterBallistics& meterClip() const { return meterClip_; }
    // The figures it shows: the footer's gain reduction over the last half second ("GR −3.2 dB"), then
    // under the meters the In, GR and Out peaks over the last second (In in the line's domain, GR as the
    // footer's, Out in dBFS). The gain reduction includes Soft Clip's share, as the GR bars do.
    Q_INVOKABLE QStringList figures() const { return {grText_, inText_, grPeakText_, outText_}; }
    // How many times it asked to be painted (the tests check it rests when nothing moves).
    int updates() const { return updates_; }

    // One tick of animation, `seconds` after the last (refreshDisplays calls it with the real time).
    Q_INVOKABLE void advance(double seconds);

Q_SIGNALS:
    void lineChanged();
    void levelsChanged();
    void draggingChanged();

protected:
    void sync() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;

private:
    enum Stream { InL, InR, OutL, OutR, GrA, GrB, Clip };

    double valuesPerSecond() const;
    double valuesPerColumn() const;
    QString lineParam() const;
    bool nearLine(const QPointF& pos) const;
    void endDrag();
    void updateHover();
    void requestPaint();
    // The most of `stream` (and its pair) over [from, to) of what is kept, or `otherwise` without any.
    float most(int stream, int pair, qint64 from, qint64 to, float otherwise) const;
    // The most of the output in dBFS over [from, to): channel 0, 1, or -1 both.
    float mostDbfs(int channel, qint64 from, qint64 to) const;
    float ring(int stream, qint64 index) const { return rings_[std::size_t(stream)][std::size_t(index & (kRing - 1))]; }
    void rebuildHistory();
    void updateTexts();
    bool updateTextsChanged();  // updateTexts(), and whether any text changed

    // The parameters, as sync() read them.
    bool maximize_ = false;
    double lineDb_ = -0.3;
    double outputShift_ = 0.0;  // dB taken off the output to draw it in the line's domain
    int mode_ = 0;              // 0 Standard, 1 Soft Clip, 2 True Peak
    int routing_ = 0;           // 0 L/R, 1 M/S
    double softKneeDb_ = -6.0, softTopDb_ = 3.5;
    bool synced_ = false;

    // The displays: rings by absolute index, [begin_, end_) valid; per stream what has come but not
    // yet in every stream (so they stay in step).
    std::array<std::vector<float>, kStreams> rings_;
    std::array<std::vector<float>, 2> outDbfs_;  // the output's peaks as they came, in dBFS: the Out meter and figure
    std::array<std::vector<float>, kStreams> pending_;
    std::array<qint64, kStreams> pendingStart_{};
    qint64 begin_ = 0, end_ = 0;
    qint64 lastLoud_ = -1;  // the newest index with anything the plot shows
    double cursor_ = 0.0;
    QElapsedTimer clock_;

    // The latest tick's most.
    double reduction_ = 0.0, clipping_ = 0.0;
    double levelIn_ = -90.0, levelOut_ = -90.0;

    // The animation.
    std::array<MeterBallistics, 2> meterIn_, meterOut_, meterGr_;
    MeterBallistics meterClip_;
    Eased line_, soft_, badge_, maxBadge_, glow_, hover_, grTint_;
    QString badgeText_;
    int updates_ = 0;

    // What paint() draws of the history: a point per column (bin), oldest first.
    std::vector<QPointF> inPoints_, outPoints_, grPoints_, clipPoints_;
    std::vector<float> grTops_, clipBottoms_;
    std::vector<std::pair<int, int>> grRuns_;  // [from, to) of the points where the reduction's edge shows
    double binX0_ = 0.0;  // the first column's left edge
    bool anyClip_ = false;
    bool historyDirty_ = true;
    // The figures: the gain reduction over the last half second; the meters' peaks over the last second.
    double grFigure_ = 0.0;
    QString grText_, inText_, grPeakText_, outText_;

    // The drag.
    QString gesture_;  // its merge key ("": none)
    QString dragId_;   // "ceiling" or "threshold"
    double dragDb_ = 0.0;
    double lastY_ = 0.0;
    // The hover: where the mouse last was over the graph, and whether the resize cursor is set.
    QPointF hoverPos_;
    bool hovering_ = false;
    bool resizeCursor_ = false;
};

}  // namespace sub::ui
