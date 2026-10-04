# The layers' boundaries, checked as a test (ctest -R boundaries):
#  * the audio engine (engine/src) and the browser backend (browser/src) never
#    include Qt or anything of the application or the UI;
#  * the application layer (app/src) never includes Qt Quick or QML, nor the UI.
# Run as: cmake -DROOT=<source dir> -P CheckBoundaries.cmake
set(_failures "")

function(_check folder pattern what)
    file(GLOB_RECURSE _files "${ROOT}/${folder}/*.h" "${ROOT}/${folder}/*.hpp" "${ROOT}/${folder}/*.cpp" "${ROOT}/${folder}/*.inc")
    foreach (_file IN LISTS _files)
        file(STRINGS "${_file}" _lines REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](${pattern})")
        if (_lines)
            file(RELATIVE_PATH _rel "${ROOT}" "${_file}")
            list(APPEND _failures "${_rel} includes ${what}: ${_lines}")
            set(_failures "${_failures}" PARENT_SCOPE)
        endif()
    endforeach()
endfunction()

_check(engine/src "Q[A-Za-z]+|qt|app/|ui/" "Qt or the application")
_check(browser/src "Q[A-Za-z]+|qt|app/|ui/" "Qt or the application")
_check(app/src "QtQuick|QtQml|QQuick|QQml|QJSValue|QJSEngine|ui/" "Qt Quick, QML or the UI")

if (_failures)
    list(JOIN _failures "\n  " _text)
    message(FATAL_ERROR "Layer boundaries crossed:\n  ${_text}")
endif()
message(STATUS "Layer boundaries hold")
