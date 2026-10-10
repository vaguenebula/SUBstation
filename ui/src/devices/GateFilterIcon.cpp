#include "devices/GateFilterIcon.h"

#include "devices/EditorPaint.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <vector>

namespace sub::ui {

namespace {

// Type `type`'s outline in `r`.
std::vector<QPointF> typeShape(int type, const QRectF& r) {
    const double left = r.left(), right = r.right(), top = r.top(), bottom = r.bottom();
    const double midX = r.center().x(), midY = r.center().y();
    std::vector<QPointF> path;
    path.reserve(40);
    auto move = [&](double x, double y) { path.assign(1, QPointF(x, y)); };
    auto cubic = [&](double x1, double y1, double x2, double y2, double x, double y) {
        const QPointF from = path.back();  // (a copy: appendCubic grows the path)
        appendCubic(path, from, QPointF(x1, y1), QPointF(x2, y2), QPointF(x, y));
    };
    switch (type) {
    case 0:  // low shelf
        move(left, top + 1);
        cubic(midX, top + 1, midX, bottom - 2, right, bottom - 2);
        break;
    case 1:  // bell
        move(left, midY + 2);
        cubic(midX - 2, midY + 2, midX - 2, top, midX, top);
        cubic(midX + 2, top, midX + 2, midY + 2, right, midY + 2);
        break;
    case 2:  // high shelf
        move(left, bottom - 2);
        cubic(midX, bottom - 2, midX, top + 1, right, top + 1);
        break;
    case 3:  // low-pass
        move(left, top + 1);
        cubic(midX, top + 1, midX + 1, top + 1, right - 1, bottom);
        break;
    case 4:  // band-pass
        move(left, bottom);
        cubic(midX - 2, bottom, midX - 1, top, midX, top);
        cubic(midX + 1, top, midX + 2, bottom, right, bottom);
        break;
    default:  // high-pass
        move(left + 1, bottom);
        cubic(midX - 1, top + 1, midX, top + 1, right, top + 1);
        break;
    }
    return path;
}

}  // namespace

GateFilterIcon::GateFilterIcon(QQuickItem* parent) : SgCanvas(parent) { setImplicitSize(17, 15); }

void GateFilterIcon::setType(int type) {
    if (type == type_)
        return;
    type_ = type;
    update();
    Q_EMIT lookChanged();
}

void GateFilterIcon::setChecked(bool checked) {
    if (checked == checked_)
        return;
    checked_ = checked;
    update();
    Q_EMIT lookChanged();
}

void GateFilterIcon::setHovered(bool hovered) {
    if (hovered == hovered_)
        return;
    hovered_ = hovered;
    update();
    Q_EMIT lookChanged();
}

void GateFilterIcon::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF rect = QRectF(0, 0, width(), height()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.fillRoundedRect(rect, 3, 3,
                      checked_ ? withAlpha(Theme::kAccent, 60) : (hovered_ ? Theme::kSurfaceHover : Theme::kSurface));
    const QColor line = checked_ ? Theme::kAccent : Theme::kText;
    const std::vector<QPointF> shape = typeShape(type_, rect.adjusted(3, 3, -3, -3));
    p.drawPolyline(shape.data(), int(shape.size()), line, 1.3, Qt::RoundCap);
}

}  // namespace sub::ui
