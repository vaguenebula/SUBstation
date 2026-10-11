#include "devices/AmpDriveGraph.h"

#include "audio/AmpResponse.h"
#include "audio/EngineBridge.h"
#include "devices/AmpDisplays.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QFontMetricsF>
#include <QList>

#include <algorithm>
#include <cmath>

namespace sub::ui {

namespace {

constexpr double kFallDbPerSecond = 18.0;  // the dots fall back this fast
constexpr double kSagSeconds = 0.06;
constexpr double kSagStepDb = 0.05;  // the curve is made again when the sag as drawn moves this far

double amplitude(double db) { return std::min(1.0, std::pow(10.0, db / 20.0)); }

constexpr double kTextRow = 12.0;  // a text's row: a line of its font centred in it

// The baseline of a line of `metrics`' font centred in a text row from `top`, on whole pixels: where
// SgPainter::drawText(QRectF, Qt::AlignVCenter) puts it (the line's top rounded, its ascent rounded).
double baselineIn(double top, const QFontMetricsF& metrics) {
    return std::round(top + (kTextRow - metrics.height()) / 2.0) + std::round(metrics.ascent());
}

// Where `text`'s glyphs land with its pen at `pen`: their tight box, out to whole pixels.
QRectF glyphBox(const QString& text, const QPointF& pen) {
    return QRectF(QFontMetricsF(AmpDriveGraph::textFont()).tightBoundingRect(text).translated(pen).toAlignedRect());
}

QString captionText() { return QStringLiteral("Drive"); }

}  // namespace

AmpDriveGraph::AmpDriveGraph(QQuickItem* parent) : DeviceCanvas(parent), floorDb_(ampDisplays::floorDb()) {
    setImplicitSize(kWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::NoButton);
    inputRead_ = floorDb_;
    input_.reset(floorDb_);
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

QFont AmpDriveGraph::textFont() { return uiFont(7); }

QString AmpDriveGraph::peakText(double db, double floorDb) {
    return db <= floorDb + 0.5
               ? QStringLiteral("−∞")
               : pythonFixed(db, 0).replace(QLatin1Char('-'), QChar(0x2212)) + QStringLiteral(" dB");
}

QPointF AmpDriveGraph::captionPen() const {
    const QRectF r = plot();
    return QPointF(std::round(r.left() + 4), baselineIn(r.top() + 2, QFontMetricsF(textFont())));
}

QPointF AmpDriveGraph::peakPen(const QString& peak) const {
    const QRectF r = plot();
    const QFontMetricsF metrics(textFont());
    return QPointF(std::round(r.right() - 4 - metrics.horizontalAdvance(peak)), baselineIn(r.bottom() - 14, metrics));
}

QRectF AmpDriveGraph::captionBox() const { return glyphBox(captionText(), captionPen()); }

QRectF AmpDriveGraph::peakBox(const QString& peak) const { return glyphBox(peak, peakPen(peak)); }

QList<AmpDriveGraph::GridLine> AmpDriveGraph::grid(const QString& peak) const {
    const QRectF r = plot();
    // The texts' boxes and a pixel round them, which no line lights: one whose pixels across (half a pixel
    // either side of its middle, out to whole pixels) reach into the caption's (top left) starts past it, one
    // reaching into the peak's (bottom right) ends before it, its end's feather (the pixel past it, half lit) too.
    const QRectF top = captionBox().adjusted(-1, -1, 1, 1), bottom = peakBox(peak).adjusted(-1, -1, 1, 1);
    const auto reaches = [](double at, double from, double to) {
        return std::floor(at - 0.5) < to && std::ceil(at + 0.5) > from;
    };
    QList<GridLine> lines;
    const auto upright = [&](double x, bool axis) {
        const double from =
            reaches(x, top.left(), top.right()) ? std::max(r.top() + 2, top.bottom() + 1) : r.top() + 2;
        const double to =
            reaches(x, bottom.left(), bottom.right()) ? std::min(r.bottom() - 2, bottom.top() - 1) : r.bottom() - 2;
        if (to > from)
            lines.append({QLineF(x, from, x, to), axis});
    };
    const auto level = [&](double y, bool axis) {
        const double from =
            reaches(y, top.top(), top.bottom()) ? std::max(r.left() + 2, top.right() + 1) : r.left() + 2;
        const double to =
            reaches(y, bottom.top(), bottom.bottom()) ? std::min(r.right() - 2, bottom.left() - 1) : r.right() - 2;
        if (to > from)
            lines.append({QLineF(from, y, to, y), axis});
    };
    // Half scale either way, then the axes through the middle.
    for (const double v : {-0.5, 0.5}) {
        upright(xOf(v), false);
        level(yOf(v * range_), false);
    }
    upright(r.center().x(), true);
    level(r.center().y(), true);
    return lines;
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
    model_ = std::clamp(static_cast<int>(std::lround(value(QStringLiteral("type")))), 0, sub::app::ampModelCount() - 1);
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
    // Made again only when what it is made from changes: sync() comes at every playhead
    // move while any of the device's parameters follows automation (Dry/Wet's too), and
    // making it is milliseconds of work.
    const double rate = sampleRate();
    const int columns = std::max(2, int(plot().width()));
    const CurveKey key{model_, columns, rate, {gain_, bass_, middle_, treble_, presence_, volume_}};
    if (key == made_)
        return;
    made_ = key;
    // The preamp's part for a column's worth of tones (the slow part), then the curve for the sag as drawn.
    // Each x and -x are exact opposites (whole numbers over the columns), so a tone's two ends share its work.
    QList<double> xs;
    xs.reserve(columns + 1);
    for (int i = 0; i <= columns; ++i) xs.append(double(2 * i - columns) / columns);
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
    const double dt = tickSeconds();
    const double recent = ampDisplays::recentSeconds(dt);
    bool read = ampDisplays::loudest(readRecent(QStringLiteral("input"), recent), floorDb_, inputRead_);
    read = ampDisplays::latest(readRecent(QStringLiteral("sag"), recent), sagRead_) || read;
    if (read) {
        lastRead_.restart();
    } else if (!lastRead_.isValid() || lastRead_.elapsed() > kQuietSeconds * 1000.0) {
        inputRead_ = floorDb_;
        sagRead_ = 0.0;
    }

    const double level = input_.level;
    input_.update(inputRead_, dt, kFallDbPerSecond, 0.0, floorDb_);
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
    p.fillRoundedRect(QRectF(0, 0, width(), height()), 4, 4, Theme::meterBg());
    // The grid: half scale either way and the axes through the middle, short of the texts.
    const QString peak = peakText();
    for (const GridLine& line : grid(peak))
        p.drawLine(line.line.p1(), line.line.p2(), withAlpha(line.axis ? Theme::gridBeat() : Theme::gridSub(), 200));

    p.save();
    p.setClipRect(r);
    // The clean gain carried on: the bend away from it is the distortion.
    const double reach = range_ / std::max(std::abs(slope_), 1e-9);  // the input where it leaves the plot
    p.drawLine(QPointF(xOf(-reach), yOf(-range_)), QPointF(xOf(reach), yOf(range_)), withAlpha(Theme::gridBar(), 150));

    std::vector<QPointF> points;
    points.reserve(curve_.size());
    for (const QPointF& v : curve_) points.emplace_back(xOf(v.x()), yOf(v.y()));
    if (points.size() >= 2)
        p.drawPolyline(points.data(), int(points.size()), withAlpha(Theme::accent(), 170), 1.5);

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
        drawGlowPolyline(p, used, withAlpha(Theme::accent().lighter(120), int(255 * shown)), 2.0);
        for (int k = kTrail - 1; k >= 1; --k) {
            const double t = trail_[size_t(k)];
            if (t == a)
                continue;
            const QColor color = withAlpha(Theme::accent(), int(110.0 * (1.0 - double(k) / kTrail) * shown));
            p.fillEllipse(QPointF(xOf(t), yOf(curveAt(t))), 2.0, 2.0, color);
            p.fillEllipse(QPointF(xOf(-t), yOf(curveAt(-t))), 2.0, 2.0, color);
        }
        for (const double x : {-a, a}) {
            const QPointF at(xOf(x), yOf(curveAt(x)));
            p.fillEllipse(at, 4.5, 4.5, withAlpha(Theme::accent(), int(255 * shown)));
            p.fillEllipse(at, 2.5, 2.5, withAlpha(Qt::white, int(255 * shown)));
        }
    }
    p.restore();

    const QFont font = textFont();
    p.drawText(captionPen(), captionText(), Theme::textDim(), font);
    p.drawText(peakPen(peak), peak, Theme::text(), font);
}

}  // namespace sub::ui
