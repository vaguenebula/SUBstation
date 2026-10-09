#pragma once

// A strip's header, a track's, a return's or the master's, laid out as
// Ableton's: the name column (its name bar in its colour, the fold button and
// its name; below, the automation choosers on a darker shade of its colour),
// the In/Out column (Audio From or MIDI From and its channel,
// monitoring, Audio To and where in that track it goes; a group's Audio To
// only) and the mixer column (activator, solo, arm; volume, pan; sends), and
// the meter. What it paints itself (the columns' backgrounds, the name column;
// the bands of the groups it is in are GroupBands', over the headers), its
// mouse handling (selecting, dragging to move tracks, resizing, folding,
// Alt+wheel), and what its QML controls show and do (menus too).
// TrackHeader.qml, ReturnHeader.qml and MasterHeader.qml lay the controls out
// over it, and tell it where the In/Out and mixer columns start:
//
//   TrackHeaderItem { session: Session; arrangement: arrangement; trackId: model.trackId; meter: meter
//       ioLeft: ...; mixerLeft: ...
//       Meter { id: meter } ... }
//
// The In/Out column, as Ableton's: Audio From is the audio device's inputs
// (Ext. In: its channel or pair below), the master's output (Resampling),
// another track's (a group's, a return's: below, where it is taken: Pre FX,
// Post FX or Post Mixer) or No Input; MIDI From every MIDI input (All Ins),
// one, the computer keyboard or none, and below its channel. Monitoring: In,
// Auto, Off. Audio To is Main (the master), its group, an audio track (below:
// its Track In) or a track with devices taking a sidechain (below: which, as
// "Sidechain-<device>"), or Sends Only; tracks it would close a cycle with are
// greyed out. Configure... asks for the preferences (Arrangement::preferencesRequested).
//
// A track's header (a group's too: no arm or input, they record nothing):
// - Click selects the track (Ctrl toggles, Shift selects the tracks from the
//   last one clicked); a plain click on one of several selected keeps them (to
//   drag them all) and selects just it on release if no drag followed. Drag:
//   moves the selected tracks (if it is one of them) between tracks, or onto a
//   group's header into it (Arrangement::dragTracks).
// - The bottom kResizeGrab px of its own lane resize it (not while folded).
// - The fold button (each click of a double-click counts): every one of the
//   dragged tracks takes the clicked one's new state.
// - Alt+wheel (over it or any control in it) resizes or folds it; the wheel
//   alone scrolls the headers.
// - Solo: soloing a track unsoloes the others, and unsoloing one unsoloes them
//   all, unless Ctrl is held; a selected track's solo acts on all the selected
//   tracks. Arm: the same rules; warns if the track has no input.
// - Volume and pan follow their automation while it plays (and show it when
//   taken hold of); on one of several selected tracks, every selected track
//   follows, by the same amount when dragged or wheeled, to the same value when
//   typed or reset.
// - Sends: one knob and letter per return while there are returns (the knob
//   moves as a volume fader does; typing a dB value works); greyed out where the
//   return feeds this strip. Right-click: Pre-Fader (the letter turns the
//   accent colour), Remove Send, Show Automation.
// - Automation choosers: for its own lane (device, parameter, +) and for each
//   lane below (device, parameter, −).

#include "arrangement/MenuEntries.h"
#include "arrangement/TrackLayout.h"
#include "controls/Meter.h"
#include "model/Track.h"
#include "sg/SgCanvas.h"
#include "session/Session.h"

#include <QColor>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <optional>

namespace sub::app {
class Project;
struct Track;
}  // namespace sub::app

