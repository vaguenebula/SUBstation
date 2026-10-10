#pragma once

// The base of the device editors' drawn items (the Compressor's reduction
// graph, the Delay's filter, the EQ's curve, the Sampler's waveform, the
// Sidechain's curve): a built-in device of a track, what DeviceWidget gave its
// editors.
//
// - value(): a parameter as it is now (its envelope's value while automation
//   plays), and automationState(); sync() is called (on the GUI thread) when
//   they may have changed: the device's parameters, its automation, the
//   playhead while any of its parameters follows automation, the device coming
//   or going. Subclasses read what they draw there into members; paint() only
//   reads those (see SgCanvas's threading note).
// - setParams() / setParam() go through the ProjectEditor (one undo step per
//   merge key, the first parameter's lane shown), touch() shows a parameter's
//   automation.
// - readDisplay(): what the device's processor published for its editor since
//   the last call (EngineBridge::readProcessorDisplay), a position kept per
//   processor and display. refreshDisplays() is called about 60 times a second
//   (DisplayClock), only while the item is visible.

#include "sg/SgCanvas.h"

#include <QHash>
#include <QMap>
#include <QPointF>
#include <QPointer>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <optional>
#include <utility>
#include <vector>

#include "input/DoubleClicks.h"
#include "model/OrderedMap.h"
#include "session/Session.h"

namespace sub::app {
struct Device;
}

namespace sub::ui {

class DeviceCanvas : public SgCanvas {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("DeviceCanvas is a base class for the device editors' items")
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY deviceChanged)
    Q_PROPERTY(QString trackId READ trackId WRITE setTrackId NOTIFY deviceChanged)
    Q_PROPERTY(QString deviceId READ deviceId WRITE setDeviceId NOTIFY deviceChanged)
    // Whether the device is there (it goes when deleted, or its track).
    Q_PROPERTY(bool alive READ alive NOTIFY aliveChanged)
    Q_PROPERTY(QString trackName READ trackName NOTIFY trackNameChanged)  // (a window's title)

public:
    explicit DeviceCanvas(QQuickItem* parent = nullptr);
    ~DeviceCanvas() override;

    sub::app::Session* session() const { return session_; }
    void setSession(sub::app::Session* session);
    QString trackId() const { return trackId_; }
    void setTrackId(const QString& trackId);
    QString deviceId() const { return deviceId_; }
    void setDeviceId(const QString& deviceId);
    bool alive() const { return alive_; }
    QString trackName() const { return trackName_; }

    // The device (null: gone). Valid until the project changes.
    const sub::app::Device* device() const;
    // A parameter as it is now: its automation's value while that plays, else
    // the device's, else its default.
    double value(const QString& paramId) const;
    // "on" while its automation plays, "off" when overridden, else "".
    QString automationState(const QString& paramId) const;
    double defaultValue(const QString& paramId) const { return defaults_.value(paramId, 0.0); }
    double sampleRate() const;

    // Several parameters at once: one undo step (one per merge key, while the same ones change).
    void setParams(const sub::app::OrderedMap<QString, double>& values, const QString& mergeKey = QString(),
                   const QString& text = QStringLiteral("Change Device Parameters"));
    void setParam(const QString& paramId, double value, const QString& mergeKey = QString());
    Q_INVOKABLE void touch(const QString& paramId);
    // One parameter, as setParams (the editors' own controls, with their undo texts).
    Q_INVOKABLE void setParamValue(const QString& paramId, double value, const QString& mergeKey = QString(),
                                   const QString& text = QStringLiteral("Change Device Parameters"));

    // The display's values since the last call (empty without any).
    std::vector<float> readDisplay(const QString& displayId);
    // The same with the absolute index of the first value.
    std::pair<qint64, std::vector<float>> readDisplayAt(const QString& displayId);

    // Reads everything again, as a change of the device would.
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void deviceChanged();
    void aliveChanged();
    void trackNameChanged();
    // The parameters (or their automation) changed: anything bound to them should read them again.
    void synced();

protected:
    // The device's parameters, automation or state may have changed: read what is drawn. The default repaints.
    virtual void sync();
    // Draws what the engine reported since; called as the meters update, while visible.
    virtual void refreshDisplays() {}
    // The device's state besides its parameters changed (a sampler's sample).
    virtual void stateChanged() { sync(); }

    void itemChange(ItemChange change, const ItemChangeData& data) override;
    // Whether a press is a double-click's second (Qt Quick delivers it before the double-click,
    // as widgets didn't): items whose first click changes what a double-click does skip it.
    bool secondPressOfDoubleClick(const QMouseEvent* event);

private:
    void connectSession();
    void readDevice();
    void readTrackName();
    void doSync();
    bool followsAutomation() const;

    QPointer<sub::app::Session> session_;
    QString trackId_;
    QString deviceId_;
    bool alive_ = false;
    QString trackName_;
    bool editing_ = false;  // setting parameters: synced once, when done
    QMap<QString, double> defaults_;  // param id -> its default
    QStringList automatable_;         // those that can follow automation
    bool following_ = false;          // any of them does now
    QMap<QString, int> displays_;     // display id -> index
    QHash<QString, quint64> positions_;  // "<processor>:<display>" -> where to read from
    QList<QMetaObject::Connection> connections_;
    DoubleClicks doubleClicks_;
};

}  // namespace sub::ui
