#include "devices/AmpDriveGraph.h"

#include "audio/AmpResponse.h"
#include "audio/EngineBridge.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QList>

#include <algorithm>
#include <cmath>

namespace sub::ui {

namespace {

constexpr double kFallDbPerSecond = 18.0;  // the dots fall back this fast
constexpr double kSagSeconds = 0.06;
constexpr double kSagStepDb = 0.05;  // the curve is made again when the sag as drawn moves this far

// The loudest of a display's last `keep` values (dB, floored), or nothing. Only the
// last: a read can hand over a long backlog (what came while the editor wasn't
// showing), which is history, not the level now.
bool loudest(const std::vector<float>& values, size_t keep, double& into) {
    if (values.empty())
        return false;
    double most = AmpDriveGraph::kFloorDb;
    for (size_t i = values.size() - std::min(keep, values.size()); i < values.size(); ++i) {
        if (std::isfinite(values[i]))
            most = std::max(most, double(values[i]));
    }
    into = most;
    return true;
}

bool latest(const std::vector<float>& values, double& into) {
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
        if (std::isfinite(*it)) {
            into = *it;
            return true;
        }
    }
    return false;
}

double amplitude(double db) { return std::min(1.0, std::pow(10.0, db / 20.0)); }

}  // namespace

AmpDriveGraph::AmpDriveGraph(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(kWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::NoButton);
    input_.reset(kFloorDb);
    sag_.snap(0.0);
    // The curve is the engine's at its sample rate: made again when the audio device changes.
    connect(this, &DeviceCanvas::deviceChanged, this, [this] {
        disconnect(bridgeConnection_);
        if (session())
            bridgeConnection_ = connect(session()->bridge(), &sub::app::EngineBridge::deviceChanged, this, [this] {
                if (device() != nullptr)
                    updateCurve();
                update();
            });
    });
}

QRectF AmpDriveGraph::plot() const { return QRectF(0, 0, width(), height()).adjusted(1, 1, -1, -1); }

double AmpDriveGraph::xOf(double v) const {
    const QRectF r = plot();
    return r.center().x() + v * r.width() / 2.0;
}

double AmpDriveGraph::yOf(double v) const {
    const QRectF r = plot();
    return r.center().y() - v / range_ * r.height() / 2.0;
}

double AmpDriveGraph::curveAt(double x) const {
    if (curve_.size() < 2)
        return 0.0;
    const double at = std::clamp((x + 1.0) / 2.0, 0.0, 1.0) * double(curve_.size() - 1);
    const auto i = std::min(curve_.size() - 2, static_cast<size_t>(at));
    const double t = at - double(i);
    return curve_[i].y() + (curve_[i + 1].y() - curve_[i].y()) * t;
}

QPointF AmpDriveGraph::dot() const {
    const double a = amplitude(input_.level);
    return QPointF(xOf(a), yOf(curveAt(a)));
}

size_t AmpDriveGraph::recentValues(double dt) const {
    return size_t(std::ceil(std::max(kRecentSeconds, dt) * sampleRate() / kSamplesPerValue));
}

double AmpDriveGraph::outputAt(double x) const {
    return sub::app::ampTransfer(model_, gain_, bass_, middle_, treble_, presence_, volume_, curveSag_, sampleRate(),
                                 {x})
        .value(0);
}

void AmpDriveGraph::sync() {
    if (device() == nullptr) {  // (not yet, as it is being made: nothing to draw)
        update();
        return;
    }
    model_ = std::clamp(static_cast<int>(std::lround(value(QStringLiteral("type")))), 0, 6);
    gain_ = value(QStringLiteral("gain"));
    bass_ = value(QStringLiteral("bass"));
    middle_ = value(QStringLiteral("middle"));
    treble_ = value(QStringLiteral("treble"));
    presence_ = value(QStringLiteral("presence"));
    volume_ = value(QStringLiteral("volume"));
    updateCurve();
    update();
}

void AmpDriveGraph::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size() && device() != nullptr)
        updateCurve();
}

void AmpDriveGraph::updateCurve() {
    // The preamp's part for a column's worth of tones (the slow part), then the curve for the sag as drawn.
    const double rate = sampleRate();
    const int columns = std::max(2, int(plot().width()));
    QList<double> xs;
    xs.reserve(columns + 1);
    for (int i = 0; i <= columns; ++i) xs.append(-1.0 + 2.0 * double(i) / columns);
    transfer_.prepare(model_, gain_, bass_, middle_, treble_, presence_, volume_, rate, xs);
    xs_ = xs;
    // Up is scaled to the curve's reach without sag (it rises with the input: its ends), so the
    // sag's breathing shows.
    const QList<double> ends =
        sub::app::ampTransfer(model_, gain_, bass_, middle_, treble_, presence_, volume_, 0.0, rate, {-1.0, 1.0});
    const double reach = std::max(std::abs(ends.value(0)), std::abs(ends.value(1)));
    range_ = reach > 1e-9 ? reach / kReach : 1.0;
    shapeCurve();
}

void AmpDriveGraph::shapeCurve() {
    curveSag_ = sag_.value;
    const QList<double> ys = transfer_.at(curveSag_);
    curve_.resize(size_t(xs_.size()));
    for (qsizetype i = 0; i < xs_.size() && i < ys.size(); ++i)
        curve_[size_t(i)] = QPointF(xs_[i], ys[i]);
    slope_ = transfer_.smallSignalGain(curveSag_);  // the clean gain
    Q_EMIT curveChanged();
}