namespace sub::ui {

class Arrangement;


class TrackHeaderItem : public SgCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(sub::ui::Arrangement* arrangement READ arrangement WRITE setArrangement NOTIFY arrangementChanged)
    // A track's (or a group's), a return's, or "master".
    Q_PROPERTY(QString trackId READ trackId WRITE setTrackId NOTIFY trackIdChanged)
    // The meter this header feeds (the bridge's meters).
    Q_PROPERTY(sub::ui::Meter* meter READ meter WRITE setMeter NOTIFY meterChanged)
    // Its place among the tracks, from 1: the activator shows it.
    Q_PROPERTY(int number READ number WRITE setNumber NOTIFY numberChanged)
    // Where the name ends, and the In/Out and mixer columns start: QML's layout says.
    Q_PROPERTY(qreal nameRight READ nameRight WRITE setNameRight NOTIFY nameRightChanged)
    Q_PROPERTY(qreal ioLeft READ ioLeft WRITE setIoLeft NOTIFY columnsChanged)
    Q_PROPERTY(qreal mixerLeft READ mixerLeft WRITE setMixerLeft NOTIFY columnsChanged)

    // "track" (audio or MIDI), "group", "return" or "master".
    Q_PROPERTY(QString kind READ kind NOTIFY changed)
    Q_PROPERTY(bool midi READ midi NOTIFY changed)
    Q_PROPERTY(bool records READ records NOTIFY changed)  // audio and MIDI tracks: arm, input, monitoring
    Q_PROPERTY(QString name READ name NOTIFY changed)
    // What renaming it starts from: its name template ("# Kick", the # its number).
    Q_PROPERTY(QString nameTemplate READ nameTemplate NOTIFY changed)
    Q_PROPERTY(QColor color READ color NOTIFY changed)
    // Muted as heard: its activator off (following its automation while it plays).
    Q_PROPERTY(bool mute READ mute NOTIFY mixerChanged)
    // How the activator shows its automation ("on", "off" when overridden, "").
    Q_PROPERTY(QString activatorAutomation READ activatorAutomation NOTIFY mixerChanged)
    Q_PROPERTY(bool solo READ solo NOTIFY changed)
    Q_PROPERTY(bool armed READ armed NOTIFY changed)
    Q_PROPERTY(bool selected READ selected NOTIFY selectedChanged)
    Q_PROPERTY(QString letter READ letter NOTIFY changed)  // a return's
    Q_PROPERTY(QString soloToolTip READ soloToolTip NOTIFY changed)
    // The In/Out column's choosers: what each shows ("" for an empty box: nothing to choose).
    Q_PROPERTY(QString inputText READ inputText NOTIFY changed)  // Audio From / MIDI From
    Q_PROPERTY(QString inputToolTip READ inputToolTip NOTIFY changed)
    Q_PROPERTY(QString inputChannelText READ inputChannelText NOTIFY changed)  // its channel, or tap
    Q_PROPERTY(QString inputChannelToolTip READ inputChannelToolTip NOTIFY changed)
    Q_PROPERTY(QString monitor READ monitor NOTIFY changed)  // "in", "auto" or "off"
    Q_PROPERTY(QString outputText READ outputText NOTIFY changed)  // Audio To
    Q_PROPERTY(QString outputToolTip READ outputToolTip NOTIFY changed)
    Q_PROPERTY(QString outputChannelText READ outputChannelText NOTIFY changed)  // Track In, Sidechain-...
    Q_PROPERTY(QString outputChannelToolTip READ outputChannelToolTip NOTIFY changed)
    // Volume (dB) and pan as heard: following their automation while it plays;
    // and how they show it ("on", "off" when overridden, "").
    Q_PROPERTY(double volume READ volume NOTIFY mixerChanged)
    Q_PROPERTY(QString volumeAutomation READ volumeAutomation NOTIFY mixerChanged)
    Q_PROPERTY(double pan READ pan NOTIFY mixerChanged)
    Q_PROPERTY(QString panAutomation READ panAutomation NOTIFY mixerChanged)
    // The master's Main Out: the audio device's outputs it plays on ("1/2").
    Q_PROPERTY(QString mainOutText READ mainOutText NOTIFY changed)
    // [{returnId, letter, value (0..1), automation, enabled, preFader, toolTip}], one per return.
    Q_PROPERTY(QVariantList sends READ sends NOTIFY sendsChanged)
    // [{lane (-1: its own), device, param}]: the automation choosers, while its automation shows.
    Q_PROPERTY(QVariantList choosers READ choosers NOTIFY choosersChanged)

