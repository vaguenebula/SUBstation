#include "controls/DragCursor.h"

#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QQuickItem>
#include <QScreen>
#include <QWindow>

namespace sub::ui {

DragCursor::DragCursor(QObject* parent) : QObject(parent) {}

DragCursor::~DragCursor() { release(); }

void DragCursor::press(const QPointF& globalPosition) {
    release();
    start_ = globalPosition.toPoint();
    qApp->installEventFilter(this);
}

qreal DragCursor::moved(QQuickItem* item, const QPointF& position, const QPointF& globalPosition, bool jump) {
    const qreal y = position.y();
    if (!start_)
        return y;
    if (!hidden_) {
        QGuiApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
        hidden_ = true;
    }
    const QPoint at = globalPosition.toPoint();
    QScreen* screen = QGuiApplication::screenAt(at);
    if (jump && screen) {
        const QRect area = screen->geometry();
        if (at.y() <= area.top() + 1 || at.y() >= area.bottom() - 1) {
            const QPoint middle(at.x(), area.center().y());
            QCursor::setPos(screen, middle);
            return item->mapFromGlobal(QPointF(middle)).y();
        }
    }
    return y;
}

void DragCursor::release() {
    if (start_)
        qApp->removeEventFilter(this);
    if (hidden_) {
        QGuiApplication::restoreOverrideCursor();
        QCursor::setPos(*start_);
    }
    start_.reset();
    hidden_ = false;
}

bool DragCursor::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Show) {
        auto* window = qobject_cast<QWindow*>(watched);
        if (window && window->type() == Qt::Popup)
            release();
    }
    return false;
}

}  // namespace sub::ui
