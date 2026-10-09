#pragma once

// One of a rack's macros, as its knob in the device view shows it: its name
// ("Macro N", or the one the user gave it), its value (0..1, the rack's
// parameter macroParam(index)) as it is now (its envelope's while its
// automation plays: refreshed as the playhead moves), its automation state
// (the knob's dot), what it is mapped to ("Utility: Gain", the device's name
// and the parameter's) and over which range, the knob's tooltip listing them;
// turning it (the editor sets it and what it moves, one undo step per gesture),
// renaming it, its automation (shown, deleted, re-enabled) and its mappings'
// ranges (in the parameters' own units too) or unmapping one of them (its
// right-click menu and the mappings' editor).
//
//   RackMacro { id: macro; session: Session; trackId: ...; rackId: ...; index: 0 }
//   Knob { value: macro.value; automation: macro.automation; onMoved: (v, key) => macro.set(v, key) }

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include "controls/Automation.h"
#include "session/Session.h"

namespace sub::ui {

class RackMacro : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY targetChanged)
    Q_PROPERTY(QString trackId READ trackId WRITE setTrackId NOTIFY targetChanged)
    Q_PROPERTY(QString rackId READ rackId WRITE setRackId NOTIFY targetChanged)
    Q_PROPERTY(int index READ index WRITE setIndex NOTIFY targetChanged)  // 0-based
    Q_PROPERTY(QString key READ key NOTIFY targetChanged)  // its automation key
    Q_PROPERTY(QString name READ name NOTIFY changed)  // "Macro 1", or as named
    Q_PROPERTY(bool named READ named NOTIFY changed)  // the user gave it a name
    Q_PROPERTY(double value READ value NOTIFY changed)
    Q_PROPERTY(QString automation READ automation NOTIFY changed)  // "", "on" or "off"
    Q_PROPERTY(bool mapped READ mapped NOTIFY changed)
    Q_PROPERTY(QStringList mappings READ mappings NOTIFY changed)  // "Utility: Gain", one per parameter it moves
    // What it moves: [{deviceId, paramId, name ("Utility: Gain"), low, high (normalized), lowText, highText (in
    // the parameter's units)}], in the order they were mapped.
    Q_PROPERTY(QVariantList mappingList READ mappingList NOTIFY changed)
    // Which parameters it moves, "<device id>/<param id>" in the order of mappingList; it changes only when they
    // do, not their ranges (a list of them is made again only then: not during a drag of a range).
    Q_PROPERTY(QStringList mappingKeys READ mappingKeys NOTIFY mappingKeysChanged)
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
    QString key() const;
    QString name() const { return name_; }
    bool named() const { return named_; }
    double value() const { return value_; }
    QString automation() const { return automation_; }
    bool mapped() const { return !mappings_.isEmpty(); }
    QStringList mappings() const { return mappings_; }
    QVariantList mappingList() const { return mappingList_; }
    QStringList mappingKeys() const { return mappingKeys_; }
    QString toolTip() const;

    // Turns it: it and what it moves, one undo step per gesture key.
    Q_INVOKABLE void set(double value, const QString& mergeKey = QString());
    // Taken hold of: the arrangement shows its automation.
    Q_INVOKABLE void touch();
    // Names it ("": by its number again). One undo step.
    Q_INVOKABLE void rename(const QString& name);
    // Its right-click menu: [{text: "Unmap Utility: Gain", deviceId, paramId}] (none: nothing mapped).
    Q_INVOKABLE QVariantList unmapEntries() const;
    Q_INVOKABLE void unmap(const QString& deviceId, const QString& paramId);
    // The range a mapped parameter moves over (normalized; low > high: the other
    // way round). One undo step per gesture key.
    Q_INVOKABLE void setRange(const QString& deviceId, const QString& paramId, double low, double high,
                              const QString& mergeKey = QString());
    // A mapped parameter's normalized value in its own units ("-12.0 dB").
    Q_INVOKABLE QString formatTarget(const QString& deviceId, const QString& paramId, double normalized) const;
    // Its rack's macros: one more (last, at 0), or the last taken away (with
    // its mappings and automation); one undo step each.
    Q_INVOKABLE bool canAddMacro() const;
    Q_INVOKABLE bool canRemoveMacro() const;
    Q_INVOKABLE void addMacro();
    Q_INVOKABLE void removeLastMacro();
    // Its automation.
    Q_INVOKABLE bool canAutomate() const;
    Q_INVOKABLE bool hasEnvelope() const;
    Q_INVOKABLE bool isOverridden() const;
    Q_INVOKABLE void showAutomation();
    Q_INVOKABLE void deleteAutomation();
    Q_INVOKABLE void reEnableAutomation();
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void targetChanged();
    void changed();
    void mappingKeysChanged();

private:
    AutomationTarget automationTarget() const;
    void refreshValue();

    QPointer<sub::app::Session> session_;
    QString trackId_;
    QString rackId_;
    int index_ = 0;
    QString name_ = QStringLiteral("Macro 1");
    bool named_ = false;
    double value_ = 0.0;
    QString automation_;
    QStringList mappings_;
    QVariantList mappingList_;
    QStringList mappingKeys_;
    QVariantList entries_;
    QList<QMetaObject::Connection> connections_;
};

// How many macros a rack shows, and adding or taking away the last one (the
// rack's body's + and - buttons).
//
//   RackMacros { id: macros; session: Session; trackId: ...; rackId: ... }
//   Repeater { model: macros.count; ... }
class RackMacros : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY targetChanged)
    Q_PROPERTY(QString trackId READ trackId WRITE setTrackId NOTIFY targetChanged)
    Q_PROPERTY(QString rackId READ rackId WRITE setRackId NOTIFY targetChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int maximum READ maximum CONSTANT)  // kMaxMacroCount

public:
    explicit RackMacros(QObject* parent = nullptr);

    sub::app::Session* session() const { return session_; }
    void setSession(sub::app::Session* session);
    QString trackId() const { return trackId_; }
    void setTrackId(const QString& trackId);
    QString rackId() const { return rackId_; }
    void setRackId(const QString& rackId);
    int count() const { return count_; }
    int maximum() const;

    // A macro more (last, at 0); the last one taken away, with its mappings and automation. One undo step each.
    Q_INVOKABLE void add();
    Q_INVOKABLE void remove();
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void targetChanged();
    void countChanged();

private:
    QPointer<sub::app::Session> session_;
    QString trackId_;
    QString rackId_;
    int count_ = 0;
    QList<QMetaObject::Connection> connections_;
};

}  // namespace sub::ui