    // Its row: its own lane's height, the lanes below it ([{index, key, top, height}],
    // tops from the header's top), how many groups it is in, folded, its automation showing.
    Q_PROPERTY(int mainHeight READ mainHeight NOTIFY rowChanged)
    Q_PROPERTY(QVariantList lanes READ lanes NOTIFY rowChanged)
    Q_PROPERTY(int depth READ depth NOTIFY rowChanged)
    Q_PROPERTY(int indent READ indent NOTIFY rowChanged)
    Q_PROPERTY(bool folded READ folded NOTIFY rowChanged)
    Q_PROPERTY(bool automationShown READ automationShown NOTIFY rowChanged)
    // Where the name starts (after the bands and the fold button).
    Q_PROPERTY(qreal nameLeft READ nameLeft NOTIFY changed)
    Q_PROPERTY(QRectF foldRect READ foldRect NOTIFY rowChanged)
    // Renaming in place: the name's text field shows (Ctrl+R, the menu's Rename).
    Q_PROPERTY(bool renaming READ renaming NOTIFY renamingChanged)

public:
    static constexpr int kResizeGrab = 4;
    static constexpr int kNameRow = 22;
    // The name row's buttons (fold, activator, solo, arm) and name: kNameButton
    // high, kNamePad below its top, so a folded track fits them evenly.
    static constexpr int kNamePad = 2;
    static constexpr int kNameButton = 16;
    static_assert(arrangement::kFoldedHeight == kNamePad + kNameButton + kNamePad + 1);  // (and the line below)
    static constexpr int kIndent = arrangement::kGroupIndent;  // per group a track is in: the group's colour band
    static constexpr int kFoldWidth = 14;
    static constexpr int kSendSlot = 36;  // a send knob and its letter, at most
    // The columns' rows: kRow apart from kNamePad, each control kNameButton high.
    static constexpr int kRow = 18;
    // A group's colour fills its name column to the choosers (two rows), a track's its name row.
    static constexpr int kGroupBlock = arrangement::kGroupBlock;
    static_assert(kGroupBlock == kNamePad + 2 * kRow);

    explicit TrackHeaderItem(QQuickItem* parent = nullptr);
    ~TrackHeaderItem() override;

    app::Session* session() const { return session_; }
    void setSession(app::Session* session);
    Arrangement* arrangement() const { return arrangement_; }
    void setArrangement(Arrangement* arrangement);
    QString trackId() const { return trackId_; }
    void setTrackId(const QString& trackId);
    Meter* meter() const { return meter_; }
    void setMeter(Meter* meter);
    int number() const { return number_; }
    void setNumber(int number);
    qreal nameRight() const { return nameRight_; }
    void setNameRight(qreal x);

    QString kind() const;
    bool midi() const;
    bool records() const;
    QString name() const;
    QString nameTemplate() const;
    QColor color() const;
    bool mute() const;
    bool solo() const;
    bool armed() const;
    bool selected() const;
    QString letter() const;
    QString soloToolTip() const;
    qreal ioLeft() const { return ioLeft_; }
    void setIoLeft(qreal x);
    qreal mixerLeft() const { return mixerLeft_; }
    void setMixerLeft(qreal x);
    QString inputText() const;
    QString inputToolTip() const;
    QString inputChannelText() const;
    QString inputChannelToolTip() const;
    QString monitor() const;
    QString outputText() const;
    QString outputToolTip() const;
    QString outputChannelText() const;
    QString outputChannelToolTip() const;
    // The name bar's colour (the track's), and the colour of what is written on it.
    QString mainOutText() const;
    QColor barColor() const;
    Q_INVOKABLE QColor barText() const;
    double volume() const { return volume_; }
    QString volumeAutomation() const { return volumeAutomation_; }
    double pan() const { return pan_; }
    QString panAutomation() const { return panAutomation_; }
    QString activatorAutomation() const { return activatorAutomation_; }
    QVariantList sends() const { return sends_; }
    QVariantList choosers() const { return choosers_; }
    int mainHeight() const { return row_.mainHeight; }
    QVariantList lanes() const;
    int depth() const { return depth_; }
    int indent() const { return depth_ * kIndent; }
    bool folded() const { return folded_; }
    bool automationShown() const { return automationShown_; }
    qreal nameLeft() const;
    QRectF foldRect() const;
    bool renaming() const { return renaming_; }
    // Whether volume, pan or a send follows its automation now.
    bool mixerAutomated() const;
    // The bottom edge of its own lane: resizes the track (not while folded).
    bool inResizeZone(double y) const;
    // What dragging this header moves: the selected tracks if it is one of them.
    QStringList draggedTracks() const;

