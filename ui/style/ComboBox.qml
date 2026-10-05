import QtQuick
import QtQuick.Templates as T
import SUBstation

// QComboBox: SURFACE with a BORDER line, 3 px corners, its list on PANEL_ALT
// with the highlighted row in ACCENT. Combo boxes in the views set
// focusPolicy: Qt.NoFocus, as the old ones did (Space stays play/stop).
T.ComboBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    // padding: 3px 8px, inside a 1 px border; the arrow on the right.
    topPadding: 4
    bottomPadding: 4
    leftPadding: 9
    rightPadding: 9 + (indicator ? indicator.width + spacing : 0)
    spacing: 6
    hoverEnabled: true
    // As wide as its widest item, as QComboBox.
    implicitContentWidthPolicy: T.ComboBox.WidestText

    delegate: ItemDelegate {
        required property var model
        required property int index
        width: ListView.view ? ListView.view.width : implicitWidth
        text: control.textRole ? (Array.isArray(control.model) ? model.modelData[control.textRole]
                                                               : model[control.textRole])
                               : model.modelData
        highlighted: control.highlightedIndex === index
        hoverEnabled: control.hoverEnabled
    }

    indicator: Icon {
        x: control.width - width - 8
        y: control.topPadding + (control.availableHeight - height) / 2
        name: "fold"
        color: Theme.text
        size: 9
    }

    contentItem: T.TextField {
        text: control.editable ? control.editText : control.displayText
        enabled: control.editable
        autoScroll: control.editable
        readOnly: control.down
        inputMethodHints: control.inputMethodHints
        validator: control.validator
        selectByMouse: control.selectTextByMouse
        font: control.font
        color: control.enabled ? Theme.text : Theme.textDisabled
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentText
        verticalAlignment: Text.AlignVCenter
        padding: 0
    }

    background: Rectangle {
        implicitWidth: 60
        implicitHeight: 22
        radius: Theme.radius
        color: control.down ? Theme.panel : Theme.surface
        border.color: control.visualFocus ? Theme.accent : Theme.border
    }

    popup: T.Popup {
        y: control.height
        width: Math.max(control.width, contentItem.implicitWidth + leftPadding + rightPadding)
        height: Math.min(contentItem.implicitHeight + topPadding + bottomPadding,
                         control.Window.height - topMargin - bottomMargin)
        topMargin: 6
        bottomMargin: 6
        padding: 1

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            implicitWidth: {
                let widest = 0
                for (let i = 0; i < count; ++i) {
                    const item = itemAtIndex(i)
                    if (item)
                        widest = Math.max(widest, item.implicitWidth)
                }
                return widest
            }
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            highlightMoveDuration: 0
            T.ScrollIndicator.vertical: ScrollIndicator {}
        }

        background: Rectangle {
            color: Theme.panelAlt
            border.color: Theme.border
        }
    }
}
