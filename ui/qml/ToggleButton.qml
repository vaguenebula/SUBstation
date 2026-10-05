import QtQuick
import SUBstation

// A checkable button that never takes keyboard focus, so Space stays play/stop;
// `role` picks its checked colour (RoleButton). `toggled()` and `clicked()`
// come from the user only. To show state that came from the model or the
// engine, set `checked` (a binding) or call setCheckedSilently(): neither emits
// toggled(). For a button whose checked state follows the engine rather than
// the click (Play, Record), set `checkable: false` and bind `checked`.
RoleButton {
    checkable: true

    function setCheckedSilently(on) {
        checked = on
    }
}