    // --- What its controls do ------------------------------------------------------------

    // Volume (dB) or pan changed here (relative: dragged or wheeled, else typed or reset).
    Q_INVOKABLE void setVolume(double value, const QString& gestureKey, bool relative);
    Q_INVOKABLE void setPan(double value, const QString& gestureKey, bool relative);
    // Pressing a mixer control shows its automation (as in Ableton).
    Q_INVOKABLE void touchVolume();
    Q_INVOKABLE void touchPan();
    Q_INVOKABLE void activatorToggled(bool on);
    Q_INVOKABLE void soloClicked(bool on);
    Q_INVOKABLE void armClicked(bool on);
    // In, Auto or Off clicked ("in", "auto", "off").
    Q_INVOKABLE void setMonitor(const QString& mode);
    // What a monitoring button says when the mouse is over it.
    Q_INVOKABLE static QString monitorToolTip(const QString& mode, bool midi);
    Q_INVOKABLE void toggleFold();
    // A send knob turned (0..1, as a volume fader), or taken hold of.
    Q_INVOKABLE void setSend(const QString& returnId, double value, const QString& gestureKey);
    Q_INVOKABLE void touchSend(const QString& returnId);
    // A send level typed ("-6", "-6 dB"): its knob's value; null if it isn't a number.
    Q_INVOKABLE QVariant parseSendLevel(const QString& text) const;
    // The "+" (another lane below) and a lane's "−".
    Q_INVOKABLE void addLane();
    Q_INVOKABLE void removeLane(int lane);
    // Volume and pan as the controls show and read them ("-6.0 dB", "25L"; null: not a pan).
    Q_INVOKABLE static QString formatDb(double db);
    // The volume as Ableton's header shows it ("0", "-15.0", "-inf"), and how far its slider is (0..1).
    Q_INVOKABLE static QString formatVolume(double db);
    Q_INVOKABLE static double volumeFraction(double db);
    Q_INVOKABLE static QString formatPan(double pan);
    Q_INVOKABLE static QVariant parsePan(const QString& text);
    Q_INVOKABLE void startRename();
    // The text field's text: the new name (blank: no change). The field goes.
    Q_INVOKABLE void finishRename(const QString& text);

    // --- Menus (entries for ArrangementMenu.qml; triggerMenu runs the one chosen) ----------

    arrangement::MenuEntries contextMenu();
    arrangement::MenuEntries inputMenu();
    arrangement::MenuEntries inputChannelMenu();
    arrangement::MenuEntries outputMenu();
    arrangement::MenuEntries outputChannelMenu();
    arrangement::MenuEntries mainOutMenu();
    arrangement::MenuEntries sendMenu(const QString& returnId);
    arrangement::MenuEntries activatorMenu();
    arrangement::MenuEntries deviceMenu(int lane);
    arrangement::MenuEntries paramMenu(int lane);
    Q_INVOKABLE QVariantList inputMenuEntries() { return show(inputMenu()); }
    Q_INVOKABLE QVariantList inputChannelMenuEntries() { return show(inputChannelMenu()); }
    Q_INVOKABLE QVariantList outputMenuEntries() { return show(outputMenu()); }
    Q_INVOKABLE QVariantList outputChannelMenuEntries() { return show(outputChannelMenu()); }
    Q_INVOKABLE QVariantList mainOutMenuEntries() { return show(mainOutMenu()); }
    Q_INVOKABLE QVariantList sendMenuEntries(const QString& returnId) { return show(sendMenu(returnId)); }
    Q_INVOKABLE QVariantList activatorMenuEntries() { return show(activatorMenu()); }
    Q_INVOKABLE QVariantList deviceMenuEntries(int lane) { return show(deviceMenu(lane)); }
    Q_INVOKABLE QVariantList paramMenuEntries(int lane) { return show(paramMenu(lane)); }
    Q_INVOKABLE void triggerMenu(int id);

