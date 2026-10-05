#pragma once

// A strip's header, a track's, a return's or the master's: what it paints
// itself (its background, a band in the colour of each group it is in, its own
// colour, the fold button, the frozen mark and its name), its mouse handling
// (selecting, dragging to move tracks, resizing, folding, Alt+wheel), and what
// its QML controls show and do (activator, solo, arm, volume, pan, input,
// monitoring, sends, meter, automation choosers, menus). TrackHeader.qml,
// ReturnHeader.qml and MasterHeader.qml lay the controls out over it:
//
//   TrackHeaderItem { session: Session; arrangement: arrangement; trackId: model.trackId; meter: meter
//       Meter { id: meter } ... }
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
#include "sg/SgCanvas.h"
#include "session/Session.h"

#include <QColor>
#include <QImage>
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
    // Where the name ends (the activator's left, less a gap): QML's layout says.
    Q_PROPERTY(qreal nameRight READ nameRight WRITE setNameRight NOTIFY nameRightChanged)

    // "track" (audio or MIDI), "group", "return" or "master".
    Q_PROPERTY(QString kind READ kind NOTIFY changed)
    Q_PROPERTY(bool midi READ midi NOTIFY changed)
    Q_PROPERTY(bool records READ records NOTIFY changed)  // audio and MIDI tracks: arm, input, monitoring
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(QColor color READ color NOTIFY changed)
    Q_PROPERTY(bool mute READ mute NOTIFY changed)
    Q_PROPERTY(bool solo READ solo NOTIFY changed)
    Q_PROPERTY(bool armed READ armed NOTIFY changed)
    Q_PROPERTY(bool selected READ selected NOTIFY selectedChanged)
    Q_PROPERTY(QString letter READ letter NOTIFY changed)  // a return's
    Q_PROPERTY(QString soloToolTip READ soloToolTip NOTIFY changed)
    Q_PROPERTY(QString inputText READ inputText NOTIFY changed)
    Q_PROPERTY(QString inputToolTip READ inputToolTip NOTIFY changed)
    Q_PROPERTY(QString monitorText READ monitorText NOTIFY changed)
    Q_PROPERTY(QString monitorToolTip READ monitorToolTip NOTIFY changed)
    // Volume (dB) and pan as heard: following their automation while it plays;
    // and how they show it ("on", "off" when overridden, "").
    Q_PROPERTY(double volume READ volume NOTIFY mixerChanged)
    Q_PROPERTY(QString volumeAutomation READ volumeAutomation NOTIFY mixerChanged)
    Q_PROPERTY(double pan READ pan NOTIFY mixerChanged)
    Q_PROPERTY(QString panAutomation READ panAutomation NOTIFY mixerChanged)
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
    // Where the name starts (after the bands, the fold button and the frozen mark).
    Q_PROPERTY(qreal nameLeft READ nameLeft NOTIFY changed)
    Q_PROPERTY(QRectF foldRect READ foldRect NOTIFY rowChanged)
    // Renaming in place: the name's text field shows (Ctrl+R, the menu's Rename).
    Q_PROPERTY(bool renaming READ renaming NOTIFY renamingChanged)

public:
    static constexpr int kResizeGrab = 4;
    static constexpr int kNameRow = 22;
    static constexpr int kIndent = 6;  // per group a track is in: the group's colour band
    static constexpr int kFoldWidth = 14;
    static constexpr int kSnowflake = 12;  // the frozen mark before a frozen track's name
    static constexpr int kChooserRow = kNameRow + 30;  // the choosers, below volume and pan (and the sends)
    static constexpr int kSendSlot = 36;  // a send knob and its letter, at most

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
    QColor color() const;
    bool mute() const;
    bool solo() const;
    bool armed() const;
    bool selected() const;
    QString letter() const;
    QString soloToolTip() const;
    QString inputText() const;
    QString inputToolTip() const;
    QString monitorText() const;
    QString monitorToolTip() const;
    double volume() const { return volume_; }
    QString volumeAutomation() const { return volumeAutomation_; }
    double pan() const { return pan_; }
    QString panAutomation() const { return panAutomation_; }
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
    Q_INVOKABLE static QString formatPan(double pan);
    Q_INVOKABLE static QVariant parsePan(const QString& text);
    Q_INVOKABLE void startRename();
    // The text field's text: the new name (blank: no change). The field goes.
    Q_INVOKABLE void finishRename(const QString& text);

    // --- Menus (entries for ArrangementMenu.qml; triggerMenu runs the one chosen) ----------

    arrangement::MenuEntries contextMenu();
    arrangement::MenuEntries inputMenu();
    arrangement::MenuEntries monitorMenu();
    arrangement::MenuEntries sendMenu(const QString& returnId);
    arrangement::MenuEntries deviceMenu(int lane);
    arrangement::MenuEntries paramMenu(int lane);
    Q_INVOKABLE QVariantList inputMenuEntries() { return show(inputMenu()); }
    Q_INVOKABLE QVariantList monitorMenuEntries() { return show(monitorMenu()); }
    Q_INVOKABLE QVariantList sendMenuEntries(const QString& returnId) { return show(sendMenu(returnId)); }
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
    void itemChange(ItemChange change, const ItemChangeData& value) override;

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
    arrangement::AutomationRows row_{80, {}};  // (a new track's height)
    int rowTop_ = 0;  // a track's row's top (content y), its lanes' tops are from
    int depth_ = 0;
    bool folded_ = false;
    bool automationShown_ = false;
    double volume_ = 0.0;
    double pan_ = 0.0;
    QString volumeAutomation_;
    QString panAutomation_;
    QVariantList sends_;
    QVariantList choosers_;
    bool sendsAutomated_ = false;
    bool renaming_ = false;
    QImage snowflake_;  // the frozen mark, drawn for the window's pixel ratio (on the GUI thread)
    arrangement::MenuEntries menu_;
    // A press that may start dragging the track, a drag of it, or a resize: (y in the window, height).
    std::optional<QPointF> press_;
    bool dragging_ = false;
    std::optional<std::pair<double, int>> resize_;
};

}  // namespace sub::ui
