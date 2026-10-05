import QtQuick
import SUBstation

// Under a parameter's control (or its whole cell): a left press touches the
// parameter, so the arrangement shows its automation, and goes on to what is
// below (the device: selecting it); a right press opens its menu (ParamMenu).
// Controls that take the left button themselves touch the parameter on their
// own press.
MouseArea {
    id: area

    property DeviceParam param: null
    readonly property alias menu: menu

    acceptedButtons: Qt.LeftButton | Qt.RightButton
    onPressed: mouse => {
        if (!param)
            mouse.accepted = false
        else if (mouse.button === Qt.RightButton)
            menu.show()
        else {
            param.touch()
            mouse.accepted = false
        }
    }

    ParamMenu {
        id: menu
        param: area.param
    }
}
