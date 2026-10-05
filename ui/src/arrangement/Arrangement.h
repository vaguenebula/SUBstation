#pragma once

// The arrangement view's state, without its widgets (the old
// arrangement_view.py's ArrangementView, and view_state.py's ViewState): the
// time axis (zoom, scroll, the adaptive grid, snapping, following the
// playhead), the tracks' vertical layout and the rows QML lays the headers,
// the returns and the master out in, the scroll bars, the playhead, dragging
// headers to move tracks, and Alt+wheel resizing. The items that draw the
// arrangement (the ruler, the lanes, the returns' and the master's lanes, the
// playhead, the headers) share one; ArrangementView.qml lays them out:
//
//   Arrangement { id: arrangement; session: Session }
//   ArrangementLanes { session: Session; arrangement: arrangement }
//
// The scroll bars are views of the state, not the other way round: the
// horizontal one reaches from the start to the content's end (the last clip,
// the loop's end, or the visible width, plus 16 bars), the vertical one over
// the tracks plus kDropZone (empty space below them for dropping files).
//
// Following (Ableton's): while playing, the view scrolls so the playhead stays
// between 4 % and 96 % of the lanes' width; scrolling by hand (the scroll bar,
// the wheel, the ruler, a Ctrl+Alt drag) pauses that until playback stops or
// starts again.

#include "arrangement/RowModels.h"
#include "arrangement/TrackLayout.h"
#include "model/Routing.h"
#include "session/Session.h"
#include "timeline/Timeline.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <optional>

namespace sub::app {
class EngineBridge;
class Project;
class ProjectEditor;
class Selection;
}  // namespace sub::app

namespace sub::ui {

class Arrangement : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(double pxPerBeat READ pxPerBeat NOTIFY viewChanged)
    Q_PROPERTY(double scrollBeats READ scrollBeats NOTIFY viewChanged)
    Q_PROPERTY(int scrollY READ scrollY NOTIFY vscrollChanged)
    // Snapping to the grid (Ctrl+4; the grid's label toggles it).
    Q_PROPERTY(bool snap READ snap WRITE setSnap NOTIFY gridChanged)
    // The view follows the playhead while playing (the transport's Follow).
    Q_PROPERTY(bool follow READ follow WRITE setFollow NOTIFY followChanged)
    // Scrolled by hand while playing: not following until playback stops or starts again.
    Q_PROPERTY(bool followPaused READ followPaused NOTIFY followChanged)
    // The grid's step in beats (whether snapping or not), and its level (Ctrl+1 narrower, Ctrl+2 wider).
    Q_PROPERTY(double gridStep READ gridStep NOTIFY gridChanged)
    Q_PROPERTY(int gridLevel READ gridLevel WRITE setGridLevel NOTIFY gridChanged)
    // "Grid 1/16", "Grid 2 Bars", with " (off)" while not snapping.
    Q_PROPERTY(QString gridLabel READ gridLabel NOTIFY gridChanged)
    // The lanes' size (the scroll bars' pages, following, zooming around the middle).
    Q_PROPERTY(double lanesWidth READ lanesWidth WRITE setLanesWidth NOTIFY lanesSizeChanged)
    Q_PROPERTY(double lanesHeight READ lanesHeight WRITE setLanesHeight NOTIFY lanesSizeChanged)
    // The tracks' rows (TrackRowModel) and the returns' (ReturnRowModel).
    Q_PROPERTY(QAbstractItemModel* rows READ rows CONSTANT)
    Q_PROPERTY(QAbstractItemModel* returns READ returns CONSTANT)
    Q_PROPERTY(int totalHeight READ totalHeight NOTIFY layoutChanged)
    Q_PROPERTY(bool hasTracks READ hasTracks NOTIFY layoutChanged)
    Q_PROPERTY(int masterMainHeight READ masterMainHeight NOTIFY masterRowsChanged)
    Q_PROPERTY(int masterHeight READ masterHeight NOTIFY masterRowsChanged)
    // The returns' rows, all together (above the master).
    Q_PROPERTY(int returnsHeight READ returnsHeight NOTIFY returnRowsChanged)
    // The scroll bars, in pixels: how far the content reaches (range plus page), where the view is, the page.
    Q_PROPERTY(double hScrollTotal READ hScrollTotal NOTIFY scrollBarsChanged)
    Q_PROPERTY(double hScrollValue READ hScrollValue NOTIFY scrollBarsChanged)
    Q_PROPERTY(double hScrollPage READ hScrollPage NOTIFY scrollBarsChanged)
    Q_PROPERTY(double vScrollTotal READ vScrollTotal NOTIFY scrollBarsChanged)
    Q_PROPERTY(double vScrollValue READ vScrollValue NOTIFY scrollBarsChanged)
    Q_PROPERTY(double vScrollPage READ vScrollPage NOTIFY scrollBarsChanged)
    // Where dragged headers would put their tracks: a line between tracks, or a
    // frame around the group they would go into (header column coordinates).
    Q_PROPERTY(bool dropMarkerVisible READ dropMarkerVisible NOTIFY dropMarkerChanged)
    Q_PROPERTY(bool dropMarkerInto READ dropMarkerInto NOTIFY dropMarkerChanged)
    Q_PROPERTY(double dropMarkerY READ dropMarkerY NOTIFY dropMarkerChanged)
    Q_PROPERTY(double dropMarkerHeight READ dropMarkerHeight NOTIFY dropMarkerChanged)

public:
    static constexpr int kHeightStep = 12;  // pixels per wheel notch when Alt+wheel resizes a track
    static constexpr double kWheelGesture = 0.4;  // s: wheel events closer than this resize (or fold) the same track
    static constexpr double kFollowMargin = 0.04;  // of the width: following keeps the playhead between 4 % and 96 %

