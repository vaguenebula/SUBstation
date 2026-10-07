#pragma once
// The session: one open project and everything that works on it, as the main
// window had it. It owns the project, the undo stack, the editor, the
// selection, the engine bridge, the browser, the plug-in index and the sound
// similarity (which analyses the browser's files), and wires
// them together (the hooks and signals the main window used to connect). The
// UI reaches all of it through the session: QML as the `Session` singleton
// (registered by the UI), the UI's C++ items through these accessors.
//
// The session's actions (transport, files, the Edit and Create menus' commands
// that act on what is selected, renders in the background, preferences) are
// the main window's logic without its widgets: the UI shows dialogs and asks
// the session to act. What the main window showed in its status bar goes out
// on statusMessage (shown for statusTimeout ms); what were message boxes go
// out on warning (QMessageBox.warning) and information
// (QMessageBox.information), for the UI to show.
//
// Its parts, for QML:
// - arrangement: what acts on the arrangement's selection, with the clipboard
//   for clips, automation and tracks (ArrangementActions);
// - deviceSelection: the device view's selection, clipboard and actions
//   (DeviceSelection);
// - render: the render running in the background, for its modal dialog
//   (RenderProgress);
// - computerKeyboard: the computer MIDI keyboard (ComputerKeyboard);
// - audioPreferences, midiPreferences: Preferences › Audio and › MIDI
//   (Preferences › Plug-ins is the plug-in index's: plugins).
//
// Files: before New, Open, Open Recent and Quit, the UI asks "Save changes to
// the current project?" (confirmDiscardText) when the project isn't `clean`
// (Save, Discard, Cancel); on Save it calls saveProject() (or, for a project
// never saved, saveProjectAs() with a path it asked for) and goes on only if
// that returned true. The session itself never asks.
//
// Selection housekeeping (the arrangement view's, in the old code): the
// selection drops what no longer exists whenever tracks, clips or automation
// change (Selection::prune), and a parameter changed by hand (or taken hold
// of) shows its automation lane, if it can be automated.

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUndoStack>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "model/Device.h"  // PluginRef
#include "model/Track.h"   // Freeze

namespace sub {
class Engine;
}

namespace sub::app {

class ArrangementActions;
class AudioPreferences;
class BrowserController;
class ComputerKeyboard;
class DeviceSelection;
class EngineBridge;
class FreezeRender;
class MidiPreferences;
class PluginIndex;
class PresetIndex;
class Project;
class ProjectEditor;
class Render;
class RenderProgress;
class Selection;
class SoundSimilarity;

class Session : public QObject {
    Q_OBJECT
    Q_PROPERTY(sub::app::Project* project READ project CONSTANT)
    Q_PROPERTY(QUndoStack* undoStack READ undoStack CONSTANT)
    Q_PROPERTY(sub::app::ProjectEditor* editor READ editor CONSTANT)
    Q_PROPERTY(sub::app::Selection* selection READ selection CONSTANT)
    Q_PROPERTY(sub::app::EngineBridge* bridge READ bridge CONSTANT)
    Q_PROPERTY(sub::app::BrowserController* browser READ browser CONSTANT)
    Q_PROPERTY(sub::app::PluginIndex* plugins READ plugins CONSTANT)
    Q_PROPERTY(sub::app::SoundSimilarity* similarity READ similarity CONSTANT)
    Q_PROPERTY(sub::app::ArrangementActions* arrangement READ arrangement CONSTANT)
    Q_PROPERTY(sub::app::DeviceSelection* deviceSelection READ deviceSelection CONSTANT)
    Q_PROPERTY(sub::app::RenderProgress* render READ render CONSTANT)
    Q_PROPERTY(sub::app::ComputerKeyboard* computerKeyboard READ computerKeyboard CONSTANT)
    Q_PROPERTY(sub::app::AudioPreferences* audioPreferences READ audioPreferences CONSTANT)
    Q_PROPERTY(sub::app::MidiPreferences* midiPreferences READ midiPreferences CONSTANT)

