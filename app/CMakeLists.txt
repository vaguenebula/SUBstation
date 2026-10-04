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
target_link_libraries(sub_app PUBLIC Qt6::Core Qt6::Gui sub_engine)
# Qt's keywords (signals, slots, emit, foreach) are macros that break other
# code's identifiers (the engine has `slots`): the app layer, and everything
# built on it, writes Q_SIGNALS, Q_SLOTS and Q_EMIT.
target_compile_definitions(sub_app PUBLIC QT_NO_KEYWORDS)
if (TARGET sub_browser)
    target_link_libraries(sub_app PUBLIC sub_browser)
endif()
sub_project_warnings(sub_app)
