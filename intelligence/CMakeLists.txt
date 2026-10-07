# The intelligence module: what SUBstation works out about music and sound,
# on threads of its own, independent of the engine, the browser and Qt (the
# application layer, app/src/intelligence, wraps it in Qt objects). Sound
# similarity for now; MIDI generation and humanisation, chord recognition and
# an MCP server for agents are to come (see docs/intelligence.md).
#
#   core/        what its parts share: decoding audio (miniaudio), an FFT, the
#                platform (Platform.cpp on Windows, PlatformPosix.cpp elsewhere)
#   similarity/  sound similarity: fingerprints, comparing them, and the index
#                of a sample library kept up to date in the background
add_library(sub_intelligence STATIC
    src/core/AudioReader.cpp
    src/core/Fft.cpp
    src/similarity/Similarity.cpp
    src/similarity/SoundFeatures.cpp
    src/similarity/SoundIndex.cpp
    src/similarity/SoundStore.cpp)
if (WIN32)
    target_sources(sub_intelligence PRIVATE src/core/Platform.cpp)
    target_compile_definitions(sub_intelligence PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
    target_sources(sub_intelligence PRIVATE src/core/PlatformPosix.cpp)
endif()
add_library(sub::intelligence ALIAS sub_intelligence)
target_include_directories(sub_intelligence PUBLIC src)
# Decoding: the miniaudio library the engine builds (engine/CMakeLists.txt), a
# third-party target of its own; nothing of the engine itself.
target_link_libraries(sub_intelligence PUBLIC Threads::Threads PRIVATE miniaudio)
set_target_properties(sub_intelligence PROPERTIES POSITION_INDEPENDENT_CODE ON)
sub_project_warnings(sub_intelligence)
