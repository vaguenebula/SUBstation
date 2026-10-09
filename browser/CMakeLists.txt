# The browser's file index and search: its own threads, independent of the
# engine and of Qt (the application layer, app/src/browser, wraps it in models).
# What it needs of the operating system is sub_platform's (platform/) and, for
# listing and watching folders, Platform.h's: Platform.cpp on Windows,
# PlatformPosix.cpp elsewhere, with a folder watcher per system
# (FolderWatcherInotify.cpp on Linux, FolderWatcherNone.cpp where there's none yet).
add_library(sub_browser STATIC
    src/Browser.cpp
    src/Indexer.cpp
    src/Model.cpp
    src/Search.cpp
    src/Text.cpp)
if (WIN32)
    target_sources(sub_browser PRIVATE src/Platform.cpp)
    target_compile_definitions(sub_browser PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(sub_browser PRIVATE src/PlatformPosix.cpp src/FolderWatcherInotify.cpp)
else()
    target_sources(sub_browser PRIVATE src/PlatformPosix.cpp src/FolderWatcherNone.cpp)
endif()
target_link_libraries(sub_browser PUBLIC sub_platform Threads::Threads)
add_library(sub::browser ALIAS sub_browser)
target_include_directories(sub_browser PUBLIC src)
set_target_properties(sub_browser PROPERTIES POSITION_INDEPENDENT_CODE ON)
sub_project_warnings(sub_browser)
