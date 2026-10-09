#include "devices/DeviceFrameInput.h"

#include "audio/EngineBridge.h"
#include "model/Devices.h"
#include "model/Project.h"
#include "session/DeviceSelection.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QStyleHints>

namespace sub::ui {

DeviceFrameInput::DeviceFrameInput(QQuickItem* parent) : QQuickItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
}

void DeviceFrameInput::setSession(sub::app::Session* session) {
    if (session == session_)
        return;
    session_ = session;
    Q_EMIT targetChanged();
}

void DeviceFrameInput::setTrackId(const QString& trackId) {
    if (trackId == trackId_)
        return;
    trackId_ = trackId;
    Q_EMIT targetChanged();
}

void DeviceFrameInput::setDeviceId(const QString& deviceId) {
    if (deviceId == deviceId_)
        return;
    deviceId_ = deviceId;
    Q_EMIT targetChanged();
}

void DeviceFrameInput::setChainArea(DeviceChainArea* area) {
    if (area == chainArea_)
        return;
    chainArea_ = area;
    Q_EMIT targetChanged();
}

void DeviceFrameInput::setFrame(QQuickItem* frame) {
    if (frame == frame_)
        return;
    frame_ = frame;
    Q_EMIT targetChanged();
}

bool DeviceFrameInput::instrument() const {
    const sub::app::Device* device = session_ ? session_->project()->findDevice(trackId_, deviceId_) : nullptr;
    return device != nullptr && sub::app::deviceIsInstrument(*device);
}

bool DeviceFrameInput::folded() const { return session_ && session_->project()->isDeviceFolded(deviceId_); }

bool DeviceFrameInput::secondPressOfDoubleClick(const QMouseEvent* event) {
    return doubleClicks_.isSecondPress(event);
}

void DeviceFrameInput::mousePressEvent(QMouseEvent* event) {
    event->accept();
    const bool second = secondPressOfDoubleClick(event);
    if (!session_ || !session_->project()->hasDevice(trackId_, deviceId_))
        return;
    sub::app::DeviceSelection* devices = session_->deviceSelection();
    if (event->button() == Qt::LeftButton) {
        press_.reset();
        if (second)
            return;  // (the double-click does what it does)
        press_ = event->position();
        devices->press(deviceId_, event->modifiers().toInt());
    } else if (event->button() == Qt::RightButton) {
        devices->menuRequested(deviceId_);  // (selected before its menu shows)
        Q_EMIT menuRequested(event->position().x(), event->position().y());
    }
}

void DeviceFrameInput::mouseMoveEvent(QMouseEvent* event) {
    if (!press_ || !(event->buttons() & Qt::LeftButton) || instrument())
        return;
    if ((event->position() - *press_).manhattanLength() < QGuiApplication::styleHints()->startDragDistance())
        return;
    press_.reset();
    ungrabMouse();  // (the drag takes the mouse)
    if (chainArea_)
        chainArea_->startDrag(deviceId_, frame_);
}

void DeviceFrameInput::mouseReleaseEvent(QMouseEvent* event) {
    if (press_ && event->button() == Qt::LeftButton && session_ && session_->project()->hasDevice(trackId_, deviceId_))
        session_->deviceSelection()->release(deviceId_, event->modifiers().toInt());
    press_.reset();
}

void DeviceFrameInput::mouseDoubleClickEvent(QMouseEvent* event) {
    event->accept();
    if (event->button() != Qt::LeftButton || !session_ || !session_->project()->hasDevice(trackId_, deviceId_))
        return;
    if (folded() || (event->modifiers() & Qt::ControlModifier)) {
        session_->deviceSelection()->toggleFold(deviceId_);  // (Ctrl+double-click folds it, as the triangle does)
        return;
    }
    // What double-clicking a device does: a plug-in shows its own editor (brought to the front if open).
    const sub::app::Device* device = session_->project()->findDevice(trackId_, deviceId_);
    if (device != nullptr && device->isPlugin() && session_->bridge()->engineDeviceId(trackId_, deviceId_))
        session_->bridge()->openPluginEditor(trackId_, deviceId_);
}

void DeviceFrameInput::mouseUngrabEvent() { press_.reset(); }

}  // namespace sub::ui
