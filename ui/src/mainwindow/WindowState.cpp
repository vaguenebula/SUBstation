#include "WindowState.h"

#include <QByteArray>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>
#include <QSettings>
#include <QVariantMap>
#include <QWindow>

namespace sub::ui {

namespace {

const QString kRect = QStringLiteral("rect");
const QString kMaximized = QStringLiteral("maximized");

// A rect some screen shows enough of to grab (else on the primary screen, centred).
QRect onScreen(QRect rect) {
    for (QScreen* screen : QGuiApplication::screens()) {
        const QRect visible = screen->availableGeometry().intersected(rect);
        if (visible.width() >= 100 && visible.height() >= 50) return rect;
    }
    if (QScreen* primary = QGuiApplication::primaryScreen()) {
        const QRect available = primary->availableGeometry();
        rect.setSize(rect.size().boundedTo(available.size()));
        rect.moveCenter(available.center());
    }
    return rect;
}

}  // namespace

WindowState::WindowState(QObject* parent) : QObject(parent) {}

void WindowState::setWindow(QQuickWindow* window) {
    if (window == window_) return;
    if (window_) disconnect(window_, nullptr, this, nullptr);
    window_ = window;
    if (window_) {
        for (auto changed : {&QWindow::xChanged, &QWindow::yChanged, &QWindow::widthChanged, &QWindow::heightChanged})
            connect(window_, changed, this, &WindowState::track);
        connect(window_, &QWindow::windowStateChanged, this, &WindowState::track);
        track();
    }
    Q_EMIT windowChanged();
}

void WindowState::track() {
    if (window_ && !(window_->windowStates() & (Qt::WindowMaximized | Qt::WindowFullScreen | Qt::WindowMinimized)))
        normal_ = window_->geometry();
}

bool WindowState::restore() {
    if (!window_) return false;
    const QVariant stored = QSettings().value(kGeometryKey);
    if (stored.typeId() != QMetaType::QVariantMap) return false;  // (none, or a widget version's bytes)
    const QVariantMap geometry = stored.toMap();
    const QRect rect = geometry.value(kRect).toRect();
    if (!rect.isValid() || rect.width() < 100 || rect.height() < 100) return false;
    window_->setGeometry(onScreen(rect));
    normal_ = window_->geometry();
    if (geometry.value(kMaximized).toBool()) window_->setWindowStates(Qt::WindowMaximized);
    return true;
}

QVariant WindowState::splitterState() const {
    const QVariant state = QSettings().value(kSplitterKey);
    // (A widget version's QSplitter::saveState() bytes start with its magic number, 0xff as a big-endian qint32.)
    if (state.toByteArray().startsWith(QByteArray("\x00\x00\x00\xff", 4))) return {};
    return state;
}

QVariant WindowState::deviceSplitterState() const { return QSettings().value(kDeviceSplitterKey); }

void WindowState::save(const QVariant& splitterState, const QVariant& deviceSplitterState) {
    QSettings settings;
    if (window_) {
        const QRect rect = normal_.isValid() ? normal_ : window_->geometry();
        settings.setValue(kGeometryKey,
                          QVariantMap{{kRect, rect}, {kMaximized, bool(window_->windowStates() & Qt::WindowMaximized)}});
    }
    if (splitterState.isValid()) settings.setValue(kSplitterKey, splitterState);
    if (deviceSplitterState.isValid()) settings.setValue(kDeviceSplitterKey, deviceSplitterState);
}

}  // namespace sub::ui
