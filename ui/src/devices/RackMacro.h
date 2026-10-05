#pragma once

// One of a rack's eight macros, as its knob in the device view shows it: its
// value (0..1, the rack's parameter macroParam(index)), what it is mapped to
// ("Utility: Gain", the device's name and the parameter's), the knob's tooltip
// listing them, and turning it (the editor sets it and what it moves, one undo
// step per gesture) or unmapping one of them (its right-click menu).
//
//   RackMacro { id: macro; session: Session; trackId: ...; rackId: ...; index: 0 }
//   Knob { value: macro.value; onMoved: (v, key) => macro.set(v, key) }

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include "session/Session.h"

namespace sub::ui {

class RackMacro : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY targetChanged)
    Q_PROPERTY(QString trackId READ trackId WRITE setTrackId NOTIFY targetChanged)
    Q_PROPERTY(QString rackId READ rackId WRITE setRackId NOTIFY targetChanged)
    Q_PROPERTY(int index READ index WRITE setIndex NOTIFY targetChanged)  // 0..7
    Q_PROPERTY(QString name READ name NOTIFY targetChanged)  // "Macro 1"
    Q_PROPERTY(double value READ value NOTIFY changed)
    Q_PROPERTY(bool mapped READ mapped NOTIFY changed)
    Q_PROPERTY(QStringList mappings READ mappings NOTIFY changed)  // "Utility: Gain", one per parameter it moves
    Q_PROPERTY(QString toolTip READ toolTip NOTIFY changed)

public:
    explicit RackMacro(QObject* parent = nullptr);

    sub::app::Session* session() const { return session_; }
    void setSession(sub::app::Session* session);
    QString trackId() const { return trackId_; }
    void setTrackId(const QString& trackId);
    QString rackId() const { return rackId_; }
    void setRackId(const QString& rackId);
    int index() const { return index_; }
    void setIndex(int index);
    QString name() const { return QStringLiteral("Macro %1").arg(index_ + 1); }
    double value() const { return value_; }
    bool mapped() const { return !mappings_.isEmpty(); }
    QStringList mappings() const { return mappings_; }
    QString toolTip() const;

    // Turns it: it and what it moves, one undo step per gesture key.
    Q_INVOKABLE void set(double value, const QString& mergeKey = QString());
    // Its right-click menu: [{text: "Unmap Utility: Gain", deviceId, paramId}] (none: nothing mapped).
    Q_INVOKABLE QVariantList unmapEntries() const;
    Q_INVOKABLE void unmap(const QString& deviceId, const QString& paramId);
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void targetChanged();
    void changed();

private:
    QPointer<sub::app::Session> session_;
    QString trackId_;
    QString rackId_;
    int index_ = 0;
    double value_ = 0.0;
    QStringList mappings_;
    QVariantList entries_;
    QList<QMetaObject::Connection> connections_;
};

}  // namespace sub::ui
