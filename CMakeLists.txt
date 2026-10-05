cmake_minimum_required(VERSION 3.26)
project(SUBstation VERSION 0.2.0 LANGUAGES C CXX)

# Three layers, each a library of its own (see docs/architecture.md):
#   engine/   the real-time audio engine (no Qt)            -> sub_engine
#   browser/  the browser's file index and search (no Qt)  -> sub_browser
#   app/      the application layer (Qt Core/Gui, no QML)   -> sub_app
#   ui/       the Qt Quick UI: QML and scene-graph items     -> sub_ui, the substation executable
# plus tools/scanner (the VST3 scanner's child process, no Qt) and tests/.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# 64-bit only (AVX2, the x86_64 VST3 bundles). A 32-bit compiler first on PATH
# (an old MinGW) otherwise ends in Qt's "version 6.x (64bit)" not being accepted.
if (CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The compiler CMake found (${CMAKE_CXX_COMPILER}) makes 32-bit programs; SUBstation is "
                        "64-bit only. Use a 64-bit one: Qt's MinGW 64-bit (C:/Qt/Tools/mingw*_64/bin first on PATH, "
                        "or give its gcc.exe and g++.exe as CMAKE_C_COMPILER and CMAKE_CXX_COMPILER) or MSVC's x64 "
                        "tools. Then delete the build folder: CMake keeps the compiler it found there.")
endif()
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if (NOT WIN32)
    message(STATUS "SUBstation targets Windows (WASAPI, ASIO, plug-in editors); elsewhere it builds and runs "
                   "with the system's default audio backend, without MIDI devices or plug-in editors.")
endif()

option(SUBSTATION_BUILD_APP "Build the application and its UI (needs Qt 6.4 or newer)" ON)
option(SUBSTATION_BUILD_TESTS "Build the tests" ON)
option(SUBSTATION_BUILD_BENCHMARKS "Build the benchmarks (benchmarks/: run by hand, not by ctest)" OFF)
option(SUBSTATION_TEST_PLUGINS "Build the VST3 plug-ins and the ASIO driver the tests use" ON)
option(SUBSTATION_ASIO "Build ASIO support (needs Steinberg's ASIO SDK: see docs/building.md)" ON)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
include(cmake/Warnings.cmake)

# Everything built lands in one folder, so the executables find the scanner and
# the tests find the test plug-ins next to them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_subdirectory(engine)
add_subdirectory(browser)
add_subdirectory(tools/scanner)

if (SUBSTATION_BUILD_APP)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
    qt_standard_project_setup()
    add_subdirectory(app)
    add_subdirectory(ui)
endif()

if (SUBSTATION_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# After the tests: the benchmarks use the fake ASIO driver they build.
if (SUBSTATION_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
