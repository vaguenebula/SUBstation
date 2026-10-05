import QtQuick
import SUBstation

// A small button on a device's title bar (frame.py's _header_button): 16 px
// square, the "device-header" look (no background but under the mouse, the
// accent while checked), its icon 5 px smaller or its text ("‹", "›") in 11 pt.
RoleButton {
    role: "device-header"
    implicitWidth: 16
    implicitHeight: 16
    width: 16
    height: 16
    iconSize: 11
    font.pointSize: 11
}
