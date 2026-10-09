#pragma once

// A device of the track the device view shows, as its frame and title bar show
// it: what it is called (a rack by its own name, a plug-in by its plug-in's),
// the title's tooltip (a plug-in's name, vendor, file and latency; a rack's
// name and latency), whether it is on (following its automation while that
// plays, as the switch's automation dot shows), folded (a rack: whether its chain list
// shows, and its chain's devices), an instrument, where it is in
// its chain (Move Left / Move Right), a plug-in's loading state (loaded,
// waiting to load, why it isn't), its editor window (open or not), and its
// sidechain: the button lit while it has one, the tooltip naming the source and
// where it is taken, and the menu (No Sidechain, the tracks, groups and returns
// it can come from, those that would close a cycle greyed out; then where it is
// taken along the source: Pre FX, after each of its effects (those in its racks
// too), Post FX, Post Mixer). It reads the project again whenever what it shows
// may have changed.
//
//   DeviceInfo { id: info; session: Session; trackId: ...; deviceId: ... }
//   Text { text: info.name }

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <optional>
#include <utility>
#include <vector>

#include "session/Session.h"

namespace sub::app {
struct Sidechain;
}

namespace sub::ui {

class DeviceInfo : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY targetChanged)
    Q_PROPERTY(QString trackId READ trackId WRITE setTrackId NOTIFY targetChanged)
    Q_PROPERTY(QString deviceId READ deviceId WRITE setDeviceId NOTIFY targetChanged)
    Q_PROPERTY(bool exists READ exists NOTIFY changed)
    Q_PROPERTY(QString kind READ kind NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)  // its title
    Q_PROPERTY(QString toolTip READ toolTip NOTIFY changed)  // its title's ("": none)
    Q_PROPERTY(bool isRack READ isRack NOTIFY changed)
    Q_PROPERTY(bool isPlugin READ isPlugin NOTIFY changed)
    Q_PROPERTY(bool instrument READ instrument NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)  // as heard: following its automation while it plays
    // How its on/off switch shows its automation: "on" while it plays, "off" when overridden, "".
    Q_PROPERTY(QString enabledAutomation READ enabledAutomation NOTIFY changed)
    Q_PROPERTY(bool folded READ folded NOTIFY changed)
    // A rack's chain list shows (hidden by default); its chain's devices show beside it (shown by default).
    Q_PROPERTY(bool chainListShown READ chainListShown NOTIFY changed)
    Q_PROPERTY(bool rackDevicesShown READ rackDevicesShown NOTIFY changed)
    Q_PROPERTY(QString chainId READ chainId NOTIFY changed)  // the chain it is in ("": the track's own)
    Q_PROPERTY(bool canMoveLeft READ canMoveLeft NOTIFY changed)
    Q_PROPERTY(bool canMoveRight READ canMoveRight NOTIFY changed)
    // Its processor is there (a plug-in loaded; a built-in device or rack made).
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    // A plug-in waiting to load (a project just opened: they load one by one).
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    // What a plug-in that isn't loaded shows instead of its parameters: "… is
    // loading…", why it failed, or "… is not loaded." ("": it is loaded).
    Q_PROPERTY(QString pluginMessage READ pluginMessage NOTIFY changed)
    Q_PROPERTY(bool editorOpen READ editorOpen NOTIFY changed)  // a plug-in's own editor shows
    Q_PROPERTY(bool hasSidechainInput READ hasSidechainInput NOTIFY changed)
    Q_PROPERTY(bool sidechainOn READ sidechainOn NOTIFY changed)
    Q_PROPERTY(QString sidechainToolTip READ sidechainToolTip NOTIFY changed)

