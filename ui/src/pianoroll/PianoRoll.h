#pragma once

// The piano roll: the clip view of a MIDI clip, laid out like Ableton's MIDI
// editor. It holds the clip it shows, its own time axis (zoom, scroll) and row
// height, the selected notes, the playhead, the key sounding, and the note
// tools' settings and actions. The items that draw it (NoteGrid, PianoKeys,
// PianoRuler, VelocityLane, RollPlayhead) share one; PianoRollView.qml lays
// them out, with the note tools (NoteTools.qml) floating over the notes.
//
// Times are content beats: beats of the clip's notes, the ruler's 1 being the
// clip's first content beat, not the arrangement's. The part the clip plays
// (offsetBeats to windowEnd) is lit, the rest dimmed; notes outside it are kept:
// trimming a clip hides notes, never deletes them. Pitch runs 127 at the top
// to 0 at the bottom, a row of rowHeight pixels each.
//
// Every edit goes through commit(): the clip's whole new list of notes, one
// undo command, merged with the previous one when they share a merge key.
// Gestures pass their own key, so a drag is one undo step, while the model
// (and what plays) changes live as the mouse moves. clip() looks the clip up
// in the project each time: the project replaces it on every edit.

#include "model/Clip.h"
#include "model/Notes.h"
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
#include <QtQml/qqmlregistration.h>

#include <optional>
#include <vector>

namespace sub::app {
class EngineBridge;
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
    Q_PROPERTY(bool hasClip READ hasClip NOTIFY contentChanged)
    // The headphones button: notes clicked, added and moved are heard.
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
    // chosen, Quantize's and Humanize's amounts in percent.
    Q_PROPERTY(QStringList quantizeGrids READ quantizeGrids CONSTANT)
    Q_PROPERTY(QString quantizeGrid READ quantizeGrid WRITE setQuantizeGrid NOTIFY toolSettingsChanged)
    Q_PROPERTY(double quantizeAmount READ quantizeAmount WRITE setQuantizeAmount NOTIFY toolSettingsChanged)
    Q_PROPERTY(double humanizeAmount READ humanizeAmount WRITE setHumanizeAmount NOTIFY toolSettingsChanged)
    // At 100 %, how far Humanize moves a note (beats) and changes its velocity.
    Q_PROPERTY(double humanizeBeats READ humanizeBeats CONSTANT)
    Q_PROPERTY(int humanizeVelocity READ humanizeVelocity CONSTANT)
    // Where the note tools are heading (shown or gone), and while shown, the
    // selected notes' bounding rectangle (in the note grid's coordinates) and
    // how many there are: the bar floats by them.
    Q_PROPERTY(bool toolsShown READ toolsShown NOTIFY toolsChanged)
    Q_PROPERTY(QRectF toolsArea READ toolsArea NOTIFY toolsChanged)
    Q_PROPERTY(int toolsCount READ toolsCount NOTIFY toolsChanged)

public:
    static constexpr int kKeysWidth = 64;
    static constexpr int kRulerHeight = 24;
    static constexpr int kRowHeight = 12;
    static constexpr int kMinRowHeight = 5;
    static constexpr int kMaxRowHeight = 36;
    static constexpr double kRowHeightStep = 1.5;  // pixels per wheel notch (Alt+wheel)
    static constexpr int kDefaultPitch = 60;       // C3: centred for a clip without notes
    static constexpr int kPreviewVelocity = 100;
    static constexpr const char* kDefaultGrid = "1/16";
    static constexpr double kDefaultHumanize = 25.0;  // %

    explicit PianoRoll(QObject* parent = nullptr);
    ~PianoRoll() override;

    app::Session* session() const { return session_; }
    void setSession(app::Session* session);
    app::Project* project() const;
    app::ProjectEditor* editor() const;
    app::EngineBridge* bridge() const;
    app::Selection* selection() const;

    // --- The clip ---------------------------------------------------------------

    // Show a MIDI clip (fitted to the view the first time), or nothing ("").
    Q_INVOKABLE void setClip(const QString& trackId, const QString& clipId);
    QString trackId() const { return trackId_; }
    QString clipId() const { return clipId_; }
    // The clip, looked up now (null: none, or gone).
    const app::Clip* clip() const;
    bool hasClip() const { return clip() != nullptr; }
    // The clip's track's colour (the accent without a clip).
    QColor trackColor() const;
    // After the clip changed (edits, undo): forget selected notes that are gone, repaint.
    void refresh();
    // Make `clipNotes` the clip's notes (one undo step per `mergeKey`) and select `selected`.
    void commit(const std::vector<app::Note>& clipNotes, const QString& text, const QString& mergeKey = {},
                const std::optional<std::vector<app::Note>>& selected = std::nullopt);

    // --- Selected notes -----------------------------------------------------------

