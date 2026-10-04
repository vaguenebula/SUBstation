# The browser's file index and search: its own threads, independent of the
# engine and of Qt (the application layer, app/src/browser, wraps it in models).
if (NOT WIN32 AND NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/PlatformPosix.cpp)
    message(STATUS "The browser backend has no POSIX platform layer yet: not built")
    return()
endif()
add_library(sub_browser STATIC
    src/Browser.cpp
    src/Indexer.cpp
    src/Model.cpp
    src/Search.cpp
    src/Text.cpp)
if (WIN32)
    target_sources(sub_browser PRIVATE src/Platform.cpp)
    target_compile_definitions(sub_browser PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
    target_sources(sub_browser PRIVATE src/PlatformPosix.cpp)
    target_link_libraries(sub_browser PUBLIC Threads::Threads)
endif()
add_library(sub::browser ALIAS sub_browser)
target_include_directories(sub_browser PUBLIC src)
set_target_properties(sub_browser PROPERTIES POSITION_INDEPENDENT_CODE ON)
sub_project_warnings(sub_browser)