    explicit Arrangement(QObject* parent = nullptr);
    ~Arrangement() override;

    app::Session* session() const { return session_; }
    void setSession(app::Session* session);
    app::Project* project() const;
    app::ProjectEditor* editor() const;
    app::EngineBridge* bridge() const;
    app::Selection* selection() const;

    // --- The time axis ---------------------------------------------------------------------

    const timeline::Timeline& view() const { return view_; }
    double pxPerBeat() const { return view_.pxPerBeat(); }
    double scrollBeats() const { return view_.scrollBeats(); }
    int scrollY() const { return view_.scrollY(); }
    bool snap() const { return view_.snap(); }
    void setSnap(bool snap);
    bool follow() const { return follow_; }
    void setFollow(bool follow);
    bool followPaused() const { return followPaused_; }
    double gridStep() const { return view_.gridStep(); }
    int gridLevel() const { return view_.gridLevel(); }
    void setGridLevel(int level);
    QString gridLabel() const;

    Q_INVOKABLE void setScrollBeats(double beats);
    // Scroll as the user asked: the view stops following the playhead (which
    // would pull it back) until playback stops or starts again.
    Q_INVOKABLE void scrollByHand(double beats);
    Q_INVOKABLE void setScrollY(double y);
    // Zoom keeping the beat under `x` in place.
    Q_INVOKABLE void zoomAt(double x, double factor);
    // The zoom as it is (pixels per beat), the scroll unchanged.
    void setPxPerBeat(double pxPerBeat);
    // + and -: zoom around the playhead if it shows, else around the middle.
    Q_INVOKABLE void zoom(double factor);
    // Z: the whole arrangement (at least 8 bars) fits the lanes.
    Q_INVOKABLE void zoomToArrangement();
    Q_INVOKABLE void narrowGrid();
    Q_INVOKABLE void widenGrid();
    // Shift+Tab: the selected clips open in the clip view (Session.arrangement's clipViewRequested).
    Q_INVOKABLE void openClipView();
    // A clip double-clicked: the selected clips open, it first.
    void openClips(const QString& leadTrackId, const QString& leadClipId);

    double lanesWidth() const { return lanesWidth_; }
    void setLanesWidth(double width);
    double lanesHeight() const { return lanesHeight_; }
    void setLanesHeight(double height);

    // --- The layout ------------------------------------------------------------------------

    const arrangement::TrackLayout& layout() const { return layout_; }
    QAbstractItemModel* rows() const { return rows_; }
    QAbstractItemModel* returns() const { return returns_; }
    int totalHeight() const { return layout_.totalHeight(); }
    bool hasTracks() const { return !layout_.rows().empty(); }
    const arrangement::AutomationRows& masterRows() const { return masterRows_; }
    int masterMainHeight() const { return masterRows_.mainHeight; }
    int masterHeight() const { return masterRows_.height(); }
    int returnsHeight() const;
    // A return's rows (none: not a return).
    std::optional<arrangement::AutomationRows> returnRows(const QString& returnId) const;
    // The rows of an owner's lanes below its own: for a track, a return or the
    // master, as [{index, key, top (from the owner's row's top), height}].
    Q_INVOKABLE QVariantList lanesOf(const QString& owner) const;
    // The routing graph, worked out once per change (which sends can be used).
    const app::RoutingGraph& routingGraph() const;

    // --- Scroll bars -------------------------------------------------------------------------

