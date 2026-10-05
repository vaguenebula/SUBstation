#pragma once

// The Sidechain device's curve, over the device's displays, and the fit.
//
// The curve: drag a point to move it (the first and last only up and down),
// drag between points to bend the curve there (as automation bends), click
// anywhere else to add a point (and keep dragging it), double-click a point (or
// Alt-click it, or select it and press Delete) to remove it, double-click a
// bend to straighten it; the wheel bends too, Shift makes any of them fine.
// Right-click (contextMenuRequested) for the shapes to start from, the fit, flip
// and reset. Behind the curve: the kick's envelope where it clashes (orange),
// and the curve the fit calls for (dashed); as hits come, a playhead rides the
// curve; at the right, the key's level against the threshold. The hint over the
// curve (no sidechain while triggered by it) asks for the sidechain menu
// (sidechainMenuRequested: the device's frame shows it).
//
// The displays "key", "input" and "phase" are read together as the meters
// update (DeviceCanvas::readDisplayAt: by absolute index) into a
// sidechainFit::Capture; each hit's kick, once it has come, is analysed (over
// the latest kKeepKicks) into `fit`, which ClashView draws too. Fit writes its
// points, length and crossover in one undo step; Auto fits again at every hit
// (one step while nothing else is done). The curve's points are always written
// whole (pointsValues: every slot's four parameters) through
// ProjectEditor::setDeviceParams, so a drag (adding a point and dragging it on,
// too) is one undo step.

#include "devices/DeviceCanvas.h"

#include "analysis/SidechainFit.h"

#include <QElapsedTimer>
#include <QPointF>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <optional>
#include <vector>

namespace sub::ui {

class CurveGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool hasFit READ hasFit NOTIFY fitChanged)
    Q_PROPERTY(QString hint READ hint NOTIFY curveChanged)  // over the curve: how to get going ("": none)
    Q_PROPERTY(QString lengthText READ lengthText NOTIFY curveChanged)
    Q_PROPERTY(int selected READ selected NOTIFY curveChanged)  // the selected point (-1: none)
    Q_PROPERTY(int hitCount READ hitCount NOTIFY ticked)
    Q_PROPERTY(QStringList shapes READ shapes CONSTANT)