void AmpDriveGraph::refreshDisplays() {
    const double dt = clock_.isValid() ? std::clamp(clock_.restart() / 1000.0, 0.0, 0.1) : 1.0 / 60.0;
    if (!clock_.isValid())
        clock_.start();
    bool read = loudest(readDisplay(QStringLiteral("input")), recentValues(dt), inputRead_);
    read = latest(readDisplay(QStringLiteral("sag")), sagRead_) || read;
    if (read) {
        lastRead_.restart();
    } else if (!lastRead_.isValid() || lastRead_.elapsed() > kQuietSeconds * 1000.0) {
        inputRead_ = kFloorDb;
        sagRead_ = 0.0;
    }

    const double level = input_.level;
    input_.update(inputRead_, dt, kFallDbPerSecond, 0.0, kFloorDb);
    bool moving = input_.level != level;
    sag_.target = std::max(0.0, sagRead_);
    moving = sag_.step(easeFraction(dt, kSagSeconds), 1e-3) || moving;
    if (!transfer_.isEmpty() &&
        (std::abs(sag_.value - curveSag_) >= kSagStepDb || (sag_.value == sag_.target && sag_.value != curveSag_))) {
        shapeCurve();  // (the power stage's part alone)
        moving = true;
    }
    // The trail: where the dots were over the last ticks.
    const double a = amplitude(input_.level);
    const bool settled = std::all_of(trail_.begin(), trail_.end(), [a](double t) { return t == a; });
    std::move_backward(trail_.begin(), trail_.end() - 1, trail_.end());
    trail_[0] = a;
    if (moving || !settled) {
        Q_EMIT levelsChanged();
        update();
    }
}

// --- Painting ---------------------------------------------------------------------------

void AmpDriveGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF r = plot();
    p.fillRoundedRect(QRectF(0, 0, width(), height()), 4, 4, Theme::kMeterBg);
    // The grid: the axes through the middle, half scale either way.
    for (const double v : {-0.5, 0.5}) {
        p.drawLine(QPointF(xOf(v), r.top() + 2), QPointF(xOf(v), r.bottom() - 2), withAlpha(Theme::kGridSub, 200));
        p.drawLine(QPointF(r.left() + 2, yOf(v * range_)), QPointF(r.right() - 2, yOf(v * range_)),
                   withAlpha(Theme::kGridSub, 200));
    }
    p.drawLine(QPointF(r.center().x(), r.top() + 2), QPointF(r.center().x(), r.bottom() - 2),
               withAlpha(Theme::kGridBeat, 200));
    p.drawLine(QPointF(r.left() + 2, r.center().y()), QPointF(r.right() - 2, r.center().y()),
               withAlpha(Theme::kGridBeat, 200));

    p.save();
    p.setClipRect(r);
    // The clean gain carried on: the bend away from it is the distortion.
    const double reach = range_ / std::max(std::abs(slope_), 1e-9);  // the input where it leaves the plot
    p.drawLine(QPointF(xOf(-reach), yOf(-range_)), QPointF(xOf(reach), yOf(range_)), withAlpha(Theme::kGridBar, 150));

    std::vector<QPointF> points;
    points.reserve(curve_.size());
    for (const QPointF& v : curve_) points.emplace_back(xOf(v.x()), yOf(v.y()));
    if (points.size() >= 2)
        p.drawPolyline(points.data(), int(points.size()), withAlpha(Theme::kAccent, 170), 1.5);

    // The part the signal uses glows; the dots ride its ends, the trail behind them.
    const double a = amplitude(input_.level);
    const double shown = std::clamp((input_.level + 72.0) / 18.0, 0.0, 1.0);  // fading out towards silence
    if (shown > 0.0 && points.size() >= 2) {
        std::vector<QPointF> used;
        used.push_back(QPointF(xOf(-a), yOf(curveAt(-a))));
        for (const QPointF& v : curve_) {
            if (v.x() > -a && v.x() < a)
                used.emplace_back(xOf(v.x()), yOf(v.y()));
        }
        used.push_back(QPointF(xOf(a), yOf(curveAt(a))));
        drawGlowPolyline(p, used, withAlpha(Theme::kAccent.lighter(120), int(255 * shown)), 2.0);
        for (int k = kTrail - 1; k >= 1; --k) {
            const double t = trail_[size_t(k)];
            if (t == a)
                continue;
            const QColor color = withAlpha(Theme::kAccent, int(110.0 * (1.0 - double(k) / kTrail) * shown));
            p.fillEllipse(QPointF(xOf(t), yOf(curveAt(t))), 2.0, 2.0, color);
            p.fillEllipse(QPointF(xOf(-t), yOf(curveAt(-t))), 2.0, 2.0, color);
        }
        for (const double x : {-a, a}) {
            const QPointF at(xOf(x), yOf(curveAt(x)));
            p.fillEllipse(at, 4.5, 4.5, withAlpha(Theme::kAccent, int(255 * shown)));
            p.fillEllipse(at, 2.5, 2.5, withAlpha(Qt::white, int(255 * shown)));
        }
    }
    p.restore();

    const QFont font = uiFont(7);
    p.drawText(QRectF(r.left() + 4, r.top() + 2, 60, 12), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Drive"),
               Theme::kTextDim, font);
    const QString peak = input_.level <= kFloorDb + 0.5
                             ? QStringLiteral("−∞")
                             : pythonFixed(input_.level, 0).replace(QLatin1Char('-'), QChar(0x2212)) +
                                   QStringLiteral(" dB");
    p.drawText(QRectF(r.right() - 64, r.bottom() - 14, 60, 12), Qt::AlignRight | Qt::AlignVCenter, peak, Theme::kText,
               font);
}

}  // namespace sub::ui
