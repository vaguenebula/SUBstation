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
# The QML type registration includes each QML_ELEMENT's header by its file name
# alone, so every folder under src/ is an include directory of the library too.
set(_ui_dirs "")
foreach (_source IN LISTS UI_SOURCES)
    get_filename_component(_dir ${_source} DIRECTORY)
    list(APPEND _ui_dirs ${_dir})
endforeach()
list(REMOVE_DUPLICATES _ui_dirs)
target_include_directories(sub_ui PRIVATE ${_ui_dirs})

# The Qt Quick Controls style (style/): the standard controls (buttons, menus,
# combo boxes, scroll bars, split views, text fields, tool tips, dialogs) drawn
# as the old stylesheet drew them, from the SUBstation module's Theme; the
# Basic style for the rest. sub::ui::setUpApplication() selects it.
file(GLOB UI_STYLE_QML RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS style/*.qml)
qt_add_library(sub_ui_style STATIC)
qt_add_qml_module(sub_ui_style
    URI SUBstation.Style
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/SUBstation/Style
    QML_FILES ${UI_STYLE_QML}
    IMPORTS QtQuick.Controls.Basic
    NO_PLUGIN_OPTIONAL)
target_link_libraries(sub_ui_style PUBLIC Qt6::Quick Qt6::QuickControls2)
target_link_libraries(sub_ui PUBLIC sub_ui_styleplugin)
target_link_libraries(sub_ui PUBLIC sub_app Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
sub_project_warnings(sub_ui)
if (MSVC)
    # C4458: a local named like a base's member. Every Qt Quick item inherits
    # QQmlParserStatus's private d-pointer `d`, which any local `d` "hides".
    target_compile_options(sub_ui PRIVATE /wd4458)
endif()

# The application.
qt_add_executable(substation main.cpp)
target_link_libraries(substation PRIVATE sub_ui sub_uiplugin)
if (WIN32)
    target_link_libraries(substation PRIVATE shell32)  # SetCurrentProcessExplicitAppUserModelID
endif()
add_dependencies(substation substation-scan)
set_target_properties(substation PROPERTIES WIN32_EXECUTABLE ON)
sub_project_warnings(substation)