    // The window's title: "<project name>[*] - SUBstation" (* while there are unsaved changes).
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    // No unsaved changes (the undo stack is at its clean state).
    Q_PROPERTY(bool clean READ clean NOTIFY cleanChanged)
    Q_PROPERTY(QString confirmDiscardText READ confirmDiscardText CONSTANT)
    // Projects opened or saved lately, the latest first (at most kMaxRecent).
    Q_PROPERTY(QStringList recentProjects READ recentProjects NOTIFY recentProjectsChanged)
    // Where the file dialogs start: the folder last opened from or saved to (else ~/Music).
    Q_PROPERTY(QString lastFolder READ lastFolder NOTIFY lastFolderChanged)
    // The file dialogs' filter: "SUBstation Project (*.gilproj)".
    Q_PROPERTY(QString projectFilter READ projectFilter CONSTANT)
    Q_PROPERTY(QString projectExtension READ projectExtension CONSTANT)

    // The count-in before recording, in bars (0, 1, 2 or 4; saved with the preferences).
    Q_PROPERTY(int countInBars READ countInBars WRITE setCountInBars NOTIFY countInBarsChanged)
    // [{label ("No Count-In", "Count-In 1 Bar"...), value: bars}]
    Q_PROPERTY(QVariantList countInChoices READ countInChoices CONSTANT)
    // Record quantization: where recorded MIDI notes start (beats; 0: as played).
    Q_PROPERTY(double recordQuantize READ recordQuantize WRITE setRecordQuantize NOTIFY recordQuantizeChanged)
    // [{label ("No Quantization", "1/16"...), value: beats}]
    Q_PROPERTY(QVariantList recordQuantizeChoices READ recordQuantizeChoices CONSTANT)
    // Whether automation was overridden somewhere (Re-Enable Automation is enabled, its button lit).
    Q_PROPERTY(bool automationOverridden READ automationOverridden NOTIFY automationOverriddenChanged)

    // A project's plug-ins loading after it opened (at the right of the status
    // bar): how many of how many, "Loading plug-ins: 1 of 3" (total 0: none).
    Q_PROPERTY(int pluginsLoaded READ pluginsLoaded NOTIFY pluginsLoadingChanged)
    Q_PROPERTY(int pluginsTotal READ pluginsTotal NOTIFY pluginsLoadingChanged)
    Q_PROPERTY(QString pluginsLoadingText READ pluginsLoadingText NOTIFY pluginsLoadingChanged)

    // Export Audio's choices: [{label, value: 16, 24 or 32}] and the default (24).
    Q_PROPERTY(QVariantList exportBitDepthChoices READ exportBitDepthChoices CONSTANT)
    Q_PROPERTY(int defaultExportBitDepth READ defaultExportBitDepth CONSTANT)

    // How long the status line shows a message (ms), as the main window did.
    Q_PROPERTY(int statusTimeout READ statusTimeout CONSTANT)
    // Help › About: its title and text (rich text).
    Q_PROPERTY(QString aboutTitle READ aboutTitle CONSTANT)
    Q_PROPERTY(QString aboutText READ aboutText CONSTANT)

public:
    struct Options {
        QString scanner;            // the plug-in scanner's executable; empty: next to the application
        bool scanPlugins = true;    // scan the plug-ins at once (tests turn it off)
        bool browserIndex = true;   // keep the browser's index and the sounds' fingerprints on disk (tests turn it off)
        bool analyseSounds = true;  // fingerprint the browser's files in the background, for Find Similar (tests turn it off)
    };

    static constexpr int kMaxRecent = 10;
    static constexpr int kStatusTimeoutMs = 8000;
    inline static const QString kRecentKey = QStringLiteral("files/recent");
    inline static const QString kLastFolderKey = QStringLiteral("files/last_dir");
    inline static const QString kCountInKey = QStringLiteral("transport/count_in_bars");

    explicit Session(sub::Engine& engine, QObject* parent = nullptr);
    Session(sub::Engine& engine, Options options, QObject* parent = nullptr);
    ~Session() override;

    sub::Engine& engine() const { return engine_; }
    Project* project() const { return project_; }
    QUndoStack* undoStack() const { return undoStack_; }
    ProjectEditor* editor() const { return editor_; }
    Selection* selection() const { return selection_; }
    EngineBridge* bridge() const { return bridge_; }
    BrowserController* browser() const { return browser_; }
    PluginIndex* plugins() const { return plugins_; }
    SoundSimilarity* similarity() const { return similarity_; }
    ArrangementActions* arrangement() const { return arrangement_; }
    DeviceSelection* deviceSelection() const { return devices_; }
    RenderProgress* render() const { return render_; }
    ComputerKeyboard* computerKeyboard() const { return keyboard_; }
    AudioPreferences* audioPreferences() const { return audioPreferences_; }
    MidiPreferences* midiPreferences() const { return midiPreferences_; }
    PresetIndex* presets() const { return presets_; }

