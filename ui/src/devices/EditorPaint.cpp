#include "devices/EditorPaint.h"

#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <algorithm>
#include <cmath>

namespace sub::ui {

QColor withAlpha(const QColor& color, int alpha) {
    QColor result(color);
    result.setAlpha(std::clamp(alpha, 0, 255));
    return result;
}

QColor mixColor(const QColor& a, const QColor& b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    const auto mix = [t](float x, float y) { return float(x + (y - x) * t); };
    return QColor::fromRgbF(mix(a.redF(), b.redF()), mix(a.greenF(), b.greenF()), mix(a.blueF(), b.blueF()),
                            mix(a.alphaF(), b.alphaF()));
}

QString pythonFixed(double value, int decimals) { return QString::number(value, 'f', decimals); }

QString pythonSigned(double value, int decimals) {
    const QString text = QString::number(value, 'f', decimals);
    return text.startsWith(QLatin1Char('-')) ? text : QLatin1Char('+') + text;
}

QString pythonGeneral(double value) { return QString::number(value, 'g', 6); }

void drawDecadeGrid(SgPainter& p, const QRectF& plot, const LogAxis& axis) {
    for (double decade = std::pow(10.0, std::floor(std::log10(axis.low))); decade < axis.high; decade *= 10.0) {
        for (int multiple = 1; multiple < 10; ++multiple) {
            const double value = decade * multiple;
            if (axis.low < value && value < axis.high) {
                const double x = axis.position(value);
                p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()),
                           withAlpha(Theme::gridBeat(), multiple == 1 ? 200 : 90));
            }
        }
    }
}

void drawDashedPolyline(SgPainter& p, const std::vector<QPointF>& points, const QColor& color, double width) {
    const double dash = 4.0 * width, gap = 2.0 * width;
    bool on = true;
    double left = dash;  // of the current dash or gap
    std::vector<QPointF> piece;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        QPointF a = points[i];
        const QPointF b = points[i + 1];
        double length = std::hypot(b.x() - a.x(), b.y() - a.y());
        while (length > 0.0) {
            const double step = std::min(left, length);
            const QPointF c = a + (b - a) * (step / length);
            if (on) {
                if (piece.empty())
                    piece.push_back(a);
                piece.push_back(c);
            }
            left -= step;
            length -= step;
            a = c;
            if (left <= 1e-9) {
                if (on && piece.size() > 1)
                    p.drawPolyline(piece.data(), int(piece.size()), color, width, Qt::FlatCap);
                piece.clear();
                on = !on;
                left = on ? dash : gap;
            }
        }
    }
    if (on && piece.size() > 1)
        p.drawPolyline(piece.data(), int(piece.size()), color, width, Qt::FlatCap);
}

void appendCubic(std::vector<QPointF>& out, QPointF from, const QPointF& c1, const QPointF& c2, const QPointF& to,
                 int steps) {
    for (int i = 1; i <= steps; ++i) {
        const double t = double(i) / steps, u = 1.0 - t;
        out.push_back(from * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + to * (t * t * t));
    }
}

void MeterBallistics::update(double db, double dtSeconds, double fallDbPerSecond, double holdSeconds,
                             double floorDb) {
    db = std::isfinite(db) ? std::max(db, floorDb) : floorDb;
    level = db >= level ? db : std::max(db, level - fallDbPerSecond * dtSeconds);
    if (db >= peak) {
        peak = db;
        held = 0.0;
    } else {
        held += dtSeconds;
        if (held > holdSeconds)
            peak = std::max({level, floorDb, peak - fallDbPerSecond * dtSeconds});
    }
    level = std::max(level, floorDb);
}

double dbToY(double db, const QRectF& rect, double floorDb, double ceilingDb) {
    if (!std::isfinite(db))
        db = db > 0 ? ceilingDb : floorDb;
    const double fraction = (std::clamp(db, floorDb, ceilingDb) - floorDb) / (ceilingDb - floorDb);
    return rect.bottom() - fraction * rect.height();
}

void drawLevelMeter(SgPainter& p, const QRectF& rect, double levelDb, double peakDb, double floorDb, double ceilingDb,
                    MeterWell well) {
    p.fillRect(rect, well == MeterWell::Panel ? Theme::panel() : Theme::meterBg());
    const double top = dbToY(levelDb, rect, floorDb, ceilingDb);
    if (top < rect.bottom()) {
        // Each colour only where the level reaches it, so the scale reads the same at any level.
        const double yellow = dbToY(ceilingDb - 12.0, rect, floorDb, ceilingDb);
        const double red = dbToY(ceilingDb - 3.0, rect, floorDb, ceilingDb);
        p.fillRect(QRectF(rect.left(), std::max(top, yellow), rect.width(), rect.bottom() - std::max(top, yellow)),
                   Theme::meterLow());
        if (top < yellow)
            p.fillRect(QRectF(rect.left(), std::max(top, red), rect.width(), yellow - std::max(top, red)),
                       Theme::meterMid());
        if (top < red)
            p.fillRect(QRectF(rect.left(), top, rect.width(), red - top), Theme::meterHigh());
    }
    for (double db = ceilingDb - 12.0; db > floorDb; db -= 12.0) {
        const double y = dbToY(db, rect, floorDb, ceilingDb);
        p.drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y),
                   well == MeterWell::Panel ? withAlpha(Theme::meterBg(), 145) : withAlpha(Theme::panel(), 160));
    }
    if (peakDb > floorDb) {
        const double y = dbToY(peakDb, rect, floorDb, ceilingDb);
        p.fillRect(QRectF(rect.left(), std::min(y, rect.bottom() - 1.5), rect.width(), 1.5),
                   peakDb >= ceilingDb - 0.05 ? Theme::meterHigh() : Theme::text());
    }
}

void drawReductionMeter(SgPainter& p, const QRectF& rect, double reductionDb, double rangeDb) {
    p.fillRect(rect, Theme::meterBg());
    const double depth = std::clamp(std::isfinite(reductionDb) ? reductionDb : 0.0, 0.0, rangeDb) / rangeDb;
    if (depth > 0.0) {
        QLinearGradient gradient(rect.topLeft(), rect.bottomLeft());
        gradient.setColorAt(0, withAlpha(Theme::accent(), 150));
        gradient.setColorAt(1, Theme::accent());
        p.fillRect(QRectF(rect.left(), rect.top(), rect.width(), depth * rect.height()), gradient);
    }
    for (double db = 6.0; db < rangeDb; db += 6.0) {
        const double y = rect.top() + db / rangeDb * rect.height();
        p.drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y), withAlpha(Theme::panel(), 160));
    }
}

void drawGlowPolyline(SgPainter& p, const std::vector<QPointF>& points, const QColor& color, double width) {
    if (points.size() < 2)
        return;
    const bool antialiased = p.antialiasing();
    p.setAntialiasing(true);
    p.drawPolyline(points.data(), int(points.size()), withAlpha(color, color.alpha() * 28 / 255), width + 5.0,
                   Qt::RoundCap);
    p.drawPolyline(points.data(), int(points.size()), withAlpha(color, color.alpha() * 70 / 255), width + 2.0,
                   Qt::RoundCap);
    p.drawPolyline(points.data(), int(points.size()), color, width, Qt::RoundCap);
    p.setAntialiasing(antialiased);
}

}  // namespace sub::ui
