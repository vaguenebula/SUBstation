#pragma once

// A plug-in device's parameters, for the device view's generic editor. They
// live in the plug-in (the engine), not in the model: PluginParams lists those
// a generic editor offers (what can be automated and isn't the plug-in's own
// business: neither hidden nor read-only; if that leaves none, every one that
// isn't hidden or read-only), by their index among the processor's parameters;
// PluginParam is one of them: what it is, its value as the plug-in has it now
// with the plug-in's own text for it, its automation state, and setting it
// through the editor with the value before (so undo can restore it, one step
// per gesture). Both follow the plug-in: loaded (or made again), its parameters
// rebuilt, its values changed by the plug-in itself, a preset loaded, an undo.
//
//   PluginParams { id: params; session: Session; trackId: ...; deviceId: ... }
//   PluginParam { session: Session; trackId: ...; deviceId: ...; index: params.indices[0] }

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <optional>

#include "audio/BridgeTypes.h"
#include "session/Session.h"

namespace sub::ui {

class PluginParams : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY targetChanged)
    Q_PROPERTY(QString trackId READ trackId WRITE setTrackId NOTIFY targetChanged)
    Q_PROPERTY(QString deviceId READ deviceId WRITE setDeviceId NOTIFY targetChanged)
    Q_PROPERTY(QList<int> indices READ indices NOTIFY indicesChanged)  // those shown, in order
    Q_PROPERTY(int count READ count NOTIFY indicesChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY indicesChanged)  // its processor is there

public:
    explicit PluginParams(QObject* parent = nullptr);

    sub::app::Session* session() const { return session_; }
    void setSession(sub::app::Session* session);
    QString trackId() const { return trackId_; }
    void setTrackId(const QString& trackId);
    QString deviceId() const { return deviceId_; }
    void setDeviceId(const QString& deviceId);
    QList<int> indices() const { return indices_; }
    int count() const { return int(indices_.size()); }
    bool loaded() const { return loaded_; }

    // What a generic editor shows of these parameters (their indices).
    static QList<int> shown(const QList<sub::app::ProcessorParam>& params);

    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void targetChanged();
    void indicesChanged();

private:
    QPointer<sub::app::Session> session_;
    QString trackId_;
    QString deviceId_;
    QList<int> indices_;
    bool loaded_ = false;
    QList<QMetaObject::Connection> connections_;
};

class PluginParam : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY targetChanged)
    Q_PROPERTY(QString trackId READ trackId WRITE setTrackId NOTIFY targetChanged)
    Q_PROPERTY(QString deviceId READ deviceId WRITE setDeviceId NOTIFY targetChanged)
    Q_PROPERTY(int index READ index WRITE setIndex NOTIFY targetChanged)  // among the processor's parameters
    Q_PROPERTY(bool valid READ valid NOTIFY specChanged)
    Q_PROPERTY(QString paramId READ paramId NOTIFY specChanged)
    Q_PROPERTY(QString name READ name NOTIFY specChanged)
    Q_PROPERTY(double minimum READ minimum NOTIFY specChanged)
    Q_PROPERTY(double maximum READ maximum NOTIFY specChanged)
    Q_PROPERTY(double defaultValue READ defaultValue NOTIFY specChanged)
    Q_PROPERTY(int steps READ steps NOTIFY specChanged)  // > 0: whole steps
    Q_PROPERTY(QStringList labels READ labels NOTIFY specChanged)
    Q_PROPERTY(bool isList READ isList NOTIFY specChanged)
    // A knob draws it from the middle: continuous, its default the middle of its range.
    Q_PROPERTY(bool bipolar READ bipolar NOTIFY specChanged)
    Q_PROPERTY(double value READ value NOTIFY valueChanged)  // as the plug-in has it now
    Q_PROPERTY(QString text READ text NOTIFY valueChanged)  // the plug-in's own text for it
    Q_PROPERTY(int listIndex READ listIndex NOTIFY valueChanged)
    Q_PROPERTY(QString automation READ automation NOTIFY automationChanged)  // "", "on" or "off"

public:
    explicit PluginParam(QObject* parent = nullptr);

    sub::app::Session* session() const { return session_; }
    void setSession(sub::app::Session* session);
    QString trackId() const { return trackId_; }
    void setTrackId(const QString& trackId);
    QString deviceId() const { return deviceId_; }
    void setDeviceId(const QString& deviceId);
    int index() const { return index_; }
    void setIndex(int index);

    bool valid() const { return spec_.has_value(); }
    QString paramId() const { return spec_ ? spec_->id : QString(); }
    QString name() const { return spec_ ? spec_->name : QString(); }
    double minimum() const { return spec_ ? spec_->minValue : 0.0; }
    double maximum() const { return spec_ ? spec_->maxValue : 1.0; }
    double defaultValue() const { return spec_ ? spec_->defaultValue : 0.0; }
    int steps() const { return spec_ ? spec_->steps : 0; }
    QStringList labels() const { return spec_ ? spec_->valueLabels : QStringList(); }
    bool isList() const { return !labels().isEmpty(); }
    bool bipolar() const;
    double value() const { return value_; }
    QString text() const { return text_; }
    int listIndex() const;
    QString automation() const { return automation_; }

    // The plug-in's text for a value of it.
    Q_INVOKABLE QString format(double value) const;
    // Sets it through the editor, with the value before: one undo step per `mergeKey`.
    Q_INVOKABLE void set(double value, const QString& mergeKey = QString());
    // Taken hold of: the arrangement shows its automation.
    Q_INVOKABLE void touch();
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void targetChanged();
    void specChanged();
    void valueChanged();
    void automationChanged();

private:
    void connectSession();
    void refreshValue();
    void refreshAutomation();

    QPointer<sub::app::Session> session_;
    QString trackId_;
    QString deviceId_;
    int index_ = -1;
    std::optional<sub::app::ProcessorParam> spec_;
    double value_ = 0.0;
    QString text_;
    QString automation_;
    QList<QMetaObject::Connection> connections_;
};

}  // namespace sub::ui