    QString title() const;
    bool clean() const;
    QString confirmDiscardText() const { return QStringLiteral("Save changes to the current project?"); }
    QStringList recentProjects() const;
    QString lastFolder() const;
    QString projectFilter() const;
    QString projectExtension() const;
    int countInBars() const;
    void setCountInBars(int bars);
    QVariantList countInChoices() const;
    double recordQuantize() const;
    void setRecordQuantize(double grid);
    QVariantList recordQuantizeChoices() const;
    bool automationOverridden() const { return automationOverridden_; }
    int pluginsLoaded() const { return pluginsLoaded_; }
    int pluginsTotal() const { return pluginsTotal_; }
    QString pluginsLoadingText() const;
    QVariantList exportBitDepthChoices() const;
    int defaultExportBitDepth() const { return 24; }
    int statusTimeout() const { return kStatusTimeoutMs; }
    QString aboutTitle() const;
    QString aboutText() const;

    // The application's main window, which plug-in editors float above (a
    // native handle: an HWND on Windows; 0: none).
    void setOwnerWindow(std::function<uintptr_t()> ownerWindow);

    // After the UI shows: the audio device (and MIDI inputs, render threads) as
    // the preferences have them.
    void start();
    // Before the application ends: a render running stops, the device closes,
    // plug-ins unload (while the application is still whole), the browser's
    // threads stop.
    void shutdown();
    // The window is asked to close: false if a render was running (it is
    // cancelled now, and the window stays, as its dialog's Cancel); true: the
    // UI goes on (asking about unsaved changes first).
    Q_INVOKABLE bool requestClose();

    // --- Transport -----------------------------------------------------------------------
    // Space / the play button: plays from the insert marker; again: stops, and
    // goes back to where playback started (as Ableton does).
    Q_INVOKABLE void togglePlay();
    // F9 / the record button: records the armed tracks from the insert marker
    // (after the count-in) or, while playing, from where the playhead is;
    // again: stops recording (punch out: playing goes on).
    Q_INVOKABLE void toggleRecord();
    // The stop button: stops (back to where playback started), or, stopped,
    // goes to the start.
    Q_INVOKABLE void stop();
    // The insert marker and the playhead to `beat` (a click on the ruler...).
    Q_INVOKABLE void locate(double beat);
    // The count-in, in beats of the project's time signature.
    double countInBeats() const;

    // --- Create --------------------------------------------------------------------------
    // Ctrl+T, Ctrl+Shift+T: a track after the selected one (and what is in
    // it), in its group; it is selected. Its id.
    Q_INVOKABLE QString insertAudioTrack();
    Q_INVOKABLE QString insertMidiTrack();
    // Ctrl+Alt+T: a return track, after the others (or after the selected one); it is selected.
    Q_INVOKABLE QString insertReturnTrack();
    // Ctrl+Shift+D / Ctrl+Shift+M: a MIDI clip over the time selection on each
    // MIDI track in it, or at the insert marker (a bar long, on the grid of
    // `gridStep` beats: 0 when snapping is off) on the selected MIDI track;
    // opened in the piano roll (arrangement's clipViewRequested).
    Q_INVOKABLE void insertMidiClip(double gridStep = 0.0);
    // Ctrl+G: the selected tracks go into a new group, which is selected; in
    // the device view, the selected devices into a rack.
    Q_INVOKABLE void groupSelected();
    // Ctrl+Shift+G: the selected groups go, what was in them stays (in the
    // device view: the selected racks).
    Q_INVOKABLE void ungroupSelected();
    // Delete Selected Tracks: the selected tracks (and return tracks).
    Q_INVOKABLE void deleteSelectedTracks();

