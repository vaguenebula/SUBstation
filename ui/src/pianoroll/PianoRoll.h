#pragma once

// The piano roll: the clip view of MIDI clips, laid out like Ableton's MIDI
// editor. It holds the clips it shows, its own time axis (zoom, scroll) and
// row height, the selected notes, the playhead, the key sounding, the notes
// copied, and the note tools' settings and actions. The items that draw it
// (NoteGrid, PianoKeys, PianoRuler, VelocityLane, RollPlayhead) share one;
// PianoRollView.qml lays them out, with the note tools (NoteTools.qml)
// floating over the notes.
//
// It shows one clip, or several at once (MIDI clips selected together, on one
// track or several): every one's notes, in its track's colour, edited
// together. The first is the lead: Generate writes into it, and new notes go
// into it unless another clip plays where they are drawn. A note stays in its
// own clip whatever is done to it. Notes are ClipNotes (NoteSet.h): the clip
// (its index in clips()) and the note in that clip's content beats.
//
// Times on the roll ("roll beats"): one clip shows in its content beats, the
// ruler's 1 being the clip's first content beat, not the arrangement's;
// several show in arrangement beats (each clip where it is in the song).
// shift(clip) converts a clip's content beats to roll beats, origin() roll
// beats to the arrangement's. The parts the clips play (each one's offsetBeats
// to windowEnd) are lit, the rest dimmed; notes outside them are kept:
// trimming a clip hides notes, never deletes them. Pitch runs 127 at the top
// to 0 at the bottom, a row of rowHeight pixels each.
//
// The song's harmony (Session.harmony) shows over the notes while it is shown
// (C): the chords the song plays over the part the clips play (the ChordLane
// along the grid's top), and notes out of the key tinted red. Both are worked
// out on the GUI thread when the harmony changes (chords(), scaleKey()), for
// the items to draw. Generate writes block chords or a bass line from those
// chords into the lead clip (from a progression in the key where the song has
// none).
//
// Every edit goes through commitNotes() (commit(): the lead clip's): the new
// list of notes of each clip it changes, one undo command, merged with the
// previous one when they share a merge key. Gestures pass their own key, so a
// drag is one undo step, while the model (and what plays) changes live as the
// mouse moves. clipAt() looks a clip up in the project each time: the project
// replaces it on every edit.
//
// A rubber band drawn over the notes selects a stretch of time too (snapped to
// the grid, at least as long as the notes in it): Ctrl+D copies the selected
// notes by its length (right after it, as Ableton duplicates a time
// selection), and so do Ctrl+C / Ctrl+V. Clicking an empty part of the grid
// places the paste marker there (pasteBeat(): a dashed line of its own, apart
// from the start marker and the playhead, which it doesn't move); Ctrl+V
// pastes there, the first copied note (or the stretch they were copied with)
// at the marker, each into the clip of its track that plays there.
//
// Where each clip's content beats are on the roll (origin(), shift()) is
// worked out on refresh() (every change of the clips shown), not looked up for
// each note: drawing and hit-testing ask for it note by note.

#include "editor/ClipRef.h"
#include "model/Clip.h"
#include "model/Keys.h"
#include "model/Notes.h"
#include "pianoroll/NoteSet.h"
#include "timeline/Timeline.h"
#include "session/Session.h"

#include <QColor>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QRandomGenerator>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace sub::app {
class EngineBridge;
class Harmony;
class Project;
class ProjectEditor;
class Selection;
}  // namespace sub::app

