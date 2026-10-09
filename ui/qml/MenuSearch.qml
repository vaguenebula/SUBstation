pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import SUBstation

// A search field over a list of tracks, as one entry of a menu (a device's
// sidechain menu, a track's Audio From and Audio To): the list shows the rows
// whose text has every word typed in it (in any case), at most `maxRows` of
// them at a time, scrolled with the wheel or its scroll bar; the menu's other
// entries stay where they are. The list keeps its height while filtering, so
// the menu doesn't move under the mouse. The field takes the keyboard as the
// menu opens, so typing filters at once, and keeps it: the arrows move the
// highlight along the rows (and on to the menu's entries above and below),
// Enter chooses the row highlighted (the first that can be chosen, once
// something is typed), Esc closes the menu. With one of the menu's own entries
// highlighted, what is typed is typed here, and the arrows come back into the
// list (Enter chooses that entry, as menus do): the menu's entries pass their
// keys to keyPressed() and typed() (the menu's searchField: see DynamicMenu,
// ArrangementMenu and the style's MenuItem).
Item {
    id: search

    required property T.Menu menu
    // The rows: the menu's entries ({text, enabled, checkable, checked}, and
    // whatever else its rowComponent shows); choosing one closes the menu, then
    // calls run(row).
    required property var rows
    required property var run
    // What a row is drawn with: a MenuItem showing `entry` (one of the rows),
    // as the menu draws its own entries. (The menu's: a component declared
    // here could not be made in the list's delegates before Qt 6.12, this
    // file's components being bound to it.)
    required property Component rowComponent
    property int maxRows: 8
    readonly property alias field: field
    readonly property alias list: list
    // The rows shown (their indices in `rows`), and the one highlighted (an index in `shown`; -1: none).
    property var shown: rows.map((row, i) => i)
    property int current: -1

    readonly property int rowHeight: probe.implicitHeight
    readonly property real widest: {
        let widest = 0
        for (const row of rows)
            widest = Math.max(widest, metrics.advanceWidth(Theme.withoutMnemonics(row.text)))
        return widest
    }

    implicitWidth: Math.max(160, Math.ceil(widest) + probe.leftPadding + probe.rightPadding
                                 + (rows.length > maxRows ? Theme.scrollBarWidth : 0))
    implicitHeight: list.y + list.height + 2

    // The rows' texts (all of them, or those shown), for the tests.
    function texts(shownOnly) {
        return (shownOnly ? shown.map(i => rows[i]) : rows).map(row => row.text)
    }

    // The row shown with this text, scrolled into view (null: none), for the tests.
    function rowItem(text) {
        const index = shown.findIndex(i => rows[i].text === text)
        if (index < 0)
            return null
        list.positionViewAtIndex(index, ListView.Contain)
        list.forceLayout()
        const made = list.itemAtIndex(index)
        return made ? made.row : null
    }

    function filter() {
        const words = field.text.toLowerCase().split(/\s+/).filter(word => word !== "")
        const matching = []
        for (let i = 0; i < rows.length; ++i) {
            const text = Theme.withoutMnemonics(rows[i].text).toLowerCase()
            if (words.every(word => text.includes(word)))
                matching.push(i)
        }
        shown = matching
        list.positionViewAtBeginning()
        highlight(words.length > 0 ? step(-1, 1) : -1, false)
    }

    // The next row shown that can be chosen from `from` going `by` (1 or -1); -1: none.
    function step(from, by) {
        for (let i = from + by; i >= 0 && i < shown.length; i += by) {
            if (rows[shown[i]].enabled)
                return i
        }
        return -1
    }

    function highlight(index, scroll) {
        current = index
        if (index >= 0 && scroll)
            list.positionViewAtIndex(index, ListView.Contain)
    }

    function choose(index) {
        const row = rows[shown[index]]
        if (!row || !row.enabled)
            return
        menu.dismiss()
        run(row)
    }

    function indexOf(item) {
        for (let i = 0; i < menu.count; ++i) {
            if (menu.itemAt(i) === item)
                return i
        }
        return -1
    }

    // The menu's entry (that can be chosen) just above it, or just below it (null: none).
    function entryAbove() {
        for (let i = indexOf(search) - 1; i >= 0; --i) {
            const item = menu.itemAt(i)
            if (item instanceof T.MenuItem && item.enabled)
                return item
        }
        return null
    }
    function entryBelow() {
        for (let i = indexOf(search) + 1; i < menu.count; ++i) {
            const item = menu.itemAt(i)
            if (item instanceof T.MenuItem && item.enabled)
                return item
        }
        return null
    }

    // The keyboard comes back here, none of the menu's own entries highlighted.
    function takeKeyboard() {
        menu.currentIndex = -1  // (first: it clears the focus of the entry it leaves)
        field.forceActiveFocus()
    }

    // Whether a key types something: a character, without Ctrl or Alt (AltGr, both, types).
    function typed(event) {
        if (event.text === "" || event.text.charCodeAt(0) < 0x20 || event.text.charCodeAt(0) === 0x7f)
            return false
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        const alt = (event.modifiers & Qt.AltModifier) !== 0
        return ctrl === alt
    }

    // A key pressed on one of the menu's own entries (Space types too: Enter is the entry's).
    function keyPressed(item, event) {
        if (event.key === Qt.Key_Down && item === entryAbove()) {
            takeKeyboard()
            highlight(step(-1, 1), true)
            event.accepted = true
        } else if (event.key === Qt.Key_Up && item === entryBelow()) {
            takeKeyboard()
            highlight(step(shown.length, -1), true)
            event.accepted = true
        } else if (event.key === Qt.Key_Backspace) {
            takeKeyboard()
            if (field.selectedText !== "")
                erase(field.selectionStart, field.selectionEnd)
            else if (field.cursorPosition > 0)
                erase(field.cursorPosition - 1, field.cursorPosition)
            event.accepted = true
        } else if (typed(event)) {
            takeKeyboard()
            if (field.selectedText !== "")
                erase(field.selectionStart, field.selectionEnd)
            const at = field.cursorPosition
            field.insert(at, event.text)
            field.cursorPosition = at + event.text.length  // (insert() leaves it before what it inserts)
            event.accepted = true
        }
    }

    function erase(start, end) {
        field.remove(start, end)
        field.cursorPosition = start
    }

    // (Made for a menu open already: once it is in it.)
    Component.onCompleted: {
        if (menu.opened)
            Qt.callLater(takeKeyboard)
    }

    Connections {
        target: search.menu

        function onOpened() {
            search.takeKeyboard()
        }
    }

    FontMetrics {
        id: metrics
        font: probe.font
    }
    // A row as the list makes them, measured.
    MenuItem {
        id: probe
        visible: false
        text: "X"
    }


    // The wheel over the field, or over a list that can't scroll further: not
    // passed on to what is under the menu.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.NoButton
        onWheel: wheel => wheel.accepted = true
    }

    TextField {
        id: field
        objectName: "menuSearch"
        x: 2
        y: 3
        width: search.width - 4
        placeholderText: qsTr("Search")

        onTextChanged: search.filter()
        onAccepted: {
            const index = search.current >= 0 ? search.current : (text.trim() !== "" ? search.step(-1, 1) : -1)
            if (index >= 0)
                search.choose(index)
        }
        Keys.onDownPressed: {
            const next = search.step(search.current, 1)
            if (next >= 0) {
                search.highlight(next, true)
            } else {
                const below = search.entryBelow()
                if (below)
                    search.menu.currentIndex = search.indexOf(below)
            }
        }
        Keys.onUpPressed: {
            const previous = search.current >= 0 ? search.step(search.current, -1) : -1
            if (previous >= 0) {
                search.highlight(previous, true)
            } else {
                const above = search.entryAbove()
                if (above)
                    search.menu.currentIndex = search.indexOf(above)
            }
        }
        Keys.onPressed: event => {
            if (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp) {
                const by = event.key === Qt.Key_PageDown ? 1 : -1
                let index = search.current
                for (let moved = 0; moved < search.maxRows - 1; ++moved) {
                    const next = search.step(index, by)
                    if (next < 0)
                        break
                    index = next
                }
                if (index >= 0)
                    search.highlight(index, true)
                event.accepted = true
            }
        }
        onActiveFocusChanged: {
            if (activeFocus && search.menu.currentIndex !== -1)
                Qt.callLater(search.takeKeyboard)  // (clicked with one of the menu's entries highlighted)
            else if (!activeFocus)
                search.current = -1  // (one of the menu's entries has the keyboard: one highlight)
        }
    }

    ListView {
        id: list
        objectName: "menuSearchList"
        y: field.y + field.height + 3
        width: search.width
        // As tall as its rows, up to maxRows of them; the same while filtering.
        height: Math.min(search.rows.length, search.maxRows) * search.rowHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height
        model: search.shown

        ScrollBar.vertical: ScrollBar {
            policy: list.interactive ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
        }

        delegate: Item {
            id: made

            required property int index
            required property int modelData
            readonly property Item row: loader.item as Item

            width: list.width
            height: search.rowHeight

            Loader {
                id: loader
                anchors.fill: parent
                sourceComponent: search.rowComponent
            }
            Binding {
                target: loader.item
                property: "entry"
                value: search.rows[made.modelData]
            }
            Binding {
                target: loader.item
                property: "highlighted"
                value: made.index === search.current
            }
            Binding {
                target: loader.item
                property: "focusPolicy"
                value: Qt.NoFocus
            }
            // Clicked: chosen here, not by the row (its own click would tick it, or untick it, first).
            MouseArea {
                anchors.fill: parent
                onClicked: search.choose(made.index)
            }
        }

        // The mouse moving over the rows highlights the one under it (as a
        // menu's entries), not the list scrolling under a mouse at rest: the
        // arrows keep the row they moved to.
        HoverHandler {
            id: rowHover

            property point last: Qt.point(-1, -1)

            onPointChanged: {
                const at = point.position
                if (!hovered || (at.x === last.x && at.y === last.y))
                    return
                last = at
                const index = list.indexAt(at.x + list.contentX, at.y + list.contentY)
                if (index >= 0 && search.rows[search.shown[index]].enabled) {
                    search.takeKeyboard()
                    search.highlight(index, false)
                }
            }
        }

        Text {
            x: probe.leftPadding
            height: search.rowHeight
            visible: search.rows.length > 0 && search.shown.length === 0
            verticalAlignment: Text.AlignVCenter
            text: qsTr("No match")
            font: probe.font
            color: Theme.textDim
        }
    }
}
