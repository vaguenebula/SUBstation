# What every layer needs from the operating system, in one place: text in the
# systems' encodings (UTF-8 everywhere, UTF-16 for Windows' wide calls), paths
# and how they compare, files, threads' priorities. No Qt and nothing of the
# other layers: the engine, the browser backend and the intelligence module
# stand on it (cmake/CheckBoundaries.cmake). Each system's code is a file of
# its own: PlatformWin32.cpp on Windows, PlatformPosix.cpp elsewhere. Headers
# are included by their folder: "platform/Paths.h".
add_library(sub_platform STATIC src/platform/Files.cpp src/platform/Unicode.cpp)
if (WIN32)
    target_sources(sub_platform PRIVATE src/platform/PlatformWin32.cpp)
    target_compile_definitions(sub_platform PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(sub_platform PRIVATE avrt)
else()
    target_sources(sub_platform PRIVATE src/platform/PlatformPosix.cpp)
endif()
add_library(sub::platform ALIAS sub_platform)
target_include_directories(sub_platform PUBLIC src)
set_target_properties(sub_platform PROPERTIES POSITION_INDEPENDENT_CODE ON)
sub_project_warnings(sub_platform)
