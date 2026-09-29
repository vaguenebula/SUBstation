cmake_minimum_required(VERSION 3.26)
project(gilstudio_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT WIN32)
    message(WARNING "GIL Studio targets Windows (WASAPI); other platforms are untested.")
endif()

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

nanobind_add_module(_engine NB_STATIC
    engine/src/bindings.cpp
    engine/src/Engine.cpp
    engine/src/AudioDevice.cpp
    engine/src/AudioSource.cpp
    engine/src/Renderer.cpp
    engine/src/Metronome.cpp
    engine/src/Warp.cpp
)
target_include_directories(_engine PRIVATE engine/src)
# Signalsmith Stretch (time stretching / pitch shifting) and its FFT library:
# MIT-licensed, header-only, no further dependencies.
target_include_directories(_engine SYSTEM PRIVATE
    engine/third_party/signalsmith-stretch
    engine/third_party/signalsmith-linear)
target_link_libraries(_engine PRIVATE miniaudio)
target_compile_definitions(_engine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
if (MSVC)
    target_compile_options(_engine PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
endif()

install(TARGETS _engine LIBRARY DESTINATION gilstudio)
