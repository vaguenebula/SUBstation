#pragma once

// A rack's chains, as its chain list shows them (rack_view.py's ChainList):
// their ids in order (the list makes its rows again only when they change),
// and the one the device view shows beside the rack (the one last clicked,
// else the first: DeviceSelection::shownChain).
//
//   RackChains { id: chains; session: Session; trackId: ...; rackId: ... }
//   Repeater { model: chains.chainIds; ... }

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include "session/Session.h"

namespace sub::ui {

class RackChains : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY targetChanged)
    Q_PROPERTY(QString trackId READ trackId WRITE setTrackId NOTIFY targetChanged)
    Q_PROPERTY(QString rackId READ rackId WRITE setRackId NOTIFY targetChanged)
    Q_PROPERTY(QStringList chainIds READ chainIds NOTIFY chainIdsChanged)
    Q_PROPERTY(int count READ count NOTIFY chainIdsChanged)
    Q_PROPERTY(QString shownChain READ shownChain NOTIFY shownChainChanged)

public:
    explicit RackChains(QObject* parent = nullptr);

    sub::app::Session* session() const { return session_; }
    void setSession(sub::app::Session* session);
    QString trackId() const { return trackId_; }
    void setTrackId(const QString& trackId);
    QString rackId() const { return rackId_; }
    void setRackId(const QString& rackId);
    QStringList chainIds() const { return chainIds_; }
    int count() const { return int(chainIds_.size()); }
    QString shownChain() const { return shownChain_; }

    // + Chain (the rack's menu's Add Chain): a new, empty chain, last.
    Q_INVOKABLE void addChain();
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void targetChanged();
    void chainIdsChanged();
    void shownChainChanged();

private:
    QPointer<sub::app::Session> session_;
    QString trackId_;
    QString rackId_;
    QStringList chainIds_;
    QString shownChain_;
    QList<QMetaObject::Connection> connections_;
};

}  // namespace sub::ui