    // --- Adding to the selected track (the browser's double-click, Enter) -------------------
    // A built-in device `kind` on the selected track (or the master, a
    // return); an instrument with no MIDI track selected: on a new MIDI track.
    Q_INVOKABLE void addDeviceToSelectedTrack(const QString& kind);
    // The same with a plug-in (PluginRef fields: format, uid, name, vendor, path, instrument).
    Q_INVOKABLE void addPluginToSelectedTrack(const QVariantMap& plugin);
    // A preset's device, new, on the selected track (an instrument with no MIDI
    // track selected: on a new one).
    Q_INVOKABLE void addPresetToSelectedTrack(const QString& path);
    // An audio file at the insert marker, on the selected track (an audio track)
    // or a new one; selected.
    Q_INVOKABLE void addFileAtInsert(const QString& path);
    // The device view saved a preset to the library: the browser lists it.
    Q_INVOKABLE void presetSaved(const QString& path);

    // --- Edit ----------------------------------------------------------------------------
    // Ctrl+X, Ctrl+C, Ctrl+V, Ctrl+D: on the device view's selected devices
    // while it has the focus, else on the arrangement: a lane range's
    // automation, a clip range, or the selected tracks (whatIsCopied()).
    Q_INVOKABLE void cut();
    Q_INVOKABLE void copy();
    Q_INVOKABLE void paste();
    Q_INVOKABLE void duplicate();
    // What Cut and Copy act on now: "devices", "automation" (a lane range),
    // "clips" (a clip range), "tracks", or "" (nothing they can; for
    // breakpoints, returns and the master a statusMessage says why). `verb`:
    // "cut" or "copied".
    Q_INVOKABLE QString whatIsCopied(const QString& verb = QStringLiteral("copied"));
    // Delete (Backspace): the selected devices (device view), breakpoints, lane
    // range's automation, clip range, or tracks.
    Q_INVOKABLE void deleteSelection();
    // Ctrl+E: the selected clips (or those under the insert marker on the
    // selected track) split at the insert marker.
    Q_INVOKABLE void split();
    // Ctrl+A: the grid from the first clip's start to the last one's end, on every track.
    Q_INVOKABLE void selectAll();
    // Ctrl+J, R: the arrangement's consolidate() and reverseSelection().
    Q_INVOKABLE void consolidate();
    Q_INVOKABLE void reverseClips();
    // Ctrl+R: what to rename, for the UI to edit in place: the preset current
    // in the browser's list (if the list has the focus), the rack chain last
    // clicked (device view), or the track (return) last clicked:
    //   {kind: "preset", path, name}
    //   {kind: "chain", trackId, rackId, chainId, name}
    //   {kind: "track", trackId, name}
    // {} if none (a statusMessage says so). The UI then calls
    // browser.renamePreset, editor.renameChain or editor.renameTrack.
    Q_INVOKABLE QVariantMap renameTarget(bool browserListFocused = false);
    // S: solo the selected tracks (and unsolo the rest); if they all are already, unsolo every track.
    Q_INVOKABLE void soloSelectedTracks();
    // Ctrl+Shift+F: freezes the selected tracks (and returns), or unfreezes them if they all are.
    Q_INVOKABLE void toggleFreeze();
    // Flatten Track: the selected frozen tracks become audio tracks playing their frozen audio.
    Q_INVOKABLE void flattenSelectedTracks();
    // A track menu's freeze actions, for these tracks: {freezeText ("Freeze
    // Track(s)" or "Unfreeze Track(s)"), unfreeze (bool), freezeEnabled,
    // freezeToolTip (why not), flattenText, flattenEnabled}.
    Q_INVOKABLE QVariantMap freezeActions(const QStringList& trackIds) const;
    // Freezes these tracks (not those in a group frozen with them) in the
    // background, then in one undo step (cancelled: none); what can't be is
    // said through the editor's `refused`.
    Q_INVOKABLE void freezeTracks(const QStringList& trackIds);
    // Unfreezes these tracks, or the frozen groups they are in; those unfrozen.
    Q_INVOKABLE QStringList unfreezeTracks(const QStringList& trackIds);
    // Flattens the frozen audio and MIDI tracks among these (one undo step), and
    // selects them again; those flattened.
    Q_INVOKABLE QStringList flattenTracks(const QStringList& trackIds);

