#pragma once

// The devices of one chain of the track the device view shows (the track's
// own, chainId "", or a rack's chain shown beside the rack), in order, while
// the view shows them (none for a frozen track, or none shown). `deviceIds`
// changes only when they do, so a Repeater over it makes its frames again
// only then (the device view's rebuild), not as devices are selected, switched
// or edited (their frames follow those themselves).
//
//   DeviceChainList { id: devices; session: Session; chainId: "" }
//   Repeater { model: devices.deviceIds; DeviceFrame { ... } }

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include "session/Session.h"

namespace sub::ui {

class DeviceChainList : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY targetChanged)
    Q_PROPERTY(QString chainId READ chainId WRITE setChainId NOTIFY targetChanged)
    Q_PROPERTY(QString trackId READ trackId NOTIFY deviceIdsChanged)  // the track shown
    Q_PROPERTY(QStringList deviceIds READ deviceIds NOTIFY deviceIdsChanged)
    Q_PROPERTY(int count READ count NOTIFY deviceIdsChanged)

public:
    explicit DeviceChainList(QObject* parent = nullptr);

    sub::app::Session* session() const { return session_; }
    void setSession(sub::app::Session* session);
    QString chainId() const { return chainId_; }
    void setChainId(const QString& chainId);
    QString trackId() const { return trackId_; }
    QStringList deviceIds() const { return deviceIds_; }
    int count() const { return int(deviceIds_.size()); }

    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void targetChanged();
    void deviceIdsChanged();

private:
    QPointer<sub::app::Session> session_;
    QString chainId_;
    QString trackId_;
    QStringList deviceIds_;
    QMetaObject::Connection connection_;
};

}  // namespace sub::ui
