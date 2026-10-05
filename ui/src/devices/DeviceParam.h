#pragma once

// One parameter of a built-in device, for the controls that show and edit it
// (DeviceParamKnob, the device editors' knobs, buttons and boxes): what it is
// (the processor's description, EngineBridge::deviceParams), its value as it is
// now (its envelope's while automation plays: refreshed as the playhead moves),
// its automation state ("on" red dot, "off" grey: overridden), and what the old
// device frame's parameter cell did with it: setting it through the editor
// (undoably, one step per gesture key), touching it (the arrangement shows its
// automation), and its right-click menu's actions (Show Automation, Delete
// Automation, Re-Enable Automation, and in a rack Map to Macro / Unmap).
//
//   DeviceParam { id: threshold; session: Session; trackId: ...; deviceId: ...; paramId: "threshold" }
//   Knob { value: threshold.value; onMoved: (v, key) => threshold.set(v, key) }

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <optional>

#include "audio/BridgeTypes.h"
#include "session/Session.h"

namespace sub::ui {

class DeviceParam : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY targetChanged)
    Q_PROPERTY(QString trackId READ trackId WRITE setTrackId NOTIFY targetChanged)
    Q_PROPERTY(QString deviceId READ deviceId WRITE setDeviceId NOTIFY targetChanged)
    Q_PROPERTY(QString paramId READ paramId WRITE setParamId NOTIFY targetChanged)
    Q_PROPERTY(QString key READ key NOTIFY targetChanged)  // its automation key
    // Its description: none while the device (or its processor) isn't there.
    Q_PROPERTY(bool valid READ valid NOTIFY specChanged)
    Q_PROPERTY(QString name READ name NOTIFY specChanged)
    Q_PROPERTY(QString unit READ unit NOTIFY specChanged)
    Q_PROPERTY(double minimum READ minimum NOTIFY specChanged)
    Q_PROPERTY(double maximum READ maximum NOTIFY specChanged)
    Q_PROPERTY(double defaultValue READ defaultValue NOTIFY specChanged)
    Q_PROPERTY(bool logScale READ logScale NOTIFY specChanged)
    // A knob draws it from the middle: its range spans 0 and it has no unit, or semitones or cents.
    Q_PROPERTY(bool bipolar READ bipolar NOTIFY specChanged)
    Q_PROPERTY(int steps READ steps NOTIFY specChanged)  // > 0: whole values only
    Q_PROPERTY(QStringList labels READ labels NOTIFY specChanged)  // a list's names
    Q_PROPERTY(bool isList READ isList NOTIFY specChanged)
    // As it is now: its envelope's value while automation plays, else the device's own.
    Q_PROPERTY(double value READ value NOTIFY valueChanged)
    Q_PROPERTY(QString text READ text NOTIFY valueChanged)  // the value in its units
    Q_PROPERTY(int index READ index NOTIFY valueChanged)  // a list's: the value rounded
    Q_PROPERTY(QString automation READ automation NOTIFY automationChanged)  // "", "on" or "off"

public:
    explicit DeviceParam(QObject* parent = nullptr);

    sub::app::Session* session() const { return session_; }
    void setSession(sub::app::Session* session);
    QString trackId() const { return trackId_; }
    void setTrackId(const QString& trackId);
    QString deviceId() const { return deviceId_; }
    void setDeviceId(const QString& deviceId);
    QString paramId() const { return paramId_; }
    void setParamId(const QString& paramId);
    QString key() const;

    bool valid() const { return spec_.has_value(); }
    QString name() const;
    QString unit() const;
    double minimum() const;
    double maximum() const;
    double defaultValue() const;
    bool logScale() const;
    bool bipolar() const;
    int steps() const;
    QStringList labels() const;
    bool isList() const { return !labels().isEmpty(); }
    double value() const { return value_; }
    QString text() const { return format(value_); }
    int index() const;
    QString automation() const { return automation_; }

    // A value in its units ("-18.0 dB", a list's name).
    Q_INVOKABLE QString format(double value) const;
    // Typed text as a value (null if it doesn't read as one); a frequency may say "1.5k" or "2 kHz".
    Q_INVOKABLE QVariant parse(const QString& text) const;
    // Sets it through the editor: one undo step per `mergeKey` (empty: a step of its own).
    Q_INVOKABLE void set(double value, const QString& mergeKey = QString());
    // Taken hold of: the arrangement shows its automation.
    Q_INVOKABLE void touch();

    // The right-click menu.
    Q_INVOKABLE bool canAutomate() const;
    Q_INVOKABLE bool hasEnvelope() const;
    Q_INVOKABLE bool isOverridden() const;
    Q_INVOKABLE void showAutomation();
    Q_INVOKABLE void deleteAutomation();
    Q_INVOKABLE void reEnableAutomation();
    // The rack the device is in ("": on the track's own chain), whose macros can move it.
    Q_INVOKABLE QString rackId() const;
    // The macro it is mapped to (-1: none), and that macro's rack (the nearest one mapping it).
    Q_INVOKABLE int macro() const;
    Q_INVOKABLE QString macroRack() const;
    Q_INVOKABLE void mapToMacro(int index);
    Q_INVOKABLE void unmapFromMacro();

    // Reads it all again (the device view does when it rebuilds).
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void targetChanged();
    void specChanged();
    void valueChanged();
    void automationChanged();

private:
    void connectSession();
    void refreshSpec();
    void refreshValue();
    void refreshAutomation();

    QPointer<sub::app::Session> session_;
    QString trackId_;
    QString deviceId_;
    QString paramId_;
    std::optional<sub::app::ProcessorParam> spec_;
    double value_ = 0.0;
    QString automation_;
    QList<QMetaObject::Connection> connections_;
};

}  // namespace sub::ui
