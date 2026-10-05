#pragma once

// A built-in device's parameters, as its processor lists them (DeviceWidget's
// `infos`): their ids in order, for laying out a knob per parameter
// (DeviceParamKnob) in pages. Empty while the device (or its processor) isn't
// there; it follows the device as it comes and goes.
//
//   DeviceParams { id: params; session: Session; trackId: ...; deviceId: ... }
//   Repeater { model: params.ids; DeviceParamKnob { required property string modelData; paramId: modelData ... } }

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include "session/Session.h"

namespace sub::ui {

class DeviceParams : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY targetChanged)
    Q_PROPERTY(QString trackId READ trackId WRITE setTrackId NOTIFY targetChanged)
    Q_PROPERTY(QString deviceId READ deviceId WRITE setDeviceId NOTIFY targetChanged)
    Q_PROPERTY(QStringList ids READ ids NOTIFY idsChanged)
    Q_PROPERTY(int count READ count NOTIFY idsChanged)

public:
    explicit DeviceParams(QObject* parent = nullptr);

    sub::app::Session* session() const { return session_; }
    void setSession(sub::app::Session* session);
    QString trackId() const { return trackId_; }
    void setTrackId(const QString& trackId);
    QString deviceId() const { return deviceId_; }
    void setDeviceId(const QString& deviceId);
    QStringList ids() const { return ids_; }
    int count() const { return int(ids_.size()); }

    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void targetChanged();
    void idsChanged();

private:
    QPointer<sub::app::Session> session_;
    QString trackId_;
    QString deviceId_;
    QStringList ids_;
    QList<QMetaObject::Connection> connections_;
};

}  // namespace sub::ui
