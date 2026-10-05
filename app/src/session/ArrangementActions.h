#pragma once
// What acts on the arrangement's selection, without its widgets: the clipboard
// for clips, automation and tracks, and cutting, copying, pasting, duplicating,
// deleting, reversing and consolidating what is selected, inserting MIDI clips,
// and drops onto the lanes. The arrangement view calls it (a paste at a clicked
// beat on a clicked track, its context menus), and the session dispatches the
// Edit menu's commands to it.
//
// The clipboard holds the last thing copied or cut (Ctrl+C / Ctrl+X): clip
// content from a clip range (ClipboardContent), automation from a lane range
// (CopiedAutomation), or tracks (CopiedTracks; their plug-ins as they were).
// Ctrl+V pastes it. A cut that was refused (nothing taken out) leaves it as it was.
//
// Reversing (R) writes a reversed copy of each file once (as Ableton does);
// copies of less than kReverseInPlaceSeconds of audio in all are written at
// once, longer ones in the background, followed in the session's
// RenderProgress (Cancel makes none, and nothing is reversed).

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <optional>
#include <variant>

#include "editor/ClipRef.h"
#include "editor/ClipboardContent.h"
#include "editor/CopiedAutomation.h"
#include "editor/CopiedTracks.h"
#include "model/Project.h"  // LaneRef

namespace sub::app {

class EngineBridge;
class ProjectEditor;
class Render;
class RenderProgress;
class Selection;

class ArrangementActions : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasClipboard READ hasClipboard NOTIFY clipboardChanged)
    // What Ctrl+V would paste: "clips", "automation", "tracks" or "" (nothing).
    Q_PROPERTY(QString clipboardKind READ clipboardKind NOTIFY clipboardChanged)

