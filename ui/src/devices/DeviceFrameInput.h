#pragma once

// What a device's frame does with the mouse (frame.py's _DeviceFrame mouse
// handlers): it lies under the frame's controls, so the clicks that reach it
// are on the frame's background, its title bar and labels. A press selects the
// device (Shift: a range; Ctrl: one more or less; a plain press on one already
// selected keeps the others, and its release selects just it if no drag
// followed: DeviceSelection::press/release); dragged past the start distance it
// drags the devices (not an instrument, which stays first); a double-click
// folds or unfolds a folded device (or any with Ctrl), else opens a plug-in's
// own editor; a right press selects it (unless it is selected) and asks for its
// menu (menuRequested).

#include <QPointF>
#include <QPointer>
#include <QString>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

#include <optional>

#include "devices/DeviceChainArea.h"
#include "session/Session.h"

namespace sub::ui {

class DeviceFrameInput : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY targetChanged)
    Q_PROPERTY(QString trackId READ trackId WRITE setTrackId NOTIFY targetChanged)
    Q_PROPERTY(QString deviceId READ deviceId WRITE setDeviceId NOTIFY targetChanged)
    // Where its drags start (the chain's area), and the frame they picture.
    Q_PROPERTY(sub::ui::DeviceChainArea* chainArea READ chainArea WRITE setChainArea NOTIFY targetChanged)
    Q_PROPERTY(QQuickItem* frame READ frame WRITE setFrame NOTIFY targetChanged)

public:
    explicit DeviceFrameInput(QQuickItem* parent = nullptr);

    sub::app::Session* session() const { return session_; }
    void setSession(sub::app::Session* session);
    QString trackId() const { return trackId_; }
    void setTrackId(const QString& trackId);
    QString deviceId() const { return deviceId_; }
    void setDeviceId(const QString& deviceId);
    DeviceChainArea* chainArea() const { return chainArea_; }
    void setChainArea(DeviceChainArea* area);
    QQuickItem* frame() const { return frame_; }
    void setFrame(QQuickItem* frame);

Q_SIGNALS:
    void targetChanged();
    // A right press: the device's menu, at (x, y) of this item.
    void menuRequested(qreal x, qreal y);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;

private:
    // Whether a press is a double-click's second (Qt Quick delivers it before
    // the double-click, as widgets didn't): it does nothing of its own.
    bool secondPressOfDoubleClick(const QMouseEvent* event);
    bool instrument() const;
    bool folded() const;

    QPointer<sub::app::Session> session_;
    QString trackId_;
    QString deviceId_;
    QPointer<DeviceChainArea> chainArea_;
    QPointer<QQuickItem> frame_;
    std::optional<QPointF> press_;  // where the left button went down (none: no click under way)
    ulong lastPress_ = 0;
    QPointF lastPressAt_;
    Qt::MouseButton lastButton_ = Qt::NoButton;
    bool lastWasSecond_ = false;
};

}  // namespace sub::ui