namespace sub::ui {

class NoteGrid;

class PianoRoll : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(QString trackId READ trackId NOTIFY clipChanged)
    Q_PROPERTY(QString clipId READ clipId NOTIFY clipChanged)
    Q_PROPERTY(int clipCount READ clipCount NOTIFY clipChanged)
    Q_PROPERTY(bool hasClip READ hasClip NOTIFY contentChanged)
    // The headphones button: notes clicked, added, moved and selected are heard.
    Q_PROPERTY(bool preview READ preview WRITE setPreview NOTIFY previewChanged)
    Q_PROPERTY(double pxPerBeat READ pxPerBeat NOTIFY viewChanged)
    Q_PROPERTY(double scrollBeats READ scrollBeats NOTIFY viewChanged)
    Q_PROPERTY(int scrollY READ scrollY NOTIFY vscrollChanged)
    Q_PROPERTY(int rowHeight READ rowHeight NOTIFY vscrollChanged)
    // The scroll bars, in pixels: how far the content reaches (the bar's range
    // plus its page), where the view is, and the page (the notes' width, height).
    Q_PROPERTY(double hScrollTotal READ hScrollTotal NOTIFY scrollBarsChanged)
    Q_PROPERTY(double hScrollValue READ hScrollValue NOTIFY scrollBarsChanged)
    Q_PROPERTY(double hScrollPage READ hScrollPage NOTIFY scrollBarsChanged)
    Q_PROPERTY(double vScrollTotal READ vScrollTotal NOTIFY scrollBarsChanged)
    Q_PROPERTY(double vScrollValue READ vScrollValue NOTIFY scrollBarsChanged)
    Q_PROPERTY(double vScrollPage READ vScrollPage NOTIFY scrollBarsChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    // The note tools: the grids Quantize offers (QUANTIZE_GRIDS), the one
    // chosen, Quantize's amount and Humanize's two (Velocity's, Timing's) in
    // percent.
    Q_PROPERTY(QStringList quantizeGrids READ quantizeGrids CONSTANT)
    Q_PROPERTY(QString quantizeGrid READ quantizeGrid WRITE setQuantizeGrid NOTIFY toolSettingsChanged)
    Q_PROPERTY(double quantizeAmount READ quantizeAmount WRITE setQuantizeAmount NOTIFY toolSettingsChanged)
    Q_PROPERTY(double humanizeVelocityAmount READ humanizeVelocityAmount WRITE setHumanizeVelocityAmount
                   NOTIFY toolSettingsChanged)
    Q_PROPERTY(double humanizeTimingAmount READ humanizeTimingAmount WRITE setHumanizeTimingAmount
                   NOTIFY toolSettingsChanged)
    // At 100 %, how far Humanize › Timing moves a note (beats).
    Q_PROPERTY(double humanizeBeats READ humanizeBeats CONSTANT)
    // Whether the velocity model is there (Humanize › Velocity is offered).
    Q_PROPERTY(bool velocityModelAvailable READ velocityModelAvailable NOTIFY sessionChanged)
    // Where the note tools are heading (shown or gone), and while shown, the
    // selected notes' bounding rectangle (in the note grid's coordinates) and
    // how many there are: the bar floats by them.
    Q_PROPERTY(bool toolsShown READ toolsShown NOTIFY toolsChanged)
    Q_PROPERTY(QRectF toolsArea READ toolsArea NOTIFY toolsChanged)
    Q_PROPERTY(int toolsCount READ toolsCount NOTIFY toolsChanged)
    Q_PROPERTY(bool hasCopiedNotes READ hasCopiedNotes NOTIFY copiedChanged)

public:
    static constexpr int kKeysWidth = 64;
    static constexpr int kRulerHeight = 24;
    static constexpr int kRowHeight = 12;
    static constexpr int kMinRowHeight = 5;
    static constexpr int kMaxRowHeight = 36;
    static constexpr double kRowHeightStep = 1.5;  // pixels per wheel notch (Alt+wheel)
    static constexpr int kDefaultPitch = 60;       // C3: centred for a clip without notes
    static constexpr int kPreviewVelocity = 100;
    static constexpr int kChordPreviewMs = 400;  // how long notes selected with a rubber band sound
    static constexpr const char* kDefaultGrid = "1/16";
    static constexpr double kDefaultHumanizeVelocity = 100.0;  // %: the model's velocities
    static constexpr double kDefaultHumanizeTiming = 25.0;     // %

    // A stretch of roll beats.
    using Span = std::pair<double, double>;

    explicit PianoRoll(QObject* parent = nullptr);
    ~PianoRoll() override;

    app::Session* session() const { return session_; }
    void setSession(app::Session* session);
    app::Project* project() const;
    app::ProjectEditor* editor() const;
    app::EngineBridge* bridge() const;
    app::Selection* selection() const;
    app::Harmony* harmony() const;

    // --- The clips ---------------------------------------------------------------

    // Show MIDI clips, the first leading (fitted to the view the first time), or nothing.
    void setClips(const app::ClipRefs& refs);
    // Show one MIDI clip, or nothing ("").
    Q_INVOKABLE void setClip(const QString& trackId, const QString& clipId);
    const app::ClipRefs& clipRefs() const { return clips_; }
    int clipCount() const { return static_cast<int>(clips_.size()); }
    // The lead clip's.
    QString trackId() const { return clips_.isEmpty() ? QString() : clips_.front().trackId; }
    QString clipId() const { return clips_.isEmpty() ? QString() : clips_.front().clipId; }
    // A clip shown, looked up now (null: none, or gone); clip(): the lead.
    const app::Clip* clipAt(int index) const;
    const app::Clip* clip() const { return clipAt(0); }
    bool hasClip() const { return clip() != nullptr; }
    // A clip's track's colour (the accent without a clip); trackColor(): the lead's.
    QColor colorOf(int index) const;
    QColor trackColor() const { return colorOf(0); }
    // The arrangement beat at roll beat 0, and the roll beat of a clip's content beat 0.
    double origin() const { return origin_; }
    double shift(int index) const {
        return index >= 0 && index < static_cast<int>(shifts_.size()) ? shifts_[static_cast<size_t>(index)] : 0.0;
    }
    // Where a note is on the roll.
    double rollStart(const ClipNote& note) const { return note.note.start + shift(note.clip); }
    // The parts the clips play, in roll beats (each clip's; they may overlap).
    std::vector<Span> windows() const;
    // Every shown clip's notes, the lead's last (drawn on top).
    std::vector<ClipNote> allNotes() const;
    // Calls `visit(clip, note)` for each of them, in that order, without copying them
    // (painting, hit-testing and drags go through every note).
    template <typename Visit>
    void forEachNote(Visit&& visit) const {
        for (int i = 1; i <= clipCount(); ++i) {
            const int index = i % clipCount();  // the lead (0) last
            if (const app::Clip* c = clipAt(index)) {
                for (const app::Note& note : c->notes) visit(index, note);
            }
        }
    }
    // The clip a note drawn at roll beat `beat` goes into: the lead if it plays
    // there, else the first that does, else the lead. On `trackId` (if given):
    // the first clip of that track playing there, else none.
    std::optional<int> clipFor(double beat, const QString& trackId = {}) const;
    // After the clips changed (edits, undo): forget selected notes that are gone, repaint.
    void refresh();
    // Make these the clips' notes (clip index -> its notes; one undo step per
    // `mergeKey`) and select `selected`.
    void commitNotes(const std::map<int, std::vector<app::Note>>& notes, const QString& text,
                     const QString& mergeKey = {}, const std::optional<std::vector<ClipNote>>& selected = std::nullopt);
    // The same for the lead clip alone.
    void commit(const std::vector<app::Note>& clipNotes, const QString& text, const QString& mergeKey = {},
                const std::optional<std::vector<app::Note>>& selected = std::nullopt);

    // --- Selected notes -----------------------------------------------------------

    // Select `notes`; `tools` brings up the note tools by them. Forgets the
    // selected stretch of time (setSelectedSpan).
    void selectNotes(const std::vector<ClipNote>& notes, bool tools = false);
    // The same with notes of the lead clip.
    void setSelection(const std::vector<app::Note>& notes, bool tools = false);
    // The selected notes, sorted (a set: no two alike).
    const std::vector<ClipNote>& selectedNotes() const { return selected_; }
    // ... as notes (with one clip shown: the lead's).
    std::vector<app::Note> selected() const;
    bool isSelected(const ClipNote& note) const;
    bool isSelected(const app::Note& note) const { return isSelected(ClipNote{0, note}); }
    int selectedCount() const { return static_cast<int>(selected_.size()); }
    // The stretch of time a rubber band selected with the notes, if one did.
    const std::optional<Span>& selectedSpan() const { return span_; }
    void setSelectedSpan(const std::optional<Span>& span);
    // The note tools show for a group chosen by a rubber band or Ctrl+A, not for
    // notes clicked or drawn; they stay while that group is edited.
    bool toolsWanted() const { return toolsWanted_; }
    void setToolsWanted(bool wanted) { toolsWanted_ = wanted; }

    // --- Editing the selection -----------------------------------------------------

    // Ctrl+D: copies of the selected notes, by the selected stretch's length
    // (right after it), or, without one, right after the last of them. Selected.
    void duplicateSelected();
    // Ctrl+C / Ctrl+X: the selected notes copied (cut: and taken out).
    void copySelected();
    void cutSelected();
    // Ctrl+V: the notes copied, at the paste marker (else right after where
    // they were copied from), each into the clip of its track that plays there
    // (else the one there, else the lead); selected, and the paste marker goes
    // to their end (pasting again appends).
    void paste();
    bool hasCopiedNotes() const { return !copied_.notes.empty(); }
    // 0: the selected notes deactivated, or activated again if they all are.
    void toggleSelectedActive();

    // --- Note tools ---------------------------------------------------------------

    // Show the note tools by the selected notes when they were chosen as a
    // group, or hide them: with nothing selected, and while a drag is moving
    // notes or drawing a rubber band.
    void placeTools();
    bool toolsShown() const { return toolsShown_; }
    QRectF toolsArea() const { return toolsArea_; }
    int toolsCount() const { return toolsCount_; }
    // What the tools act on: the selected notes, or all of them if none are
    // (Ctrl+U with nothing selected).
    std::vector<ClipNote> toolTargets() const;
    Q_INVOKABLE void legato();
    // ×2 (2.0) and ÷2 (0.5).
    Q_INVOKABLE void scaleTime(double factor);
    Q_INVOKABLE void quantize();
    // Humanize › Velocity: the velocity model's velocities (Session.humanizer),
    // each track's notes judged with the notes it plays around them, moved
    // humanizeVelocityAmount of the way there. Nothing if the model can't be
    // loaded (the status line says why).
    Q_INVOKABLE void humanizeVelocity();
    // Humanize › Timing: starts nudged at random (notes::humanizedTiming).
    Q_INVOKABLE void humanizeTiming();
    // Humanize › Timing's random numbers start over from `seed` (for tests).
    Q_INVOKABLE void seedRandom(quint32 seed) { rng_.seed(seed); }

    static QStringList quantizeGrids();
    QString quantizeGrid() const { return quantizeGrid_; }
    void setQuantizeGrid(const QString& grid);
    double quantizeAmount() const { return quantizeAmount_; }
    void setQuantizeAmount(double percent);
    double humanizeVelocityAmount() const { return humanizeVelocityAmount_; }
    void setHumanizeVelocityAmount(double percent);
    double humanizeTimingAmount() const { return humanizeTimingAmount_; }
    void setHumanizeTimingAmount(double percent);
    // Quantize's grid in beats.
    double quantizeStep() const;
    static double humanizeBeats() { return app::notes::kHumanizeBeats; }
    bool velocityModelAvailable() const;

    // --- Harmony -------------------------------------------------------------------

    // A chord of the song over the part the clips play, in roll beats.
    struct RollChord {
        double start = 0.0;
        double end = 0.0;
        QString name;  // "Am7", "C/E"
        int root = 0;  // pitch class
        bool minor = false;  // minor, diminished, half-diminished: drawn darker

        friend bool operator==(const RollChord&, const RollChord&) = default;
    };
    // The song's chords over the part the clips play, and the key notes out of
    // it are tinted red by (none: none known), while the harmony is shown.
    const std::vector<RollChord>& chords() const { return chords_; }
    const std::optional<app::Key>& scaleKey() const { return scaleKey_; }
    // Whether a pitch is out of scaleKey() (and so drawn tinted red).
    bool outOfKey(int pitch) const;
    // Write block chords (or a bass line) into the lead clip from the song's
    // chords over the part it plays, or from a progression in the key (C major
    // if none) where it has none; selected. The clip's own notes stay as they
    // are: a written note that would overlap one on its key is shortened or
    // left out. One undo step (none if nothing would be added).
    Q_INVOKABLE void generateChords();
    Q_INVOKABLE void generateBass();

    // --- Geometry -----------------------------------------------------------------

    // The time axis (in roll beats) and the vertical scroll.
    const timeline::Timeline& view() const { return view_; }
    double pxPerBeat() const { return view_.pxPerBeat(); }
    double scrollBeats() const { return view_.scrollBeats(); }
    int scrollY() const { return view_.scrollY(); }
    int rowHeight() const { return rowHeight_; }
    // Top of a key's row, in the note grid's (and the keys') coordinates.
    double pitchTop(int pitch) const;
    int pitchAt(double y) const;
    // A note's rectangle in the note grid (at least 3 px wide); of a lead clip's note.
    QRectF noteRect(const ClipNote& note) const;
    QRectF noteRect(const app::Note& note) const { return noteRect(ClipNote{0, note}); }

    Q_INVOKABLE void setScrollBeats(double beats);
    Q_INVOKABLE void setScrollY(double y);
    // Zoom time keeping the beat under `x` in place.
    Q_INVOKABLE void zoomAt(double x, double factor);
    // Make the keys' rows taller (or shorter), keeping the pitch under
    // `anchorY` (a note grid y) in place.
    Q_INVOKABLE void zoomRows(double notches, double anchorY);
    // The note grid's wheel (the keys forward theirs): Alt makes the rows taller
    // or shorter, Ctrl zooms time around the mouse, Shift or a horizontal wheel
    // scrolls sideways, otherwise three rows a notch.
    void wheel(const QPointF& pos, const QPoint& angleDelta, Qt::KeyboardModifiers modifiers);

    double hScrollTotal() const { return hTotal_; }
    double hScrollValue() const { return hValue_; }
    double hScrollPage() const;
    double vScrollTotal() const;
    double vScrollValue() const { return view_.scrollY(); }
    double vScrollPage() const;
    // The scroll bars moved by the user (pixels).
    Q_INVOKABLE void scrollToX(double pixels);
    Q_INVOKABLE void scrollToY(double pixels);

    // --- The note grid --------------------------------------------------------------

    NoteGrid* grid() const;
    void setGrid(NoteGrid* grid);
    // The note grid was resized, shown or hidden.
    void gridGeometryChanged();
    void gridVisibilityChanged(bool visible);
    // The notes take the keyboard (clicks on the keys, ruler and velocities give it them).
    void focusGrid();

    // --- Playhead, the start marker and the paste marker ---------------------------------

    // The playhead in roll beats, while playing inside a clip shown.
    std::optional<double> playhead() const { return playhead_; }
    // Where playback starts next (the arrangement's insert marker), as a roll
    // beat; none outside the clips shown.
    std::optional<double> startBeat() const;
    // Where Ctrl+V pastes (roll beats): where the grid was last clicked (none
    // until it is, and for new clips). Playback doesn't start from it.
    std::optional<double> pasteBeat() const { return pasteBeat_; }
    void setPasteBeat(double beat);
    // The ruler was clicked: play from this roll beat (snapped by the ruler).
    void requestLocate(double rollBeat);

    // --- Hearing notes ------------------------------------------------------------------

    bool preview() const { return preview_; }
    void setPreview(bool enabled);
    // Sound a key on the instrument of a clip's track (the lead's) until
    // releaseAudition(); one key sounds at a time.
    void audition(int pitch, int velocity = kPreviewVelocity, int clip = 0);
    Q_INVOKABLE void releaseAudition();
    std::optional<int> auditioned() const { return auditioned_; }
    // Sound notes for a moment (a rubber band's, as it catches them), each key
    // once (as loud as its loudest), on its clip's track; with those sounding
    // already (a key sounding isn't struck again), all of them stopping
    // kChordPreviewMs after the last were caught.
    void previewNotes(const std::vector<ClipNote>& notes);
    // The keys sounding that way now ((track, pitch)).
    const std::vector<std::pair<QString, int>>& previewing() const { return previewing_; }

Q_SIGNALS:
    void sessionChanged();
    void clipChanged();
    // Everything needs drawing again (repaint_all): notes, selection, colours, the start marker.
    void contentChanged();
    // Zoom or horizontal scroll.
    void viewChanged();
    // Vertical scroll or row height.
    void vscrollChanged();
    void scrollBarsChanged();
    void selectionChanged();
    void playheadChanged();
    void auditionChanged();
    void previewChanged();
    void toolSettingsChanged();
    void toolsChanged();
    void copiedChanged();
    // Play from this arrangement beat (the ruler was clicked).
    void locateRequested(double beat);

private:
    // The notes copied: each one's track and note, its start counted from
    // `start` (roll beats, where they were copied), `length` the stretch they
    // were copied with (or their own).
    struct Copied {
        std::vector<std::pair<QString, app::Note>> notes;
        double start = 0.0;
        double length = 0.0;
    };

    void refreshHarmony();
    void generate(bool bass);
    void connectSession();
    void repaintAll();
    void viewMoved();     // ViewState.changed: zoom or horizontal scroll
    void vscrolled();     // ViewState.vscroll_changed
    void fitIfReady();
    void updateBars();
    void onPosition(double beat);
    // origin() and shift() worked out again, from the clips as they are now.
    void relayout();
    void stopPreview();
    bool showsTrack(const QString& trackId) const;
    // A tool's result: `changed` (as `targets`, note for note) in place of `targets`.
    void applyTool(const std::vector<ClipNote>& targets, const std::vector<ClipNote>& changed, const QString& text);
    // A tool that works on times (Quantize, Humanize › Timing, ×2), applied on the roll
    // (across clips: the arrangement's grid), each note back in its clip.
    void applyOnRoll(const std::vector<ClipNote>& targets,
                     const std::function<std::vector<app::Note>(const std::vector<app::Note>&)>& tool,
                     const QString& text);
    // The selected notes' stretch: the rubber band's, or their own.
    std::optional<Span> selectionStretch() const;
    bool gridShowing() const;
    double gridWidth() const;
    double gridHeight() const;

    QPointer<app::Session> session_;
    app::ClipRefs clips_;
    double origin_ = 0.0;
    std::vector<double> shifts_;  // each clip's shift()
    timeline::Timeline view_;
    int rowHeight_ = kRowHeight;
    double rowHeightExact_ = kRowHeight;  // keeps a trackpad's small steps adding up
    std::vector<ClipNote> selected_;
    std::optional<Span> span_;
    std::optional<double> playhead_;
    std::optional<int> auditioned_;
    QString auditionTrack_;
    std::vector<std::pair<QString, int>> previewing_;
    QTimer previewTimer_;
    std::optional<double> pasteBeat_;
    Copied copied_;
    bool fitPending_ = false;
    bool toolsWanted_ = false;
    bool preview_ = true;
    QRandomGenerator rng_;  // for Humanize › Timing
    QString quantizeGrid_ = QString::fromLatin1(kDefaultGrid);
    double quantizeAmount_ = 100.0;
    double humanizeVelocityAmount_ = kDefaultHumanizeVelocity;
    double humanizeTimingAmount_ = kDefaultHumanizeTiming;
    bool toolsShown_ = false;
    QRectF toolsArea_;
    int toolsCount_ = 0;
    double hTotal_ = 0.0;
    double hValue_ = 0.0;
    QPointer<NoteGrid> grid_;
    std::vector<RollChord> chords_;
    std::optional<app::Key> scaleKey_;
};

}  // namespace sub::ui