    // Refresh everything it shows from the model.
    void refresh();

Q_SIGNALS:
    void sessionChanged();
    void arrangementChanged();
    void trackIdChanged();
    void meterChanged();
    void numberChanged();
    void nameRightChanged();
    void columnsChanged();
    void changed();
    void selectedChanged();
    void mixerChanged();
    void sendsChanged();
    void choosersChanged();
    void rowChanged();
    void renamingChanged();
    // Show this menu (its right-click menu) at `pos` (item coordinates).
    void menuRequested(const QVariantList& entries, const QPointF& pos);

protected:
    void paint(SgPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void hoverEnterEvent(QHoverEvent* event) override { hoverMoveEvent(event); }
    void hoverMoveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    const app::Track* track() const;
    app::Project* project() const;
    bool isMaster() const;
    bool isReturn() const;
    void connectAll();
    void refreshRow();
    void refreshMixer();
    void refreshSends();
    void refreshChoosers();
    void followAutomation();
    void updateMeter();
    QVariantList show(const arrangement::MenuEntries& menu);
    void setMixer(const QString& field, double value, const QString& gestureKey, bool relative);
    // Solo or arm (the same rules): `apply` with the tracks, whether on, whether exclusive.
    QStringList clickedTracks() const;
    void addFreezeEntries(arrangement::MenuEntries& menu, const QStringList& trackIds);
    void addAutomationEntries(arrangement::MenuEntries& menu);
    // "Configure...": the preferences' Audio page (or the MIDI page).
    void addConfigure(arrangement::MenuEntries& menu, bool midi = false);
    // A track an output can go into, and how: (its Track In, the devices on it taking a sidechain).
    struct OutputTarget {
        const app::Track* track = nullptr;
        bool trackIn = false;
        QStringList devices;
    };
    std::vector<OutputTarget> outputTargets() const;
    OutputTarget outputTarget(const QString& trackId) const;
    // Where its output goes now, as a track (its group, or none: the master, nowhere).
    std::optional<QString> outputTrack() const;
    QString automationState(const QString& key) const;
    void chooseLane(int lane, const QString& key);
    QString laneKey(int lane) const;
    double columnY(const QPointF& pos) const;

    QPointer<app::Session> session_;
    QPointer<Arrangement> arrangement_;
    QString trackId_;
    QPointer<Meter> meter_;
    QList<QPointer<QObject>> connected_;
    int number_ = 1;
    qreal nameRight_ = 0;
    qreal ioLeft_ = 0;
    qreal mixerLeft_ = 0;
    arrangement::AutomationRows row_{app::kDefaultTrackHeight, {}};  // (a new track's height)
    int rowTop_ = 0;  // a track's row's top (content y), its lanes' tops are from
    int depth_ = 0;
    bool folded_ = false;
    bool automationShown_ = false;
    double volume_ = 0.0;
    double pan_ = 0.0;
    QString volumeAutomation_;
    QString panAutomation_;
    QString activatorAutomation_;
    bool mute_ = false;  // as heard (mute())
    QVariantList sends_;
    QVariantList choosers_;
    bool sendsAutomated_ = false;
    bool renaming_ = false;
    arrangement::MenuEntries menu_;
    // A press that may start dragging the track, a drag of it, or a resize: (y in the window, height).
    std::optional<QPointF> press_;
    bool dragging_ = false;
    std::optional<std::pair<double, int>> resize_;
};

}  // namespace sub::ui
