cmake_minimum_required(VERSION 3.26)
project(gilstudio_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT WIN32)
    message(WARNING "GIL Studio targets Windows (WASAPI); other platforms are untested.")
endif()

option(GILSTUDIO_TEST_PLUGINS "Build the VST3 plug-ins the tests use" ON)

find_package(Python 3.12 COMPONENTS Interpreter Development.Module REQUIRED)

execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE nanobind_ROOT)
find_package(nanobind CONFIG REQUIRED)

# miniaudio is compiled once, as C, in its own translation unit.
add_library(miniaudio STATIC engine/src/miniaudio_impl.c)
target_include_directories(miniaudio PUBLIC engine/third_party/miniaudio)
target_compile_definitions(miniaudio PUBLIC
    MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_GENERATION)
set_target_properties(miniaudio PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (MSVC)
    target_compile_options(miniaudio PRIVATE /W0)
endif()

# --- VST3 SDK (MIT; the parts we use are vendored in engine/third_party/vst3sdk) ---
set(VST3_SDK ${CMAKE_CURRENT_SOURCE_DIR}/engine/third_party/vst3sdk)

function(gil_vst3_sdk_library name)
    add_library(${name} STATIC ${ARGN})
    target_include_directories(${name} SYSTEM PUBLIC ${VST3_SDK})
    target_compile_definitions(${name} PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if (MSVC)
        target_compile_options(${name} PRIVATE /W0 /utf-8 /Zc:__cplusplus)
    endif()
endfunction()

# Interfaces, the SDK's base library and helpers both sides use.
gil_vst3_sdk_library(vst3_base
    ${VST3_SDK}/base/source/baseiids.cpp
    ${VST3_SDK}/base/source/fbuffer.cpp
    ${VST3_SDK}/base/source/fdebug.cpp
    ${VST3_SDK}/base/source/fdynlib.cpp
    ${VST3_SDK}/base/source/fobject.cpp
    ${VST3_SDK}/base/source/fstreamer.cpp
    ${VST3_SDK}/base/source/fstring.cpp
    ${VST3_SDK}/base/source/timer.cpp
    ${VST3_SDK}/base/source/updatehandler.cpp
    ${VST3_SDK}/base/thread/source/fcondition.cpp
    ${VST3_SDK}/base/thread/source/flock.cpp
    ${VST3_SDK}/pluginterfaces/base/conststringtable.cpp
    ${VST3_SDK}/pluginterfaces/base/coreiids.cpp
    ${VST3_SDK}/pluginterfaces/base/funknown.cpp
    ${VST3_SDK}/pluginterfaces/base/ustring.cpp
    ${VST3_SDK}/public.sdk/source/common/commoniids.cpp
    ${VST3_SDK}/public.sdk/source/common/commonstringconvert.cpp
    ${VST3_SDK}/public.sdk/source/common/memorystream.cpp
    ${VST3_SDK}/public.sdk/source/common/threadchecker_win32.cpp
    ${VST3_SDK}/public.sdk/source/vst/utility/stringconvert.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstinitiids.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstpresetfile.cpp)

# The host side: loading modules, host context, component/controller connection.
gil_vst3_sdk_library(vst3_hosting
    ${VST3_SDK}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/module.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/module_win32.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp)
target_link_libraries(vst3_hosting PUBLIC vst3_base ole32 shell32)
# As C++17 the module loader reads paths as UTF-8 (u8path); as C++20 it would
# use the ANSI code page, and plug-ins with non-ASCII paths would not load.
set_target_properties(vst3_hosting PROPERTIES CXX_STANDARD 17)

nanobind_add_module(_engine NB_STATIC
    engine/src/bindings.cpp
    engine/src/Engine.cpp
    engine/src/AudioDevice.cpp
    engine/src/AudioSource.cpp
    engine/src/Renderer.cpp
    engine/src/Metronome.cpp
    engine/src/Warp.cpp
    engine/src/processors/Synth.cpp
    engine/src/plugins/EditorWindow.cpp
    engine/src/plugins/Vst3Format.cpp
    engine/src/plugins/Vst3Processor.cpp
)
target_include_directories(_engine PRIVATE engine/src)
# Signalsmith Stretch (time stretching / pitch shifting) and its FFT library:
# MIT-licensed, header-only, no further dependencies.
target_include_directories(_engine SYSTEM PRIVATE
    engine/third_party/signalsmith-stretch
    engine/third_party/signalsmith-linear)
target_link_libraries(_engine PRIVATE miniaudio vst3_hosting user32)
target_compile_definitions(_engine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
if (MSVC)
    target_compile_options(_engine PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
endif()

install(TARGETS _engine LIBRARY DESTINATION gilstudio)

# --- Test plug-ins ------------------------------------------------------------
# One VST3 bundle with a test instrument and effects (tests/vst3_plugins), installed
# next to the engine so the tests can find it.
if (GILSTUDIO_TEST_PLUGINS)
    gil_vst3_sdk_library(vst3_plugin_sdk
        ${VST3_SDK}/public.sdk/source/common/pluginview.cpp
        ${VST3_SDK}/public.sdk/source/main/pluginfactory.cpp
        ${VST3_SDK}/public.sdk/source/vst/vstaudioeffect.cpp
        ${VST3_SDK}/public.sdk/source/vst/vstbus.cpp
        ${VST3_SDK}/public.sdk/source/vst/vstcomponent.cpp
        ${VST3_SDK}/public.sdk/source/vst/vstcomponentbase.cpp
        ${VST3_SDK}/public.sdk/source/vst/vsteditcontroller.cpp
        ${VST3_SDK}/public.sdk/source/vst/vstparameters.cpp
        ${VST3_SDK}/public.sdk/source/vst/vstsinglecomponenteffect.cpp)
    target_link_libraries(vst3_plugin_sdk PUBLIC vst3_base)
    # vsteditcontroller.cpp is compiled on its own, not inside vstsinglecomponenteffect.cpp.
    target_compile_definitions(vst3_plugin_sdk PRIVATE PROJECT_INCLUDES_VSTEDITCONTROLLER=1)

    add_library(gil_test_plugins MODULE
        tests/vst3_plugins/test_plugins.cpp
        tests/vst3_plugins/test_effect.cpp
        ${VST3_SDK}/public.sdk/source/main/dllmain.cpp)
    target_link_libraries(gil_test_plugins PRIVATE vst3_plugin_sdk user32 gdi32)
    target_compile_definitions(gil_test_plugins PRIVATE UNICODE _UNICODE NOMINMAX)
    set_target_properties(gil_test_plugins PROPERTIES OUTPUT_NAME "GILTestPlugins" PREFIX "" SUFFIX ".vst3")
    if (MSVC)
        target_compile_options(gil_test_plugins PRIVATE /W3 /utf-8 /Zc:__cplusplus)
    endif()
    install(TARGETS gil_test_plugins
            LIBRARY DESTINATION gilstudio/_testplugins/GILTestPlugins.vst3/Contents/x86_64-win)
endif()
