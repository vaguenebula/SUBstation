# The application layer: project state (tracks, clips, devices, automation),
# editing and undo, saving, the engine's mirror of the project, the transport,
# recording, the browser's and the plug-ins' models, preferences.
#
# Qt Core and Qt Gui only (QUndoStack lives in Gui): never Qt Quick or QML, so
# the UI (ui/) is the only layer that knows how things are shown. It drives the
# engine (sub_engine) through Engine.h, and the browser backend (sub_browser).
#
# Sources are globbed: a new file under src/ is picked up on the next configure.
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
qt_add_library(sub_app STATIC ${APP_SOURCES})
add_library(sub::app ALIAS sub_app)
target_include_directories(sub_app PUBLIC src)
# The engine's built-in devices register themselves from objects nothing else
# refers to (engine/src/builtin/devices/), which a linker drops from a static
# library: whatever links the application layer takes the whole engine, so
# BuiltinRegistry knows every device.
target_link_libraries(sub_app PUBLIC Qt6::Core Qt6::Gui "$<LINK_LIBRARY:WHOLE_ARCHIVE,sub_engine>")
if (TARGET sub_browser)
    target_link_libraries(sub_app PUBLIC sub_browser)
endif()
sub_project_warnings(sub_app)
