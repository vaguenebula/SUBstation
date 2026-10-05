#pragma once

// The track lanes (the old lanes_canvas.py's LanesCanvas): clips, grid, loop
// region, and all clip mouse editing; and the tracks' automation, over their
// clips and in lanes below them (Envelopes.h). A group's lane shows a summary
// of the clips of the tracks in it (folded or not); the tracks in a folded
// group have no lane. The playhead and the takes being recorded are items of
// their own above it (ArrangementPlayhead, LiveTakes): they change every frame.
//
// Painting, in order: each row's background (lighter if its track is
// selected); the grid all the way down (below the tracks too, where selecting
// works as well) and the loop region; per row a folded track's tint (under its
// clips' bars), a group's summary, its clips (but those the gesture hides), a
// frozen track's tint, the lines between its automation lanes and its bottom
// border; the gesture's previews (what stays of moved clips, and where they go,
// translucent); the envelopes; a drop preview or the empty arrangement's hint;
// the time selection (over the automation lanes it covers, or as a tint over
// each track's stretch, with the title bars and outlines of the clips there
// drawn again over it: selecting never lights up a title bar); the insert
// marker on the selected track (when nothing is selected); a breakpoint's
// value while dragged.
//
// Mouse: a left press starts, in this order of precedence,
//   Ctrl+Alt held                     -> PanGesture (hand scrolling)
//   on an automation lane             -> envelopes::press
//   on a clip's trim handle           -> select that clip; TrimGesture
//   inside the selected clip range    -> MoveRangeGesture (a click without
//     (in the clip band, no Shift)       dragging selects as a click elsewhere would)
//   on a clip's title                 -> select its area (Shift: the area holding
//                                        it and the last clip clicked); the insert
//                                        marker at its start; MoveRangeGesture
//   anywhere else                     -> clear the selection on that track; the
//                                        insert marker at the snapped beat;
//                                        TimeSelectGesture (not on a folded
//                                        track's lane: no grid there)
// Double-click a clip: the selected clips (or just it) open in the clip view.
// On an automation lane each click of a double-click counts. Wheel: Alt
// resizes (or folds) the track, Ctrl zooms around the mouse (1.2 a notch),
// Shift or a horizontal wheel scrolls sideways (80 px a notch), otherwise up
// and down (48 px a notch). Right-click: the lane's, the clip's or the time
// selection's menu (menuRequested; ArrangementMenu.qml shows it).
//
// Drops: audio files (from the browser or the file manager; dashed boxes show
// where they would go), devices, plug-ins and presets from the browser, and
// devices dragged from a track's chain (kDeviceMoveMime).

#include "arrangement/ArrangementItem.h"
#include "arrangement/Envelopes.h"
#include "arrangement/MenuEntries.h"
#include "arrangement/WaveformCache.h"
#include "editor/ClipRef.h"
#include "model/Clip.h"

#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>
#include <vector>

class QMimeData;

namespace sub::ui {

namespace arrangement {
// Devices dragged from a track's chain (the device view): the source track's
// id, then the device ids, a line each.
inline constexpr const char* kDeviceMoveMime = "application/x-substation-device-move";

inline constexpr double kEdgeGrab = 6;      // trim handles: this many pixels inside each end of a clip's title bar
inline constexpr double kTitleHeight = 16;  // also the grab area for selecting/moving the clip
inline constexpr double kMinTitleRow = 30;  // clips in shorter rows have a thin title bar instead
inline constexpr double kShortTitleHeight = 9;  // that thin bar: grab it to move the clip; below it, select time

// The title bar of a clip this high: where it is grabbed (the rest selects
// time). A folded track's clips are all title bar, as in Ableton: a bar with
// the clip's name, grabbed anywhere.
double clipTitleHeight(double clipHeight, bool folded = false);
}  // namespace arrangement

class ArrangementLanes : public ArrangementItem, public arrangement::LanesHost {
    Q_OBJECT
    QML_ELEMENT

public:
    // Where a clip was hit: its trim handles (the ends of its title bar), its
    // title (select and move), or its body (time selection, as on an empty lane).
    enum class Zone { Left, Right, Title, Body };
    struct Hit {
        QString trackId;
        app::Clip clip;
        Zone zone;
    };

    explicit ArrangementLanes(QQuickItem* parent = nullptr);
    ~ArrangementLanes() override;

    // --- Geometry (item coordinates) ---------------------------------------------------------

    // The row under an item y; with `clamp`, above the first track gives the
    // first, below the last gives the last shown.
    std::optional<int> rowIndexAt(double y, bool clamp = false) const;
    std::optional<Hit> hitClip(const QPointF& pos) const;
    // Whether `pos` is in the top band of a lane, where clips (not automation)
    // are selected. Short lanes have no title bar, so all of them is the band.
    bool inClipBand(const QPointF& pos) const override;
    // The automation lanes showing, top to bottom: in tracks' own lanes (below
    // the clips' title band) and below them.
    std::vector<arrangement::EnvelopeArea> envelopeAreas() const override;
    std::optional<arrangement::EnvelopeArea> envelopeAreaAt(const QPointF& pos) const;