    // Select `notes`; `tools` brings up the note tools by them.
    void setSelection(const std::vector<app::Note>& notes, bool tools = false);
    // The selected notes, sorted (a set: no two alike).
    const std::vector<app::Note>& selected() const { return selected_; }
    bool isSelected(const app::Note& note) const;
    int selectedCount() const { return static_cast<int>(selected_.size()); }
    // The note tools show for a group chosen by a rubber band or Ctrl+A, not for
    // notes clicked or drawn; they stay while that group is edited.
    bool toolsWanted() const { return toolsWanted_; }
    void setToolsWanted(bool wanted) { toolsWanted_ = wanted; }

    // --- Note tools ---------------------------------------------------------------

    // Show the note tools by the selected notes when they were chosen as a
    // group, or hide them: with nothing selected, and while a drag is moving
    // notes or drawing a rubber band.
    void placeTools();
    bool toolsShown() const { return toolsShown_; }
    QRectF toolsArea() const { return toolsArea_; }
    int toolsCount() const { return toolsCount_; }
    // What the tools act on: the selected notes, or all of them if none are
    // (Ctrl+U with nothing selected), by time.
    std::vector<app::Note> toolTargets() const;
    Q_INVOKABLE void legato();
    // ×2 (2.0) and ÷2 (0.5).
    Q_INVOKABLE void scaleTime(double factor);
    Q_INVOKABLE void quantize();
    Q_INVOKABLE void humanize();
    // Humanize's random numbers start over from `seed` (for tests).
    Q_INVOKABLE void seedRandom(quint32 seed) { rng_.seed(seed); }

    static QStringList quantizeGrids();
    QString quantizeGrid() const { return quantizeGrid_; }
    void setQuantizeGrid(const QString& grid);
    double quantizeAmount() const { return quantizeAmount_; }
    void setQuantizeAmount(double percent);
    double humanizeAmount() const { return humanizeAmount_; }
    void setHumanizeAmount(double percent);
    // Quantize's grid in beats.
    double quantizeStep() const;
    static double humanizeBeats() { return app::notes::kHumanizeBeats; }
    static int humanizeVelocity() { return app::notes::kHumanizeVelocity; }

    // --- Geometry -----------------------------------------------------------------

    // The time axis (in content beats) and the vertical scroll.
    const timeline::Timeline& view() const { return view_; }
    double pxPerBeat() const { return view_.pxPerBeat(); }
    double scrollBeats() const { return view_.scrollBeats(); }
    int scrollY() const { return view_.scrollY(); }
    int rowHeight() const { return rowHeight_; }
    // Top of a key's row, in the note grid's (and the keys') coordinates.
    double pitchTop(int pitch) const;
    int pitchAt(double y) const;
    // A note's rectangle in the note grid (at least 3 px wide).
    QRectF noteRect(const app::Note& note) const;

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

    // --- Playhead and the start marker -------------------------------------------------

    // The playhead in content beats, while playing inside the clip.
    std::optional<double> playhead() const { return playhead_; }
    // Where playback starts next (the arrangement's insert marker), as a content
    // beat; none outside the clip.
    std::optional<double> startBeat() const;
    // The ruler was clicked: play from this content beat (snapped by the ruler).
    void requestLocate(double contentBeat);

    // --- Hearing notes ------------------------------------------------------------------

    bool preview() const { return preview_; }
    void setPreview(bool enabled);
    // Sound a key on the track's instrument until releaseAudition(); one key
    // sounds at a time.
    void audition(int pitch, int velocity = kPreviewVelocity);
    Q_INVOKABLE void releaseAudition();
    std::optional<int> auditioned() const { return auditioned_; }

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
    // Play from this arrangement beat (the ruler was clicked).
    void locateRequested(double beat);

private:
    void connectSession();
    void repaintAll();
    void viewMoved();     // ViewState.changed: zoom or horizontal scroll
    void vscrolled();     // ViewState.vscroll_changed
    void fitIfReady();
    void updateBars();
    void onPosition(double beat);
    void applyTool(const std::vector<app::Note>& targets, const std::vector<app::Note>& changed, const QString& text);
    bool gridShowing() const;
    double gridWidth() const;
    double gridHeight() const;

    QPointer<app::Session> session_;
    QString trackId_;
    QString clipId_;
    timeline::Timeline view_;
    int rowHeight_ = kRowHeight;
    double rowHeightExact_ = kRowHeight;  // keeps a trackpad's small steps adding up
    std::vector<app::Note> selected_;
    std::optional<double> playhead_;
    std::optional<int> auditioned_;
    bool fitPending_ = false;
    bool toolsWanted_ = false;
    bool preview_ = true;
    QRandomGenerator rng_;  // for Humanize
    QString quantizeGrid_ = QString::fromLatin1(kDefaultGrid);
    double quantizeAmount_ = 100.0;
    double humanizeAmount_ = kDefaultHumanize;
    bool toolsShown_ = false;
    QRectF toolsArea_;
    int toolsCount_ = 0;
    double hTotal_ = 0.0;
    double hValue_ = 0.0;
    QPointer<NoteGrid> grid_;
};

}  // namespace sub::ui