    double hScrollTotal() const { return hTotal_; }
    double hScrollValue() const { return hValue_; }
    double hScrollPage() const { return std::max(1.0, lanesWidth_); }
    double vScrollTotal() const { return view_.maxScrollY() + vScrollPage(); }
    double vScrollValue() const { return view_.scrollY(); }
    double vScrollPage() const { return std::max(1.0, lanesHeight_); }
    // The scroll bars moved by the user (pixels).
    Q_INVOKABLE void scrollToX(double pixels);
    Q_INVOKABLE void scrollToY(double pixels);

    // --- The playhead -----------------------------------------------------------------------

    // Where the playhead shows while playing (none stopped: only the start marker shows).
    std::optional<double> playhead() const { return playhead_; }
    // The bridge's position: the playhead, and following it.
    void onPosition(double beat);

    // --- Tracks ------------------------------------------------------------------------------

    // Ctrl+R: rename a track (or a return) in place, scrolled into view (false:
    // it can't be: the master, or a track folded away in its group).
    Q_INVOKABLE bool renameTrack(const QString& trackId);
    // Alt+wheel over a track (its header or its lane), as one gesture: down
    // shrinks it, and once it is as small as it gets, folds it (a group: hides
    // its tracks too); up unfolds a folded track, then makes it taller. A turn
    // of the wheel acts on the track it started on: the tracks below move up
    // under the mouse as it shrinks or folds.
    void wheelResize(const QString& trackId, int delta);
    // The clock wheelResize goes by (seconds; the tests' own).
    void setClock(std::function<double()> clock) { clock_ = std::move(clock); }

    // Where tracks dropped at `y` (header column coordinates) go: before the
    // track at `index`, into `parent` ("": none), the group shown as taking
    // them (`into`, or ""), and the line between tracks (content y). Onto the
    // middle of a group's header: into it, last; on the upper half of a header:
    // before it, in its group; on the lower half: after it (after the whole
    // subtree of a folded group; into an open group, first); below the tracks:
    // last, in no group. None where they can't go (editor's canMoveTracks).
    struct DropTarget {
        int index = 0;
        QString parent;
        QString into;
        int line = 0;
    };
    std::optional<DropTarget> dropTarget(const QStringList& trackIds, double y) const;
    // Tracks dragged over the headers: the marker shows where they would go.
    void dragTracks(const QStringList& trackIds, double y);
    // Dropped: moved there (if they can go). The marker goes.
    void dropTracks(const QStringList& trackIds, double y);
    bool dropMarkerVisible() const { return marker_.has_value(); }
    bool dropMarkerInto() const { return marker_ && marker_->into; }
    double dropMarkerY() const { return marker_ ? marker_->y : 0.0; }
    double dropMarkerHeight() const { return marker_ ? marker_->height : 0.0; }

Q_SIGNALS:
    void sessionChanged();
    void viewChanged();     // zoom or horizontal scroll
    void vscrollChanged();
    void gridChanged();
    void followChanged();
    void lanesSizeChanged();
    void layoutChanged();   // the tracks' rows (heights, folding, automation lanes)
    void masterRowsChanged();
    void returnRowsChanged();
    void scrollBarsChanged();
    void playheadChanged();
    void dropMarkerChanged();
    void statusMessage(const QString& message);
    // Start renaming this track (or return) in place (its header does).
    void renameRequested(const QString& trackId);

private:
    struct Marker {
        bool into = false;
        double y = 0.0;
        double height = 0.0;
    };

    void connectSession();
    void onReset();
    void rebuild();
    void onTrackChanged(const QString& trackId);
    void onAutomationViewChanged(const QString& owner);
    void updateMasterRows();
    void updateReturnRows();
    void updateHBar();
    void updateVBar();
    void moved();     // ViewState.changed
    void vscrolled(); // ViewState.vscroll_changed
    void invalidateGraph() { graphDirty_ = true; }

    QPointer<app::Session> session_;
    timeline::Timeline view_;
    bool follow_ = true;
    bool followPaused_ = false;
    arrangement::TrackLayout layout_;
    arrangement::TrackRowModel* rows_;
    arrangement::ReturnRowModel* returns_;
    arrangement::AutomationRows masterRows_{arrangement::kMasterHeight, {}};
    double lanesWidth_ = 0.0;
    double lanesHeight_ = 0.0;
    double hTotal_ = 0.0;
    double hValue_ = 0.0;
    std::optional<double> playhead_;
    std::optional<Marker> marker_;
    std::optional<std::pair<QString, double>> lastWheel_;  // the track turned last, and when
    std::function<double()> clock_;
    QElapsedTimer elapsed_;
    mutable app::RoutingGraph graph_;
    mutable bool graphDirty_ = true;
};

}  // namespace sub::ui