    // --- What the mouse is over (for the tests too) ----------------------------------------

    // The cursor and hover for the mouse at `pos` (what a mouse move does).
    void updateHover(const QPointF& pos, Qt::KeyboardModifiers modifiers);
    const std::optional<arrangement::Hover>& hoverPoint() const { return hoverPoint_; }
    // The trim handle under the mouse: (clip id, left).
    std::optional<std::pair<QString, bool>> hoverEdge() const { return hoverEdge_; }
    bool gestureActive() const { return gesture_ != nullptr; }

    // --- Menus ------------------------------------------------------------------------------

    // The right-click menu at `pos` (a lane's, a clip's (selecting it first), the
    // time selection's, or an empty lane's).
    arrangement::MenuEntries contextMenu(const QPointF& pos);
    // Runs the entry of the last menu shown.
    Q_INVOKABLE void triggerMenu(int id);
    // Insert MIDI Clip at `x` on a MIDI track (see ArrangementActions::insertMidiClip). The clip on `trackId`.
    std::optional<app::ClipRef> insertMidiClip(const QString& trackId, double x);

    // --- Drops ----------------------------------------------------------------------------

    struct DropPreview {
        std::optional<int> row;  // none: a new track below the tracks
        double beat = 0.0;
        struct Source {
            QString path;
            QString name;
            double duration = 0.0;
        };
        std::vector<Source> sources;
    };
    const std::optional<DropPreview>& dropPreview() const { return dropPreview_; }
    // A drag over the lanes: whether it can drop here (and the preview of files).
    bool dragOver(const QMimeData* mime, const QPointF& pos);
    void dragLeft();
    // Dropped: whether it took it.
    bool drop(const QMimeData* mime, const QPointF& pos);

    // --- LanesHost ------------------------------------------------------------------------

    app::Session* hostSession() const override { return session(); }
    Arrangement* hostArrangement() const override { return arrangement(); }
    double hostWidth() const override { return width(); }
    void setHostCursor(const QCursor& cursor) override { setCursor(cursor); }
    void repaint() override { ArrangementItem::repaint(); }
    bool isTrackLanes() const override { return true; }
    std::optional<int> rowAt(double y, bool clamp) const override { return rowIndexAt(y, clamp); }

    arrangement::WaveformCache& waveforms() { return waveforms_; }

Q_SIGNALS:
    // Show this menu at `pos` (item coordinates); its entries' ids go back to triggerMenu().
    void menuRequested(const QVariantList& entries, const QPointF& pos);

protected:
    void paint(SgPainter& painter) override;
    void updatePolish() override;
    void connectSession(app::Session* session) override;
    void connectArrangement(Arrangement* arrangement) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void hoverEnterEvent(QHoverEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    // A clip as the last paint drew it, to draw its frame again over the selection's tint.
    struct Frame {
        QString trackId;
        QColor color;
        app::Clip clip;
        QRectF rect;
        bool selected;
        bool ghost;
        bool folded;
    };

    QRectF clipRect(const app::Clip& clip, double rowTop, int rowHeight) const;
    bool inClipRange(const QPointF& pos) const;
    bool inSelection(const QPointF& pos) const;
    void click(const QPointF& pos, Qt::KeyboardModifiers modifiers, const std::optional<Hit>& hit);
    void endGesture();
    void setHoverEdge(const std::optional<std::pair<QString, bool>>& edge);
    void onModifiers(Qt::KeyboardModifiers modifiers);
    void showMenu(const QPointF& pos);
    void updateIfShown(const QString& trackId);

    QHash<QString, QRectF> selectedAreas(const app::TimeRange& range) const;
    void drawClip(SgPainter& p, const QColor& trackColor, const app::Clip& clip, const QRectF& rect,
                  const QRectF& visible, bool selected, bool ghost, bool folded);
    void drawContent(SgPainter& p, const app::Clip& clip, const QRectF& rect, const QRectF& body,
                     const QRectF& visible);
    void drawClipFrame(SgPainter& p, const QColor& trackColor, const app::Clip& clip, const QRectF& rect,
                       bool selected, bool ghost, bool folded) const;
    void drawGroupSummary(SgPainter& p, const QString& groupId, double rowTop, int rowHeight,
                          const QRectF& visible) const;
    void drawNotes(SgPainter& p, const app::Clip& clip, const QRectF& area, const QRectF& visible) const;
    void drawDropPreview(SgPainter& p) const;

    std::unique_ptr<arrangement::Gesture> gesture_;
    std::optional<app::ClipRef> clipAnchor_;  // the last clip clicked without Shift
    std::optional<DropPreview> dropPreview_;
    QStringList dropPaths_;  // the files the preview was worked out for
    std::optional<std::pair<QString, bool>> hoverEdge_;
    std::optional<arrangement::Hover> hoverPoint_;
    std::optional<QPointF> hoverPos_;
    arrangement::MenuEntries menu_;
    arrangement::WaveformCache waveforms_;
    // Worked out on the GUI thread for paint(): the envelopes showing, and how each looks.
    std::vector<arrangement::EnvelopeArea> areas_;
    std::vector<arrangement::EnvelopeLook> looks_;
};

}  // namespace sub::ui
