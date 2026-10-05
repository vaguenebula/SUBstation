import QtQuick
import QtQuick.Controls
import SUBstation

// The arrangement: the ruler on top, the track lanes with their headers on the
// right (as in Ableton), the return tracks and the master pinned at the bottom
// (the returns above the master, a compact row each), and the scroll bars. What
// it draws and how it is edited are the C++ items' (ui/src/arrangement); the
// state they share is the Arrangement.
//
//    col 0 (stretches)               col 1 (252)          col 2
//   ┌──────────────────────────────┬────────────────────────────┐
//   │ ArrangementRuler (40)        │ GridInfo        (spans 1-2)│
//   ├──────────────────────────────┼───────────────────┬────────┤
//   │ ArrangementLanes             │ track headers     │ vbar   │
//   ├──────────────────────────────┼───────────────────┤ (rows  │
//   │ a BusLane per return         │ ReturnHeaders     │  1-3)  │
//   ├──────────────────────────────┼───────────────────┤        │
//   │ BusLane (the master's)       │ MasterHeader      │        │
//   ├──────────────────────────────┼───────────────────┴────────┘
//   │ hbar                         │
//   └──────────────────────────────┘
//
// For the main window:
//   functions zoom(factor), zoomToArrangement(), narrowGrid(), widenGrid(),
//   openClipView() (the selected clips, through Session.arrangement's
//   clipViewRequested), renameTrack(trackId) (in place; false if it can't be),
//   focusLanes(); properties snap, follow (read and write), gridStep (beats,
//   the grid's step whether snapping or not), gridLevel; signal
//   statusMessage(text). Clips double-clicked open through
//   Session.arrangement's clipViewRequested; a click on the ruler plays from
//   there through Session.locate(beat).
FocusScope {
    id: view

    property alias snap: arrangementState.snap
    property alias follow: arrangementState.follow
    readonly property real gridStep: arrangementState.gridStep
    property alias gridLevel: arrangementState.gridLevel
    readonly property Arrangement arrangement: arrangementState
    readonly property ArrangementLanes lanes: lanesItem

    readonly property int headerWidth: 252
    readonly property int barWidth: Theme.scrollBarWidth
    readonly property int rulerHeight: 40
    readonly property real lanesWidth: Math.max(0, width - headerWidth - barWidth)
    readonly property real lanesHeight: Math.max(0, height - rulerHeight - arrangementState.returnsHeight
                                                     - arrangementState.masterHeight - barWidth)

    signal statusMessage(string message)

    function zoom(factor) {
        arrangementState.zoom(factor)
    }
    function zoomToArrangement() {
        arrangementState.zoomToArrangement()
    }
    function narrowGrid() {
        arrangementState.narrowGrid()
    }
    function widenGrid() {
        arrangementState.widenGrid()
    }
    function openClipView() {
        arrangementState.openClipView()
    }
    function renameTrack(trackId) {
        return arrangementState.renameTrack(trackId)
    }
    function focusLanes() {
        lanesItem.forceActiveFocus()
    }

    implicitWidth: 1000
    implicitHeight: 500

    Arrangement {
        id: arrangementState
        session: Session
        onStatusMessage: message => view.statusMessage(message)
    }

    ArrangementMenu {
        id: arrangementMenu
        objectName: "arrangementMenu"
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.window
    }

    // --- Row 0: the ruler, and the grid's size ---

    ArrangementRuler {
        id: ruler
        objectName: "ruler"
        width: view.lanesWidth
        height: view.rulerHeight
        clip: true
        session: Session
        arrangement: arrangementState

        ArrangementPlayhead {
            anchors.fill: parent
            session: Session
            arrangement: arrangementState
            ruler: true
        }
    }

    GridInfo {
        objectName: "gridInfo"
        x: view.lanesWidth
        width: view.headerWidth + view.barWidth
        height: view.rulerHeight
        arrangement: arrangementState
    }

    // --- Row 1: the lanes and the tracks' headers ---

    ArrangementLanes {
        id: lanesItem
        objectName: "lanes"
        y: view.rulerHeight
        width: view.lanesWidth
        height: view.lanesHeight
        clip: true
        session: Session
        arrangement: arrangementState
        focus: true
        onMenuRequested: (entries, pos) => arrangementMenu.show(entries, lanesItem, lanesItem, pos.x, pos.y)

        LiveTakes {
            objectName: "liveTakes"
            anchors.fill: parent
            session: Session
            arrangement: arrangementState
        }
        ArrangementPlayhead {
            anchors.fill: parent
            session: Session
            arrangement: arrangementState
        }
    }

    Item {
        id: headerColumn
        objectName: "headers"
        x: view.lanesWidth
        y: view.rulerHeight
        width: view.headerWidth
        height: view.lanesHeight
        clip: true

        Rectangle {
            anchors.fill: parent
            color: Theme.emptyArea
        }

        // Below the headers: a click selects no track; the wheel scrolls them with the lanes.
        MouseArea {
            anchors.fill: parent
            onPressed: Session.selection.selectTrack("")
            onWheel: wheel => arrangementState.setScrollY(arrangementState.scrollY - wheel.angleDelta.y / 120 * 48)
        }

        Repeater {
            model: arrangementState.rows

            // (the model's roles: trackId, top, mainHeight, rowHeight, hidden, folded, depth, number)
            TrackHeader {
                objectName: "header:" + model.trackId
                trackId: model.trackId
                number: model.number
                y: model.top - arrangementState.scrollY
                width: view.headerWidth
                height: model.rowHeight
                visible: !model.hidden  // (in a folded group)
                session: Session
                arrangement: arrangementState
                menu: arrangementMenu
            }
        }

        // Where dragged headers would put their tracks.
        Rectangle {
            objectName: "dropLine"
            width: parent.width
            y: arrangementState.dropMarkerY
            height: arrangementState.dropMarkerHeight
            visible: arrangementState.dropMarkerVisible && !arrangementState.dropMarkerInto
            color: Theme.accent
        }
        Rectangle {
            objectName: "dropFrame"
            width: parent.width
            y: arrangementState.dropMarkerY
            height: arrangementState.dropMarkerHeight
            visible: arrangementState.dropMarkerVisible && arrangementState.dropMarkerInto
            color: "transparent"
            border.color: Theme.accent
            border.width: 2
        }
    }

    // --- Row 2: the returns, a lane and a header each ---

    Column {
        id: returnLanes
        y: view.rulerHeight + view.lanesHeight
        width: view.lanesWidth

        Repeater {
            model: arrangementState.returns

            // (the model's roles: trackId, mainHeight, rowHeight)
            BusLane {
                objectName: "returnLane:" + model.trackId
                owner: model.trackId
                width: view.lanesWidth
                height: model.rowHeight
                clip: true
                session: Session
                arrangement: arrangementState
                id: returnLane
                onMenuRequested: (entries, pos) => arrangementMenu.show(entries, returnLane, returnLane, pos.x, pos.y)

                ArrangementPlayhead {
                    anchors.fill: parent
                    session: Session
                    arrangement: arrangementState
                }
            }
        }
    }

    Column {
        id: returnHeaders
        x: view.lanesWidth
        y: view.rulerHeight + view.lanesHeight
        width: view.headerWidth

        Repeater {
            model: arrangementState.returns

            ReturnHeader {
                objectName: "returnHeader:" + model.trackId
                trackId: model.trackId
                width: view.headerWidth
                height: model.rowHeight
                session: Session
                arrangement: arrangementState
                menu: arrangementMenu
            }
        }
    }

    // --- Row 3: the master ---

    BusLane {
        id: masterLane
        objectName: "masterLane"
        y: view.rulerHeight + view.lanesHeight + arrangementState.returnsHeight
        width: view.lanesWidth
        height: arrangementState.masterHeight
        clip: true
        owner: "master"
        session: Session
        arrangement: arrangementState
        onMenuRequested: (entries, pos) => arrangementMenu.show(entries, masterLane, masterLane, pos.x, pos.y)

        ArrangementPlayhead {
            anchors.fill: parent
            session: Session
            arrangement: arrangementState
        }
    }

    MasterHeader {
        objectName: "masterHeader"
        x: view.lanesWidth
        y: masterLane.y
        width: view.headerWidth
        height: arrangementState.masterHeight
        session: Session
        arrangement: arrangementState
        menu: arrangementMenu
    }

    // --- The scroll bars: they follow the view, except while dragged (then the view follows them) ---

    ScrollBar {
        id: vbar
        objectName: "vbar"
        x: view.lanesWidth + view.headerWidth
        y: view.rulerHeight
        width: view.barWidth
        height: view.lanesHeight + arrangementState.returnsHeight + arrangementState.masterHeight
        orientation: Qt.Vertical
        policy: ScrollBar.AlwaysOn
        focusPolicy: Qt.NoFocus
        size: arrangementState.vScrollPage / Math.max(1, arrangementState.vScrollTotal)
        stepSize: 24 / Math.max(1, arrangementState.vScrollTotal)
        onPositionChanged: if (pressed) arrangementState.scrollToY(position * arrangementState.vScrollTotal)

        Binding on position {
            when: !vbar.pressed
            value: arrangementState.vScrollValue / Math.max(1, arrangementState.vScrollTotal)
        }
    }

    ScrollBar {
        id: hbar
        objectName: "hbar"
        y: view.height - view.barWidth
        width: view.lanesWidth
        height: view.barWidth
        orientation: Qt.Horizontal
        policy: ScrollBar.AlwaysOn
        focusPolicy: Qt.NoFocus
        size: arrangementState.hScrollPage / Math.max(1, arrangementState.hScrollTotal)
        stepSize: Math.max(1, Math.floor(view.lanesWidth / 20)) / Math.max(1, arrangementState.hScrollTotal)
        onPositionChanged: if (pressed) arrangementState.scrollToX(position * arrangementState.hScrollTotal)

        Binding on position {
            when: !hbar.pressed
            value: arrangementState.hScrollValue / Math.max(1, arrangementState.hScrollTotal)
        }
    }
}
