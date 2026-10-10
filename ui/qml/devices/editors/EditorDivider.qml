import QtQuick
import SUBstation

// The thin upright line between a device editor's sections, 6 px from its top and bottom (place
// it with x; its parent is the editor).
Rectangle {
    y: 6
    width: 1
    height: parent ? parent.height - 12 : 0
    color: Theme.border
}