public:
    static constexpr int kPoints = 16;
    static constexpr int kKeepKicks = 3;  // the fit averages this many of the latest hits
    static constexpr int kMinimumWidth = 380;
    static constexpr int kMinimumHeight = 100;
    static constexpr double kPointRadius = 4.5, kPointHoverRadius = 6.5;
    static constexpr double kHitRadius = 9.0;
    static constexpr double kCurveHit = 7.0;  // px from the curve where a drag bends it
    static constexpr double kMeterWidth = 6.0;
    static constexpr double kMeterFloor = -60.0;
    static constexpr int kTrail = 14;  // the playhead's trail, in refreshes

    using Point = sub::app::sidechainFit::FitPoint;  // (x 0..1, y: 1 untouched, 0 ducked by the depth, curve)

    explicit CurveGraph(QQuickItem* parent = nullptr);

    // A curve point's parameter id (points from 0): "p1_x".
    static QString pointParam(int index, const QString& name);
    // The curve at x (0..1), as the device plays it.
    static double curveValue(const std::vector<Point>& points, double x);
    // The parameters for a curve of these points (every slot: the same parameters every time).
    static sub::app::OrderedMap<QString, double> pointsValues(const std::vector<Point>& points);
    // 1500 ms -> "1.50 s", 250 -> "250 ms".
    static QString formatMs(double ms);
    // Shapes to start from: (name, points).
    static const std::vector<std::pair<QString, std::vector<Point>>>& shapeList();

    bool hasFit() const { return fit_.has_value(); }
    const std::optional<sub::app::sidechainFit::Fit>& fit() const { return fit_; }
    QString hint() const { return hint_; }
    QString lengthText() const { return lengthText_; }
    int selected() const { return selected_; }
    int hitCount() const { return capture_.hitCount(); }
    QStringList shapes() const;
    const sub::app::sidechainFit::Capture& capture() const { return capture_; }
    const std::vector<double>& trail() const { return trail_; }
    double flash() const { return flash_; }

    // The curve's points, in order of x.
    const std::vector<Point>& points() const { return points_; }
    void setPoints(const std::vector<Point>& points, const QString& mergeKey = QString(),
                   const QString& text = QStringLiteral("Change Sidechain Curve"),
                   const sub::app::OrderedMap<QString, double>& extra = {});
    double lengthMs() const { return lengthMs_; }

    // Geometry, for the tests too.
    QRectF plot() const;
    QPointF toScreen(double x, double y) const;
    QPointF fromScreen(const QPointF& pos) const;
    // ("point" | "segment", index); kind "" for nothing.
    std::pair<QString, int> hit(const QPointF& pos) const;

    Q_INVOKABLE void removePoint(int index);
    Q_INVOKABLE void applyShape(int index);
    Q_INVOKABLE void flip();
    Q_INVOKABLE void resetCurve();
    // Fit: the curve, its length and the crossover from the latest fit (one undo step).
    Q_INVOKABLE void fitNow();
    Q_INVOKABLE void setAuto(bool on);
    Q_INVOKABLE void setCharacter(int index);
    // The parameters a fit sets: the curve, its length (in ms) and the crossover above the clash.
    sub::app::OrderedMap<QString, double> fitValues(const sub::app::sidechainFit::Fit& fit) const;
    // A display refresh: the playhead (where on the curve, 0..1; none: not playing one), the key's
    // peak since the last, and whether a hit came.
    void tick(std::optional<double> phase, double keyPeak, bool hit);
    // Reads the displays (as the meters update; the tests call it after rendering).
    Q_INVOKABLE void readDisplays() { refreshDisplays(); }

Q_SIGNALS:
    void fitChanged();
    void curveChanged();
    void ticked();
    void contextMenuRequested(QPointF position, int point);  // point: an inner one under it (-1: none)
    void sidechainMenuRequested(QPointF position);

protected:
    void sync() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;
    bool event(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct Drag {
        QString kind;  // "point" or "bend"
        int index = 0;
        QPointF origin;
        std::vector<Point> points;  // as they were at the press
        QString gesture;
        QString text = QStringLiteral("Change Sidechain Curve");
    };

    void set(const sub::app::OrderedMap<QString, double>& values, const QString& mergeKey, const QString& text);
    void analyze();
    void applyAuto();
    QRectF hintRect() const;
    QString readoutText(const Point& point) const;
    void hoverAt(const QPointF& pos);
    void endDrag();

    // The parameters, as they are now.
    std::vector<Point> points_;
    double depth_ = 1.0;
    double threshold_ = -24.0;
    double lengthMs_ = 250.0;
    QString lengthText_;
    QString hint_;
    // The fit.
    sub::app::sidechainFit::Capture capture_;
    std::vector<std::vector<double>> kicks_;
    std::vector<double> bass_;
    std::optional<sub::app::sidechainFit::Fit> fit_;
    int hitsSeen_ = 0;
    QString autoGesture_;
    // Live.
    std::vector<double> trail_;  // the playhead's latest positions (x 0..1), oldest first
    double level_ = kMeterFloor;  // the key's level (dB), falling back slowly
    double flash_ = 0.0;          // a hit lights the meter up
    // Interaction.
    std::pair<QString, int> hover_{QString(), -1};
    int selected_ = -1;
    std::optional<Drag> drag_;
    std::optional<std::pair<QPointF, QString>> readout_;  // shown while dragging
    QElapsedTimer addedClock_;
    int added_ = -1;  // the point a click last added: its double-click doesn't remove it
    QElapsedTimer wheelClock_;
    QString wheelGesture_;
};

}  // namespace sub::ui
