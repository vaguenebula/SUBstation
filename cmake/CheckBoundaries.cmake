# The layers' boundaries, checked as a test (ctest -R boundaries):
#  * the audio engine (engine/src) and the browser backend (browser/src) never
#    include Qt or anything of the application or the UI;
#  * the application layer (app/src) never includes Qt Quick or QML, nor the UI;
#  * the UI (ui/) never includes the engine's headers: it talks to the
#    application layer only.
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

# The engine's headers, by name (as the engine includes them: "Engine.h", "plugins/Vst3Format.h"...).
file(GLOB_RECURSE _engine_headers RELATIVE "${ROOT}/engine/src" "${ROOT}/engine/src/*.h")
set(_engine_names "")
foreach (_header IN LISTS _engine_headers)
    string(REGEX REPLACE "([.+])" "\\\\\\1" _escaped "${_header}")
    list(APPEND _engine_names "${_escaped}")
endforeach()
list(JOIN _engine_names "|" _engine_pattern)
if (_engine_pattern)
    _check(ui "(${_engine_pattern})[\">]|miniaudio|pluginterfaces|public\\.sdk" "the engine")
endif()

if (_failures)
    list(JOIN _failures "\n  " _text)
    message(FATAL_ERROR "Layer boundaries crossed:\n  ${_text}")
endif()
message(STATUS "Layer boundaries hold")
