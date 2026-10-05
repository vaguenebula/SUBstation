#include "arrangement/Cursors.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>

#include <utility>

namespace sub::ui::arrangement {

namespace {

QCursor makeTrimCursor(bool left) {
    constexpr int kSize = 24;
    QPixmap pixmap(kSize, kSize);
    pixmap.fill(Qt::transparent);
    const double x = left ? 9 : 14;  // the upright
    const double serif = left ? 5 : -5;
    QPainterPath path;
    path.moveTo(x + serif, 3);
    path.lineTo(x, 3);
    path.lineTo(x, 20);
    path.lineTo(x + serif, 20);
    const double arrow = left ? -1 : 1;  // small arrow pointing outward
    const double tip = x + arrow * 7;
    path.moveTo(x + arrow * 2, 11.5);
    path.lineTo(tip, 11.5);
    path.moveTo(tip - arrow * 3, 8.5);
    path.lineTo(tip, 11.5);
    path.lineTo(tip - arrow * 3, 14.5);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    for (const auto& [color, width] : {std::pair{QColor(0, 0, 0), 4.0}, std::pair{QColor(255, 255, 255), 2.0}}) {
        p.setPen(QPen(color, width, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        p.drawPath(path);
    }
    p.end();
    return QCursor(pixmap, static_cast<int>(x), 11);
}

QCursor makeAddCursor() {
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath arrow(QPointF(1, 1));
    for (const QPointF& point :
         {QPointF(1, 16), QPointF(4.5, 12.5), QPointF(7, 18), QPointF(9.5, 17), QPointF(7, 11.5), QPointF(12, 11.5)})
        arrow.lineTo(point);
    arrow.closeSubpath();
    p.setPen(QPen(QColor(0, 0, 0), 1.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.setBrush(QColor(255, 255, 255));
    p.drawPath(arrow);
    QPainterPath plus;
    plus.moveTo(14, 18.5);
    plus.lineTo(21, 18.5);
    plus.moveTo(17.5, 15);
    plus.lineTo(17.5, 22);
    p.setBrush(Qt::NoBrush);
    for (const auto& [color, width] : {std::pair{QColor(0, 0, 0), 3.5}, std::pair{QColor(255, 255, 255), 1.5}}) {
        p.setPen(QPen(color, width, Qt::SolidLine, Qt::FlatCap));
        p.drawPath(plus);
    }
    p.end();
    return QCursor(pixmap, 1, 1);
}

}  // namespace

// Made once and kept (never destroyed: a pixmap can't outlive the application).
QCursor trimCursor(bool left) {
    static const QCursor* const cursors[2] = {new QCursor(makeTrimCursor(true)), new QCursor(makeTrimCursor(false))};
    return *cursors[left ? 0 : 1];
}

QCursor addCursor() {
    static const QCursor* const cursor = new QCursor(makeAddCursor());
    return *cursor;
}

}  // namespace sub::ui::arrangement