public:
    static constexpr double kReverseInPlaceSeconds = 30.0;  // less audio than this reverses at once: no progress dialog

    using Clipboard = std::variant<std::monostate, ClipboardContent, CopiedAutomation, CopiedTracks>;
    // How a render in the background starts (the session's: one at a time).
    // False: it couldn't (one runs), and it is deleted.
    using RenderStarter = std::function<bool(Render*)>;

    ArrangementActions(ProjectEditor* editor, Selection* selection, EngineBridge* bridge, RenderProgress* progress,
                       QObject* parent = nullptr);

    bool hasClipboard() const { return !std::holds_alternative<std::monostate>(clipboard_); }
    QString clipboardKind() const;
    const Clipboard& clipboard() const { return clipboard_; }
    void setClipboard(Clipboard clipboard);
    void setRenderStarter(RenderStarter starter) { startRender_ = std::move(starter); }
    // Less audio than this (seconds, all files together) reverses at once (the tests make it 0).
    void setReverseInPlaceSeconds(double seconds) { reverseInPlaceSeconds_ = seconds; }

    // --- The selected area ---
    // Delete: what is in the selected clip range (its stretch of the clips, and
    // the automation under it); the (now empty) area stays selected.
    Q_INVOKABLE void deleteArea();
    // Ctrl+D: the selected clip range (clips and automation) copied to right
    // after it; the copy is selected.
    Q_INVOKABLE void duplicateArea();
    // Ctrl+C: what is in the selected clip range: its clips, and the automation under them.
    Q_INVOKABLE void copyArea();
    // Ctrl+X: copied, and taken out; the (now empty) area stays selected.
    Q_INVOKABLE void cutArea();
    // Ctrl+C / Ctrl+X on a lane range: its automation.
    Q_INVOKABLE void copyAutomation();
    Q_INVOKABLE void cutAutomation();
    // Ctrl+C / Ctrl+X on tracks (a group with what is in it), their plug-ins as they are now.
    Q_INVOKABLE void copyTracks(const QStringList& trackIds);
    Q_INVOKABLE void cutTracks(const QStringList& trackIds);
    // Ctrl+D on tracks: copies of them (groups with what is in them; see
    // ProjectEditor::duplicateTracks), their plug-ins as they are now. The
    // copies are selected; their ids.
    Q_INVOKABLE QStringList duplicateTracks(const QStringList& trackIds);
    // Ctrl+V: the clipboard at the insert marker, its top track onto the
    // selected track; copied automation onto the selected lanes (else those it
    // came from); copied tracks after the selected track. What is pasted is
    // selected, and the insert marker goes to its end (pasting again appends).
    Q_INVOKABLE void paste();
    // The same at `atBeat` (a clip range or automation), onto `trackId` ("": the
    // selected track): tracks after it.
    Q_INVOKABLE void paste(double atBeat, const QString& trackId = {});
    // An automation lane's menu's Paste: copied automation at the insert marker,
    // onto the selected lanes if this one is one of them, else onto this one.
    Q_INVOKABLE void pasteAutomationAt(const QString& owner, const QString& key);
    // R: the audio clips in the selected area are reversed (the stretch of each
    // inside it plays its file backwards); reversing them again goes back to
    // their files.
    Q_INVOKABLE void reverseSelection();
    // Ctrl+J: the selected MIDI clips on each track joined into one; selected.
    Q_INVOKABLE void consolidate();
    // A clip's menu's Split Here: the selected clips at `beat`.
    Q_INVOKABLE void splitAt(double beat);
    // Whether the menu's Consolidate and Reverse can act on the selection.
    Q_INVOKABLE bool canConsolidate() const;
    Q_INVOKABLE bool canReverse() const;

    // A new MIDI clip on a MIDI track, selected and opened (clipViewRequested):
    // over the time selection (on each of its MIDI tracks) if `beat` is inside
    // it, else at `beat` (see ProjectEditor::midiClipSpan, on the grid of
    // `gridStep` beats: 0 when snapping is off). {trackId, clipId}; {} for none.
    Q_INVOKABLE QVariantMap insertMidiClip(const QString& trackId, double beat, double gridStep = 0.0);

    // The lanes' menu's Insert Audio Track / Insert MIDI Track: after the track
    // under the mouse (and what is in it), in its group ("": last). Its id.
    Q_INVOKABLE QString insertTrackAfter(const QString& trackId, bool midi);

    // --- Drops onto the lanes ---
    // Audio files: their lengths (for the drop's preview): [{path, name, duration}] (unreadable ones left out).
    Q_INVOKABLE QVariantList dropSources(const QStringList& paths);
    // Audio files dropped at `beat`, one after another: on `trackId` if it is an
    // audio track, else on a new track ("": below the tracks). Selected.
    Q_INVOKABLE void dropFiles(const QStringList& paths, const QString& trackId, double beat);
    // Built-in devices and plug-ins (PluginRef fields) dropped on a track (or
    // the master, a return), or below the tracks ("": an instrument among them
    // makes a new MIDI track, the effects go on it). The track is selected.
    // Whether anything went in.
    Q_INVOKABLE bool dropDevices(const QStringList& kinds, const QVariantList& plugins, const QString& trackId);
    // Presets dropped on a track, or below the tracks (an instrument preset
    // makes a MIDI track, the others go on it). Whether any went in.
    Q_INVOKABLE bool dropPresets(const QStringList& paths, const QString& trackId);
    // Devices dragged from a track's chain (the device view) onto another
    // track: moved there (with their automation), and that track selected.
    Q_INVOKABLE bool dropMovedDevices(const QString& fromTrackId, const QStringList& deviceIds, const QString& toTrackId);

    // Asks the arrangement to open clips in the clip view (or piano roll),
    // `lead` first (clipViewRequested).
    void requestClipView(const ClipRefs& refs, const ClipRef& lead);

Q_SIGNALS:
    void clipboardChanged();
    void statusMessage(const QString& message);
    // Open these clips ([{trackId, clipId}]) in the clip view, `leadTrackId`/`leadClipId` first.
    void clipViewRequested(const QVariantList& refs, const QString& leadTrackId, const QString& leadClipId);

private:
    void pasteTracks(const CopiedTracks& content, QString after);
    void pasteAutomation(const CopiedAutomation& content, double atBeat, std::optional<QList<LaneRef>> lanes = {});
    void reverseWith(double start, double end, const QStringList& trackIds,
                     const QMap<QString, std::pair<QString, double>>& reversed);

    ProjectEditor* editor_;
    Project* project_;
    Selection* selection_;
    EngineBridge* bridge_;
    RenderProgress* progress_;
    RenderStarter startRender_;
    Clipboard clipboard_;
    double reverseInPlaceSeconds_ = kReverseInPlaceSeconds;
};

}  // namespace sub::app
