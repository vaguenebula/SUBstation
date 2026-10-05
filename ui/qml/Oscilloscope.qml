import QtQuick
import QtQuick.Controls
import SUBstation

// The master output's oscilloscope (OscilloscopeItem), 150 x 30, with its
// tooltip. Give it a `feed` (scopeWritten, scopeSamples()).
OscilloscopeItem {
    HoverHandler {
        id: hover
    }

    ToolTip.visible: hover.hovered
    ToolTip.text: qsTr("Master output")
    ToolTip.delay: 700
}
