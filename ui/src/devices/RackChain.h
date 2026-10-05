#pragma once

// A rack's chain, as its row in the rack's chain list shows it (rack_view.py's
// _ChainRow): its name, its mixer (activator: not muted; solo among the rack's
// chains; volume and pan, as they are heard: following their automation while
// it plays, with the automation dots), its meter (meterUpdated, as the meters
// update), and what its controls and menu do through the editor: the mixer's
// edits (one undo step per gesture key for volume and pan), rename, duplicate,
// delete, add a chain to its rack, show its volume's or pan's automation.
//
//   RackChain { id: chain; session: Session; trackId: ...; rackId: ...; chainId: ... }
//   ValueBox { value: chain.volume; automation: chain.volumeAutomation; onMoved: (v, key) => chain.setVolume(v, key) }

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

#include "session/Session.h"

namespace sub::ui {

class RackChain : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY targetChanged)
    Q_PROPERTY(QString trackId READ trackId WRITE setTrackId NOTIFY targetChanged)
    Q_PROPERTY(QString rackId READ rackId WRITE setRackId NOTIFY targetChanged)
    Q_PROPERTY(QString chainId READ chainId WRITE setChainId NOTIFY targetChanged)
    Q_PROPERTY(bool exists READ exists NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(bool mute READ mute NOTIFY changed)
    Q_PROPERTY(bool solo READ solo NOTIFY changed)
    Q_PROPERTY(double volume READ volume NOTIFY changed)  // dB, as heard
    Q_PROPERTY(double pan READ pan NOTIFY changed)        // -1..1, as heard
    Q_PROPERTY(QString volumeAutomation READ volumeAutomation NOTIFY changed)  // "", "on" or "off"
    Q_PROPERTY(QString panAutomation READ panAutomation NOTIFY changed)

public:
    explicit RackChain(QObject* parent = nullptr);

    sub::app::Session* session() const { return session_; }
    void setSession(sub::app::Session* session);
    QString trackId() const { return trackId_; }
    void setTrackId(const QString& trackId);
    QString rackId() const { return rackId_; }
    void setRackId(const QString& rackId);
    QString chainId() const { return chainId_; }
    void setChainId(const QString& chainId);

    bool exists() const { return state_.exists; }
    QString name() const { return state_.name; }
    bool mute() const { return state_.mute; }
    bool solo() const { return state_.solo; }
    double volume() const { return state_.volume; }
    double pan() const { return state_.pan; }
    QString volumeAutomation() const { return state_.volumeAutomation; }
    QString panAutomation() const { return state_.panAutomation; }

    Q_INVOKABLE void setMute(bool mute);
    Q_INVOKABLE void setSolo(bool solo);
    Q_INVOKABLE void setVolume(double volumeDb, const QString& mergeKey = QString());
    Q_INVOKABLE void setPan(double pan, const QString& mergeKey = QString());
    // Renamed in place: a name left empty keeps the old one.
    Q_INVOKABLE void rename(const QString& name);
    Q_INVOKABLE void duplicate();
    Q_INVOKABLE void remove();
    Q_INVOKABLE void addChain();  // to its rack, last
    Q_INVOKABLE void showVolumeAutomation();
    Q_INVOKABLE void showPanAutomation();

    // The volume box's and the pan knob's texts ("-6.0 dB", "25L"), and the pan typed.
    Q_INVOKABLE static QString formatVolume(double volumeDb);
    Q_INVOKABLE static QString formatPan(double pan);
    Q_INVOKABLE static QVariant parsePan(const QString& text);

    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void targetChanged();
    void changed();
    // As the meters update: its meter's peaks since the last update (linear).
    void meterUpdated(double left, double right);

private:
    struct State {
        bool exists = false;
        QString name;
        bool mute = false;
        bool solo = false;
        double volume = 0.0;
        double pan = 0.0;
        QString volumeAutomation;
        QString panAutomation;

        bool operator==(const State&) const = default;
    };

    void connectSession();
    QString volumeKey() const;
    QString panKey() const;
    QString automationStateOf(const QString& key) const;

    QPointer<sub::app::Session> session_;
    QString trackId_;
    QString rackId_;
    QString chainId_;
    State state_;
    QList<QMetaObject::Connection> connections_;
};

}  // namespace sub::ui
