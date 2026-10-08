# The intelligence module: what SUBstation works out about music and sound,
# on threads of its own, independent of the engine, the browser and Qt (the
# application layer, app/src/intelligence, wraps it in Qt objects). Sound
# similarity and harmony (a song's chords and key from its MIDI, and parts made
# from them); MIDI generation and humanisation by machine learning and an MCP
# server for agents are to come (see docs/intelligence.md).
#
#   core/        what its parts share: decoding audio (miniaudio), the platform
#                (Platform.cpp on Windows, PlatformPosix.cpp elsewhere)
#   similarity/  sound similarity: fingerprints, comparing them, and the index
#                of a sample library kept up to date in the background
#   harmony/     chords and keys: inferring them from notes, block chords and
#                bass lines from them
add_library(sub_intelligence STATIC
    src/core/AudioReader.cpp
    src/harmony/Accompaniment.cpp
    src/harmony/ChordInference.cpp
    src/harmony/Chords.cpp
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
# Decoding and FFTs: third-party targets the engine's folder holds
# (engine/CMakeLists.txt), miniaudio and Signalsmith Linear; nothing of the
# engine itself.
target_link_libraries(sub_intelligence PUBLIC Threads::Threads PRIVATE miniaudio signalsmith_linear)
set_target_properties(sub_intelligence PROPERTIES POSITION_INDEPENDENT_CODE ON)
sub_project_warnings(sub_intelligence)