public:
    explicit DeviceInfo(QObject* parent = nullptr);

    sub::app::Session* session() const { return session_; }
    void setSession(sub::app::Session* session);
    QString trackId() const { return trackId_; }
    void setTrackId(const QString& trackId);
    QString deviceId() const { return deviceId_; }
    void setDeviceId(const QString& deviceId);

    bool exists() const { return state_.exists; }
    QString kind() const { return state_.kind; }
    QString name() const { return state_.name; }
    QString toolTip() const { return state_.toolTip; }
    bool isRack() const { return state_.isRack; }
    bool isPlugin() const { return state_.isPlugin; }
    bool instrument() const { return state_.instrument; }
    bool enabled() const { return state_.enabled; }
    QString enabledAutomation() const { return state_.enabledAutomation; }
    bool folded() const { return state_.folded; }
    bool chainListShown() const { return state_.chainListShown; }
    bool rackDevicesShown() const { return state_.rackDevicesShown; }
    QString chainId() const { return state_.chainId; }
    bool canMoveLeft() const { return state_.canMoveLeft; }
    bool canMoveRight() const { return state_.canMoveRight; }
    bool loaded() const { return state_.loaded; }
    bool pending() const { return state_.pending; }
    QString pluginMessage() const { return state_.pluginMessage; }
    bool editorOpen() const { return state_.editorOpen; }
    bool hasSidechainInput() const { return state_.hasSidechainInput; }
    bool sidechainOn() const { return state_.sidechainOn; }
    QString sidechainToolTip() const { return state_.sidechainToolTip; }

    // The on/off switch. Switched while its automation plays, that stops (it is overridden).
    Q_INVOKABLE void setEnabled(bool enabled);
    // The switch's right-click menu: its automation shown, deleted, re-enabled
    // (while overridden). Whether it has an envelope.
    Q_INVOKABLE void showSwitchAutomation();
    Q_INVOKABLE void deleteSwitchAutomation();
    Q_INVOKABLE void reEnableAutomation();
    Q_INVOKABLE bool hasSwitchEnvelope() const;
    // Move Left / Move Right in its chain (an instrument stays first).
    Q_INVOKABLE void moveLeft();
    Q_INVOKABLE void moveRight();
    // A plug-in's own editor: shown (brought to the front if it is open) or closed.
    Q_INVOKABLE void showEditor(bool show = true);
    // What double-clicking the device does: a plug-in shows its own editor.
    Q_INVOKABLE void openEditor();

    // The sidechain menu's entries, in order: {text, checkable, checked,
    // enabled, source, tap}, {separator: true}, or {search: true, children}
    // (a search field over the tracks, entries as the others). Choosing one is
    // setSidechain(source, tap) (source "": no sidechain).
    Q_INVOKABLE QVariantList sidechainMenu() const;
    Q_INVOKABLE void setSidechain(const QString& sourceTrackId, const QString& tap);

    // Where a sidechain from a track can be taken, along its signal, as in
    // Ableton: before its devices, after each (in its racks' chains too, before
    // the rack: "After Rack › Chain › EQ"), after them all, after its fader
    // (label, tap). On a MIDI track, Pre FX is after the instrument; devices of
    // the same name in a chain are numbered.
    std::vector<std::pair<QString, QString>> tapChoices(const QString& sourceTrackId) const;
    // Where a sidechain is taken now: after a device that has left its source,
    // before the fader; after its instrument, before its effects.
    QString tapOf(const sub::app::Sidechain& sidechain) const;

    // Reads it all again.
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void targetChanged();
    void changed();

private:
    struct State {
        bool exists = false;
        QString kind;
        QString name;
        QString toolTip;
        bool isRack = false;
        bool isPlugin = false;
        bool instrument = false;
        bool enabled = true;
        QString enabledAutomation;
        bool folded = false;
        bool chainListShown = false;
        bool rackDevicesShown = true;
        QString chainId;
        bool canMoveLeft = false;
        bool canMoveRight = false;
        bool loaded = false;
        bool pending = false;
        QString pluginMessage;
        bool editorOpen = false;
        bool hasSidechainInput = false;
        bool sidechainOn = false;
        QString sidechainToolTip;

        bool operator==(const State&) const = default;
    };

    void connectSession();
    QString switchKey() const;
    // Whether it is on as heard (`own`: as the model has it), and how its switch shows its automation.
    std::pair<bool, QString> switchState(bool own) const;
    // Only the switch again, as its automation plays.
    void refreshSwitch();
    // The device's sidechain, unless its source is going (or gone).
    std::optional<sub::app::Sidechain> currentSidechain() const;

    QPointer<sub::app::Session> session_;
    QString trackId_;
    QString deviceId_;
    State state_;
    QString sidechainSource_;  // the track its sidechain comes from ("": none)
    QList<QMetaObject::Connection> connections_;
};

}  // namespace sub::ui
