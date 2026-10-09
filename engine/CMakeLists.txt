# The real-time audio engine: DSP, the routing graph, VST3 hosting, MIDI input,
# recording, rendering and real-time scheduling. A static library with no Qt
# (and no other UI) dependency: the application layer (app/) drives it through
# Engine.h. tests/boundaries.cmake checks that nothing here includes Qt.

# miniaudio is compiled once, as C, in its own translation unit.
add_library(miniaudio STATIC src/miniaudio_impl.c)
target_include_directories(miniaudio SYSTEM PUBLIC third_party/miniaudio)
target_compile_definitions(miniaudio PUBLIC
    MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_GENERATION)
set_target_properties(miniaudio PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Signalsmith Linear's FFTs (header-only, MIT): Signalsmith Stretch's, and the
# intelligence module's (intelligence/).
add_library(signalsmith_linear INTERFACE)
target_include_directories(signalsmith_linear SYSTEM INTERFACE third_party/signalsmith-linear)
if (MSVC)
    target_compile_options(miniaudio PRIVATE /W0)
else()
    target_compile_options(miniaudio PRIVATE -w)
    target_link_libraries(miniaudio PUBLIC ${CMAKE_DL_LIBS} Threads::Threads m)
endif()

# --- VST3 SDK (MIT; the parts we use are vendored in third_party/vst3sdk) --------
set(VST3_SDK ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk CACHE INTERNAL "The vendored VST 3 SDK")

function(sub_vst3_sdk_library name)
    add_library(${name} STATIC ${ARGN})
    target_include_directories(${name} SYSTEM PUBLIC ${VST3_SDK})
    target_compile_definitions(${name} PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if (MSVC)
        target_compile_options(${name} PRIVATE /W0 /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(${name} PRIVATE -w)
    endif()
endfunction()

if (WIN32)
    set(_vst3_threadchecker ${VST3_SDK}/public.sdk/source/common/threadchecker_win32.cpp)
    set(_vst3_module_loader ${VST3_SDK}/public.sdk/source/vst/hosting/module_win32.cpp)
else()
    set(_vst3_threadchecker ${VST3_SDK}/public.sdk/source/common/threadchecker_linux.cpp)
    set(_vst3_module_loader ${VST3_SDK}/public.sdk/source/vst/hosting/module_linux.cpp)
endif()

# Interfaces, the SDK's base library and helpers both sides use.
sub_vst3_sdk_library(vst3_base
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
    ${_vst3_threadchecker}
    ${VST3_SDK}/public.sdk/source/vst/utility/stringconvert.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstinitiids.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstpresetfile.cpp)
if (NOT WIN32)
    target_link_libraries(vst3_base PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

# The host side: loading modules, host context, component/controller connection.
sub_vst3_sdk_library(vst3_hosting
    ${VST3_SDK}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/module.cpp
    ${_vst3_module_loader}
    ${VST3_SDK}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp)
target_link_libraries(vst3_hosting PUBLIC vst3_base)
if (WIN32)
    target_link_libraries(vst3_hosting PUBLIC ole32 shell32)
endif()
# As C++17 the module loader reads paths as UTF-8 (u8path); as C++20 it would
# use the ANSI code page, and plug-ins with non-ASCII paths would not load.
set_target_properties(vst3_hosting PROPERTIES CXX_STANDARD 17)

# --- ASIO SDK (optional, Windows) ------------------------------------------------
# Steinberg's ASIO SDK is not part of the repository: its licence doesn't allow
# passing it on. Unzip it (https://www.steinberg.net/asiosdk) into the project
# folder as it comes (.gitignore keeps asiosdk* folders out of git), or point
# SUBSTATION_ASIO_SDK (a CMake or environment variable) at it. Only its headers
# are used. Without it the engine is built with WASAPI only.
set(SUBSTATION_ASIO_SDK "$ENV{SUBSTATION_ASIO_SDK}" CACHE PATH "Steinberg's ASIO SDK: the folder containing common/iasiodrv.h")
set(ASIO_SDK "")
if (SUBSTATION_ASIO AND WIN32)
    if (SUBSTATION_ASIO_SDK)
        if (NOT EXISTS "${SUBSTATION_ASIO_SDK}/common/iasiodrv.h")
            message(FATAL_ERROR "SUBSTATION_ASIO_SDK (${SUBSTATION_ASIO_SDK}) has no common/iasiodrv.h")
        endif()
        set(ASIO_SDK "${SUBSTATION_ASIO_SDK}")
    else()
        # Any folder in the project folder, or one inside it (some unzip tools add
        # a level); else an asio* folder beside the project or in the drive's root.
        set(_asio_patterns
            "${PROJECT_SOURCE_DIR}/*/common/iasiodrv.h"
            "${PROJECT_SOURCE_DIR}/*/*/common/iasiodrv.h"
            "${PROJECT_SOURCE_DIR}/../asio*/common/iasiodrv.h"
            "${PROJECT_SOURCE_DIR}/../asio*/*/common/iasiodrv.h")
        if (DEFINED ENV{SystemDrive})
            list(APPEND _asio_patterns "$ENV{SystemDrive}/asio*/common/iasiodrv.h" "$ENV{SystemDrive}/asio*/*/common/iasiodrv.h")
        endif()
        file(GLOB _asio_headers ${_asio_patterns})
        if (_asio_headers)
            list(SORT _asio_headers ORDER DESCENDING)  # the newest of several (asiosdk_2.3.3_... after asiosdk2.3)
            list(GET _asio_headers 0 _asio_header)
            cmake_path(GET _asio_header PARENT_PATH _asio_common)
            cmake_path(GET _asio_common PARENT_PATH ASIO_SDK)
            cmake_path(NORMAL_PATH ASIO_SDK)
        endif()
    endif()
    if (ASIO_SDK)
        message(STATUS "ASIO SDK: ${ASIO_SDK}")
    else()
        message(WARNING "ASIO SDK not found, so the engine is built without ASIO. Unzip the SDK from "
                        "https://www.steinberg.net/asiosdk into ${PROJECT_SOURCE_DIR} or set SUBSTATION_ASIO_SDK.")
    endif()
endif()
set(SUBSTATION_ASIO_SDK_DIR "${ASIO_SDK}" CACHE INTERNAL "The ASIO SDK found (empty: none)")

# --- The engine ---------------------------------------------------------------------
# Every built-in device is one .cpp in builtin/devices/; a new file is picked up on the next configure.
file(GLOB BUILTIN_DEVICE_SOURCES CONFIGURE_DEPENDS src/builtin/devices/*.cpp)
# Each defines an empty function named after its file, which BuiltinDevices.cpp
# calls: or a program linking the (static) engine would leave the devices'
# files out, nothing else referring to them (see builtin/BuiltinRegistry.h).
set(_anchor_declarations "")
set(_anchor_calls "")
foreach (_source IN LISTS BUILTIN_DEVICE_SOURCES)
    get_filename_component(_stem "${_source}" NAME_WE)
    string(MAKE_C_IDENTIFIER "subBuiltinAnchor_${_stem}" _anchor)
    set_property(SOURCE "${_source}" APPEND PROPERTY COMPILE_DEFINITIONS "SUB_BUILTIN_ANCHOR=${_anchor}")
    string(APPEND _anchor_declarations "void ${_anchor}();\n")
    string(APPEND _anchor_calls "    ${_anchor}();\n")
endforeach()
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/BuiltinDevices.cpp CONTENT
"// Written by engine/CMakeLists.txt from builtin/devices/*.cpp (see builtin/BuiltinRegistry.h).

namespace sub {

@_anchor_declarations@
void linkBuiltinDevices() {
@_anchor_calls@}

}  // namespace sub
" @ONLY)

add_library(sub_engine STATIC
    src/Engine.cpp
    src/EngineChains.cpp
    src/EngineDevice.cpp
    src/EngineInput.cpp
    src/EngineOffline.cpp
    src/EngineSnapshot.cpp
    src/EngineTracks.cpp
    src/AudioDevice.cpp
    src/backends/WasapiBackend.cpp
    src/AudioSource.cpp
    src/Renderer.cpp
    src/Scheduler.cpp
    src/Recorder.cpp
    src/Metronome.cpp
    src/MidiInput.cpp
    src/Warp.cpp
    src/builtin/BuiltinProcessor.cpp
    src/builtin/BuiltinRegistry.cpp
    src/builtin/SampleSlicing.cpp
    ${BUILTIN_DEVICE_SOURCES}
    ${CMAKE_CURRENT_BINARY_DIR}/generated/BuiltinDevices.cpp
    src/plugins/Vst3Format.cpp
    src/plugins/Vst3Processor.cpp
)
if (WIN32)
    target_sources(sub_engine PRIVATE src/backends/MidiWinMM.cpp src/plugins/EditorWindow.cpp)
else()
    target_sources(sub_engine PRIVATE src/backends/MidiNone.cpp src/plugins/EditorWindowNone.cpp)
endif()
add_library(sub::engine ALIAS sub_engine)
target_include_directories(sub_engine PUBLIC src)
# Signalsmith Stretch (time stretching / pitch shifting) and its FFT library:
# MIT-licensed, header-only, no further dependencies.
target_include_directories(sub_engine SYSTEM PRIVATE third_party/signalsmith-stretch)
target_link_libraries(sub_engine PRIVATE signalsmith_linear)
target_link_libraries(sub_engine PUBLIC miniaudio vst3_hosting)
if (WIN32)
    target_link_libraries(sub_engine PUBLIC user32 ole32 uuid advapi32 winmm avrt)
    target_compile_definitions(sub_engine PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_link_libraries(sub_engine PUBLIC Threads::Threads)
endif()
target_compile_definitions(sub_engine PUBLIC _USE_MATH_DEFINES)
set_target_properties(sub_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (ASIO_SDK)
    target_sources(sub_engine PRIVATE src/backends/AsioBackend.cpp)
    target_include_directories(sub_engine SYSTEM PRIVATE "${ASIO_SDK}/common")
    target_compile_definitions(sub_engine PUBLIC SUBSTATION_HAS_ASIO=1)
endif()
sub_project_warnings(sub_engine)
# The audio path is float loops: let the compiler vectorize them with AVX2.
# Needs a CPU with AVX2 (Intel Haswell / AMD Zen and later). No fast math
# (/fp:fast, -ffast-math): a DAW's results must not depend on the compiler
# reordering arithmetic or assuming there's no NaN or infinity (a broken
# plug-in's output, a division by a parameter at 0). Denormals are flushed
# where audio is rendered instead (ScopedNoDenormals, rt/RtUtils.h).
# -fno-math-errno only lets GCC and Clang inline sqrt and friends: nothing
# reads errno.
if (MSVC)
    target_compile_options(sub_engine PRIVATE /arch:AVX2)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    target_compile_options(sub_engine PRIVATE -mavx2 -mfma -fno-math-errno)
    if (MINGW)
        # GCC on 64-bit Windows can't align the stack to 32 bytes (GCC bug 54412),
        # so an aligned AVX move to a stack slot may crash: the assembler makes
        # them all unaligned ones (as fast on any AVX2 CPU). Needs binutils 2.38.
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-Wa,-muse-unaligned-vector-move" SUB_HAVE_UNALIGNED_VECTOR_MOVE)
        if (SUB_HAVE_UNALIGNED_VECTOR_MOVE)
            target_compile_options(sub_engine PRIVATE -Wa,-muse-unaligned-vector-move)
        endif()
    endif()
else()
    target_compile_options(sub_engine PRIVATE -fno-math-errno)
endif()
