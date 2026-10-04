import QtQuick
import SUBstation

// QPushButton as the old stylesheet drew it (RoleButton has the look, with no
// role here; `flat` is the "flat" role). Unlike the role buttons in the views,
// a plain button takes the focus, as a dialog's buttons do.
RoleButton {
    role: flat ? "flat" : ""
    lit: checked || highlighted
    focusPolicy: Qt.StrongFocus
}
