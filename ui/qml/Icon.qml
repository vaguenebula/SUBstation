import QtQuick
import SUBstation

// One of the icons (theme/Icons.h) at `size`: drawn for its size by the image
// provider, in its own colour unless `color` is set, its On picture when
// `checked` (lock_envelopes: closed; fold: folded), and the disabled variant
// while disabled.
Image {
    id: icon

    property string name
    property var color: undefined
    property bool checked: false
    property real size: Theme.iconSize

    width: size
    height: size
    sourceSize: Qt.size(size, size)
    fillMode: Image.PreserveAspectFit
    smooth: true
    // (Theme.name, never empty: drawn again in a new theme's colours.)
    source: name && Theme.name ? Icons.url(name, color, checked, !enabled) : ""
}
