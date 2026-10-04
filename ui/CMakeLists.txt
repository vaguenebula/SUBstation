# The UI: Qt Quick. QML lays out and styles everything; what draws time (the
# arrangement, the piano roll, envelopes, waveforms, meters, curves) is C++
# scene-graph items (src/), rendered on the GPU. The UI reads the application
# layer's models and calls its editor and controllers; it never changes the
# project itself and never talks to the engine.
#
# QML files and sources are globbed: a new file is picked up on the next configure.
# A QML file starting with "pragma Singleton" is registered as a singleton.
file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
file(GLOB_RECURSE UI_QML RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS qml/*.qml)
file(GLOB_RECURSE UI_RESOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS qml/*.svg qml/*.png)
foreach (_qml IN LISTS UI_QML)
    file(STRINGS ${_qml} _singleton REGEX "^pragma Singleton")
    if (_singleton)
        set_source_files_properties(${_qml} PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
    endif()
endforeach()

qt_add_library(sub_ui STATIC)
qt_add_qml_module(sub_ui
    URI SUBstation
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/SUBstation
    QML_FILES ${UI_QML}
    RESOURCES ${UI_RESOURCES}
    SOURCES ${UI_SOURCES}
    NO_PLUGIN_OPTIONAL)
target_include_directories(sub_ui PUBLIC src)
target_link_libraries(sub_ui PUBLIC sub_app Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
sub_project_warnings(sub_ui)

# The application.
qt_add_executable(substation main.cpp)
target_link_libraries(substation PRIVATE sub_ui sub_uiplugin)
add_dependencies(substation substation-scan)
set_target_properties(substation PROPERTIES WIN32_EXECUTABLE ON)
sub_project_warnings(substation)
