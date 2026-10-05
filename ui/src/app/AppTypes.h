#pragma once
// Makes the application layer known to QML: the session as the `Session`
// singleton (import SUBstation), and the application layer's types it hands
// out (the project, the editor, the selection, the bridge, the browser's and
// plug-ins' models...) so QML can read their properties and call their
// invokables. The application layer itself knows nothing of QML.

namespace sub::app {
class Session;
}

namespace sub::ui {

// Before the QML that uses `Session` is loaded; `session` outlives the engines.
void registerSession(sub::app::Session* session);

}  // namespace sub::ui