    // --- Files ---------------------------------------------------------------------------
    // New Project (the UI asked about unsaved changes first).
    Q_INVOKABLE void newProject();
    // Opens a project file (the UI asked about unsaved changes first; a file
    // that can't be read: warning). Whether it opened (projectOpened).
    Q_INVOKABLE bool openProject(const QString& path);
    // Ctrl+S: to the project's file; one never saved: saveAsRequested (the UI
    // asks where, then saveProjectAs), false meanwhile. Whether it was saved.
    Q_INVOKABLE bool saveProject();
    // Save As: to `path`.
    Q_INVOKABLE bool saveProjectAs(const QString& path);
    // Where Save As starts: "<last folder>/Untitled.gilproj".
    Q_INVOKABLE QString suggestedSavePath() const;
    // A recent project chosen: false if its file is gone (warning, and it
    // leaves the list); true: the UI goes on to openProject (asking first).
    Q_INVOKABLE bool recentProjectAvailable(const QString& path);
    // The Open Recent menu: [{path, label ("&1  song.gilproj", "&" doubled in
    // the name), toolTip}], the latest first; empty: "No Recent Projects".
    Q_INVOKABLE QVariantList recentMenuItems() const;
    Q_INVOKABLE void clearRecentProjects();

    // --- Export Audio -------------------------------------------------------------------
    // The ranges it can render: [{label, value: "arrangement" | "loop"}] (the
    // loop region only while the loop is on and has a length).
    Q_INVOKABLE QVariantList exportRangeChoices() const;
    // Why a range can't be exported ("There is nothing to export yet."), or "".
    Q_INVOKABLE QString exportProblem(const QString& range) const;
    // Where its file dialog starts: "<last folder>/<project name>.wav".
    Q_INVOKABLE QString suggestedExportPath() const;
    // Renders the range into `path` (WAV, `bitDepth`) in the background (its
    // progress in `render`), playback stopped first. False if it didn't start
    // (nothing to export: information; or a render runs).
    Q_INVOKABLE bool exportAudio(const QString& path, const QString& range, int bitDepth);

    // --- For the tests -------------------------------------------------------------------
    // How a track's render becomes its frozen audio (default EngineBridge::finishFreeze).
    void setFreezeFinisher(std::function<std::optional<Freeze>(FreezeRender&)> finisher);
    // Starts a render in the background: false (and it is deleted) if one runs.
    bool startRender(Render* render);
    bool rendering() const;

Q_SIGNALS:
    // For the status line: what the bridge, the browser, the plug-in index and the
    // editor (refused edits) have to say, and what the session's actions report.
    void statusMessage(const QString& message);
    void warning(const QString& message);      // a message box with a warning
    void information(const QString& message);  // a message box with information
    void saveAsRequested();                    // Ctrl+S on a project never saved: the UI asks where
    void projectOpened();                      // a project was opened: the arrangement shows all of it
    void titleChanged();
    void cleanChanged();
    void recentProjectsChanged();
    void lastFolderChanged();
    void countInBarsChanged();
    void recordQuantizeChanged();
    void automationOverriddenChanged();
    void pluginsLoadingChanged();

private:
    void wire();
    void resetSession();
    bool saveTo(const QString& path);
    void addRecent(const QString& path);
    void setRecent(const QStringList& paths);
    void setLastFolder(const QString& folder);
    struct Place {
        int index = -1;
        std::optional<QString> parent;
    };
    Place afterSelectedTrack() const;
    void addDevice(const QString& kind, const std::optional<PluginRef>& plugin);
    void freeze(const QStringList& trackIds, bool report);
    void pruneSelection();

    sub::Engine& engine_;
    Options options_;
    Project* project_ = nullptr;
    QUndoStack* undoStack_ = nullptr;
    ProjectEditor* editor_ = nullptr;
    Selection* selection_ = nullptr;
    EngineBridge* bridge_ = nullptr;
    PluginIndex* plugins_ = nullptr;
    SoundSimilarity* similarity_ = nullptr;
    BrowserController* browser_ = nullptr;
    PresetIndex* presets_ = nullptr;
    RenderProgress* render_ = nullptr;
    ArrangementActions* arrangement_ = nullptr;
    DeviceSelection* devices_ = nullptr;
    ComputerKeyboard* keyboard_ = nullptr;
    AudioPreferences* audioPreferences_ = nullptr;
    MidiPreferences* midiPreferences_ = nullptr;
    QPointer<Render> rendering_;
    std::function<std::optional<Freeze>(FreezeRender&)> finishFreeze_;
    double playStart_ = 0.0;
    bool automationOverridden_ = false;
    int pluginsLoaded_ = 0;
    int pluginsTotal_ = 0;
    bool shutDown_ = false;
};

}  // namespace sub::app
