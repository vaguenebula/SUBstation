import QtQuick
import SUBstation

// One chain of the track the device view shows, left to right: a frame per
// device and, after a rack that isn't folded and has chains, the chain it shows
// (the one last clicked in its chain list, else its first) in its bracket, with
// its devices, and so on inside. The frames are made again only when the
// chain's devices change (DeviceChainList).
Row {
    id: chain

    property string chainId  // "": the track's own
    property var panel
    readonly property alias list: list
    readonly property int count: list.count

    spacing: 6

    DeviceChainList {
        id: list
        session: Session
        chainId: chain.chainId
    }

    Repeater {
        model: list.deviceIds

        // The device, then (a rack showing a chain) that chain beside it.
        Row {
            id: entry

            required property string modelData

            height: chain.height
            spacing: 6

            DeviceFrame {
                id: frame
                height: entry.height
                trackId: list.trackId
                deviceId: entry.modelData
                chainId: chain.chainId
                panel: chain.panel
            }

            Loader {
                id: shown
                height: entry.height
                active: frame.showsChain
                visible: active
                width: item ? item.implicitWidth : 0

                // (By its file: a chain shows chains in turn.)
                function load() {
                    if (active)
                        setSource(Qt.resolvedUrl("RackChainView.qml"), { panel: chain.panel, chainId: frame.shownChain })
                    else
                        source = ""
                }
                onActiveChanged: load()
                Component.onCompleted: load()
                onLoaded: item.chainId = Qt.binding(() => frame.shownChain)
            }
        }
    }
}
