import QtQuick
import QtQuick.Controls
import SUBstation

// The EQ curve's right-click menus (eq.py's EqGraph._band_menu and _view_menu),
// made for how things are when they open: a band's (its type, slope and
// placement, Enabled, Delete Band) and the background's (the analyzer's mode,
// the curve's range, Delete All Bands).
Item {
    id: menus

    required property EqGraph graph
    readonly property alias bandMenu: bandMenu
    readonly property alias viewMenu: viewMenu

    // A band's menu, filled for it.
    function buildBand(index) {
        const g = graph
        const band = g.band(index)
        bandMenu.clear()
        if (band.index === undefined)
            return false
        const types = bandMenu.submenu(qsTr("Type"))
        for (let kind = 0; kind < g.types.length; ++kind) {
            const k = kind
            bandMenu.entry(g.types[kind], () => g.setType(index, k), kind === band.type, true, types)
        }
        if (g.hasSlope(band.type)) {
            const slopes = bandMenu.submenu(qsTr("Slope"))
            for (let i = 0; i < g.slopes.length; ++i) {
                const s = i
                bandMenu.entry(g.slopes[i], () => g.setBandParam(index, "slope", s, "", "Change EQ Band Slope"),
                               i === band.slope, true, slopes)
            }
        }
        const places = bandMenu.submenu(qsTr("Placement"))
        for (let i = 0; i < g.places.length; ++i) {
            const p = i
            bandMenu.entry(g.places[i], () => g.setBandParam(index, "place", p, "", "Change EQ Band Placement"),
                           i === band.place, true, places)
        }
        bandMenu.separator()
        bandMenu.entry(qsTr("Enabled"), () => g.toggleBand(index), band.on)
        bandMenu.entry(qsTr("Delete Band"), () => g.removeBand(index))
        return true
    }

    // The background's menu, filled.
    function buildView() {
        const g = graph
        viewMenu.clear()
        for (let i = 0; i < EqView.analyzerModes.length; ++i) {
            const mode = i
            viewMenu.entry(EqView.analyzerModes[i], () => EqView.analyzer = mode, i === EqView.analyzer)
        }
        viewMenu.separator()
        for (const value of EqView.ranges) {
            const range = value
            viewMenu.entry(qsTr("Range ± %1 dB").arg(value), () => EqView.range = range, value === EqView.range)
        }
        if (g.hasBands) {
            viewMenu.separator()
            viewMenu.entry(qsTr("Delete All Bands"), () => g.removeAll())
        }
    }

    Connections {
        target: menus.graph
        function onBandMenuRequested(band, position) {
            if (menus.buildBand(band))
                bandMenu.popup(menus.graph, position.x, position.y)
        }
        function onViewMenuRequested(position) {
            menus.buildView()
            viewMenu.popup(menus.graph, position.x, position.y)
        }
    }

    DynamicMenu {
        id: bandMenu
    }
    DynamicMenu {
        id: viewMenu
    }
}
