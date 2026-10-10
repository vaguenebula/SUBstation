#include "devices/ClashView.h"

#include "devices/CurveGraph.h"
#include "devices/EditorPaint.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

namespace sf = sub::app::sidechainFit;

namespace {

const QColor kBackgroundTop(0x1d, 0x20, 0x27), kBackgroundBottom(0x10, 0x12, 0x16);
const QColor kGridMinor(255, 255, 255, 11);
const QColor kLabelColor(255, 255, 255, 90);
const QColor kKickColor(0xff, 0x9f, 0x5a);
const QColor kBassColor(0x6e, 0xa0, 0xeb);
const QColor kClashColor(0xff, 0x6f, 0xb5);

// A rounded rect filled with a vertical gradient: its middle and sides exactly, its corners in the colour there.
void fillRoundedGradient(SgPainter& p, const QRectF& rect, double radius, const QColor& top, const QColor& bottom) {
    QLinearGradient gradient(rect.topLeft(), rect.bottomLeft());
    gradient.setColorAt(0, top);
    gradient.setColorAt(1, bottom);
    auto at = [&](double y) {
        const double f = std::clamp((y - rect.top()) / rect.height(), 0.0, 1.0);
        return QColor::fromRgbF(top.redF() + (bottom.redF() - top.redF()) * f,
                                top.greenF() + (bottom.greenF() - top.greenF()) * f,
                                top.blueF() + (bottom.blueF() - top.blueF()) * f);
    };
    for (const QPointF& corner : {QPointF(rect.left() + radius, rect.top() + radius),
                                  QPointF(rect.right() - radius, rect.top() + radius),
                                  QPointF(rect.left() + radius, rect.bottom() - radius),
                                  QPointF(rect.right() - radius, rect.bottom() - radius)})
        p.fillEllipse(corner, radius, radius, at(corner.y()));
    p.fillRect(QRectF(rect.left() + radius, rect.top(), rect.width() - 2 * radius, rect.height()), gradient);
    p.fillRect(QRectF(rect.left(), rect.top() + radius, rect.width(), rect.height() - 2 * radius), gradient);
}

}  // namespace

ClashView::ClashView(QQuickItem* parent) : SgCanvas(parent) {
    setImplicitSize(150, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton);
    setCursor(Qt::PointingHandCursor);
}

void ClashView::setGraph(CurveGraph* graph) {
    if (graph == graph_)
        return;
    if (graph_)
        disconnect(graph_, nullptr, this, nullptr);
    graph_ = graph;
    if (graph_) {
        connect(graph_, &CurveGraph::fitChanged, this, &QQuickItem::update);
        connect(graph_, &CurveGraph::curveChanged, this, &QQuickItem::update);
    }
    update();
    Q_EMIT graphChanged();
}

void ClashView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && graph_)
        graph_->fitNow();
}

void ClashView::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF rect = QRectF(0, 0, width(), height()).adjusted(0.5, 0.5, -0.5, -0.5);
    fillRoundedGradient(p, rect, 5, kBackgroundTop, kBackgroundBottom);
    p.drawRoundedRect(rect, 5, 5, QColor(255, 255, 255, 22), 1);
    const QRectF plot = rect.adjusted(4, 13, -4, -3);
    const QFont small = uiFont(7);
    if (!graph_ || !graph_->fit()) {
        p.drawText(rect.adjusted(6, 0, -6, 0), Qt::AlignCenter | Qt::TextWordWrap,
                   QStringLiteral("Play the kick (and the bass) to fit to it"), kLabelColor, small);
        return;
    }
    const sf::Spectra& found = graph_->fit()->spectra;
    const LogAxis axis{sf::kClashLow, sf::kClashHigh, plot.left(), plot.width()};
    auto x = [&](double freq) { return axis.position(freq); };
    auto y = [&](double db) { return plot.bottom() - (std::max(db, -48.0) + 48.0) / 48.0 * plot.height(); };

    for (double freq : {50.0, 100.0, 200.0, 500.0, 1000.0})
        p.drawLine(QPointF(x(freq), plot.top()), QPointF(x(freq), plot.bottom()), kGridMinor, 1);
    p.fillRect(QRectF(QPointF(x(found.low), plot.top()), QPointF(x(found.high), plot.bottom())),
               withAlpha(kClashColor, 30));
    std::vector<double> xs(found.freqs.size());
    for (std::size_t i = 0; i < xs.size(); ++i)
        xs[i] = x(found.freqs[i]);
    if (found.bassHeard && !xs.empty()) {
        std::vector<QPointF> clash;
        clash.emplace_back(xs.front(), plot.bottom());
        for (std::size_t i = 0; i < xs.size(); ++i)
            clash.emplace_back(xs[i], y(found.clash[i]));
        clash.emplace_back(xs.back(), plot.bottom());
        p.fillToBaseline(clash.data(), int(clash.size()), plot.bottom(), withAlpha(kClashColor, 90));
    }
    for (const auto& [values, color] : {std::pair{&found.bass, kBassColor}, std::pair{&found.kick, kKickColor}}) {
        if (values == &found.bass && !found.bassHeard)
            continue;
        std::vector<QPointF> line;
        for (std::size_t i = 0; i < xs.size() && i < values->size(); ++i)
            line.emplace_back(xs[i], y((*values)[i]));
        p.drawPolyline(line.data(), int(line.size()), color, 1.3);
    }
    const QString band = pythonFixed(found.low, 0) + QStringLiteral("–") + pythonFixed(found.high, 0);
    const QString title = found.bassHeard ? QStringLiteral("Clash %1 Hz").arg(band)
                                          : QStringLiteral("Kick %1 Hz (no bass)").arg(band);
    p.drawText(QRectF(rect.left() + 6, rect.top() + 1, rect.width() - 12, 12), Qt::AlignLeft | Qt::AlignVCenter, title,
               withAlpha(kClashColor, 230), small);
}

}  // namespace sub::ui
