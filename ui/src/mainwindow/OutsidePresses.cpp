#include "OutsidePresses.h"

#include <QCoreApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWindow>

namespace sub::ui {

OutsidePresses::OutsidePresses(QObject* parent) : QObject(parent) { watch(); }

OutsidePresses::~OutsidePresses() {
    if (watching_ && QCoreApplication::instance()) QCoreApplication::instance()->removeEventFilter(this);
}

void OutsidePresses::setItem(QQuickItem* item) {
    if (item == item_) return;
    item_ = item;
    Q_EMIT itemChanged();
}

void OutsidePresses::setEnabled(bool enabled) {
    if (enabled == enabled_) return;
    enabled_ = enabled;
    watch();
    Q_EMIT enabledChanged();
}

void OutsidePresses::setIgnore(bool ignore) {
    if (ignore == ignore_) return;
    ignore_ = ignore;
    Q_EMIT ignoreChanged();
}

void OutsidePresses::watch() {
    QCoreApplication* app = QCoreApplication::instance();
    if (!app || enabled_ == watching_) return;
    if (enabled_)
        app->installEventFilter(this);
    else
        app->removeEventFilter(this);
    watching_ = enabled_;
}

bool OutsidePresses::eventFilter(QObject* watched, QEvent* event) {
    // A press reaches a window first (then its items): only the window's is looked at.
    if (event->type() != QEvent::MouseButtonPress || !watched->isWindowType() || ignore_ || !item_) return false;
    auto* window = static_cast<QWindow*>(watched);
    bool inside = false;
    if (window == item_->window() && item_->isVisible()) {
        const QPointF at = item_->mapFromScene(static_cast<QMouseEvent*>(event)->scenePosition());
        inside = item_->contains(at);
    }
    if (!inside) Q_EMIT pressed();
    return false;
}

}  // namespace sub::ui
