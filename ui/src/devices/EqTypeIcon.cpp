#include "devices/EqTypeIcon.h"

#include "devices/EditorPaint.h"
#include "devices/EqGraph.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <vector>

namespace sub::ui {

namespace {

// A filter type's outline in `r` (eq.py's _type_shape).
std::vector<QPointF> typeShape(int kind, const QRectF& r) {
    const double left = r.left(), right = r.right(), top = r.top(), bottom = r.bottom();
    const double midX = r.center().x(), midY = r.center().y();
    std::vector<QPointF> path;
    auto move = [&](double x, double y) { path.assign(1, QPointF(x, y)); };
    auto cubic = [&](double x1, double y1, double x2, double y2, double x, double y) {
        appendCubic(path, path.back(), QPointF(x1, y1), QPointF(x2, y2), QPointF(x, y));
    };
    switch (kind) {
    case EqGraph::Bell:
        move(left, midY + 1);
        cubic(midX - 2, midY + 1, midX - 2, top, midX, top);
        cubic(midX + 2, top, midX + 2, midY + 1, right, midY + 1);
        break;
    case EqGraph::LowShelf:
        move(left, top + 1);
        cubic(midX, top + 1, midX, bottom - 2, right, bottom - 2);
        break;
    case EqGraph::HighShelf:
        move(left, bottom - 2);
        cubic(midX, bottom - 2, midX, top + 1, right, top + 1);
        break;
    case EqGraph::LowCut:
        move(left + 1, bottom);
        cubic(midX - 1, top + 1, midX, top + 1, right, top + 1);
        break;
    case EqGraph::HighCut:
        move(left, top + 1);
        cubic(midX, top + 1, midX + 1, top + 1, right - 1, bottom);
        break;
    case EqGraph::Notch:
        move(left, top + 1);
        cubic(midX - 1, top + 1, midX - 0.5, bottom, midX, bottom);
        cubic(midX + 0.5, bottom, midX + 1, top + 1, right, top + 1);
        break;
    case EqGraph::BandPass:
        move(left, bottom);
        cubic(midX - 2, bottom, midX - 1, top, midX, top);
        cubic(midX + 1, top, midX + 2, bottom, right, bottom);
        break;
    default:  // tilt
        move(left, bottom - 1);
        cubic(midX - 2, bottom - 1, midX + 2, top + 1, right, top + 1);
        break;
    }
    return path;
}

}  // namespace

EqTypeIcon::EqTypeIcon(QQuickItem* parent) : SgCanvas(parent), color_(Theme::kAccent) { setImplicitSize(17, 15); }

void EqTypeIcon::setKind(int kind) {
    if (kind == kind_)
        return;
    kind_ = kind;
    update();
    Q_EMIT lookChanged();
}

void EqTypeIcon::setColor(const QColor& color) {
    if (color == color_)
        return;
    color_ = color;
    update();
    Q_EMIT lookChanged();
}

void EqTypeIcon::setChecked(bool checked) {
    if (checked == checked_)
        return;
    checked_ = checked;
    update();
    Q_EMIT lookChanged();
}

void EqTypeIcon::setHovered(bool hovered) {
    if (hovered == hovered_)
        return;
    hovered_ = hovered;
    update();
    Q_EMIT lookChanged();
}

void EqTypeIcon::itemChange(ItemChange change, const ItemChangeData& data) {
    SgCanvas::itemChange(change, data);
    if (change == ItemEnabledHasChanged)
        update();
}

void EqTypeIcon::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF rect = QRectF(0, 0, width(), height()).adjusted(0.5, 0.5, -0.5, -0.5);
    const bool enabled = isEnabled();
    p.fillRoundedRect(rect, 3, 3,
                      checked_ ? withAlpha(color_, 60) : (hovered_ && enabled ? Theme::kSurfaceHover : Theme::kSurface));
    const QColor line = checked_ ? color_ : (enabled ? Theme::kText : Theme::kTextDisabled);
    const std::vector<QPointF> shape = typeShape(kind_, rect.adjusted(3, 3, -3, -3));
    p.drawPolyline(shape.data(), int(shape.size()), line, 1.3, Qt::RoundCap);
}

}  // namespace sub::ui
