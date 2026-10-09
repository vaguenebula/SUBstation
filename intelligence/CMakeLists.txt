# The intelligence module: what SUBstation works out about music and sound,
# on threads of its own, independent of the engine, the browser and Qt (the
# application layer, app/src/intelligence, wraps it in Qt objects). Sound
# similarity, harmony (a song's chords and key from its MIDI, and parts made
# from them), humanizing (velocities by machine learning) and track labels;
# MIDI generation by machine learning and an MCP server for agents are to come
# (see docs/intelligence.md).
#
#   core/        what its parts share: decoding audio (miniaudio), the platform
#                (Platform.cpp on Windows, PlatformPosix.cpp elsewhere)
#   similarity/  sound similarity: fingerprints, comparing them, and the index
#                of a sample library kept up to date in the background
#   harmony/     chords and keys: inferring them from notes, block chords and
#                bass lines from them
#   humanize/    a part's velocities from HUMANBRO's model (models/velocity.hbm)
#   labels/      what each track is, in a few words ("Washed Out Serum Pluck"),
#                from its names, presets, notes, effects and mixer
add_library(sub_intelligence STATIC
    src/core/AudioReader.cpp
    src/harmony/Accompaniment.cpp
    src/harmony/ChordInference.cpp
    src/harmony/Chords.cpp
    src/humanize/VelocityModel.cpp
    src/labels/Effects.cpp
    src/labels/NoteProfile.cpp
    src/labels/TrackLabels.cpp
    src/labels/Words.cpp
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
# engine itself. HUMANBRO's runtime, below.
target_link_libraries(sub_intelligence PUBLIC Threads::Threads PRIVATE miniaudio signalsmith_linear humanbro)
set_target_properties(sub_intelligence PROPERTIES POSITION_INDEPENDENT_CODE ON)
sub_project_warnings(sub_intelligence)

# HUMANBRO's C++ runtime (third_party/humanbro, see its VERSION.txt): MIDI
# features and the tree ensemble that predicts velocities from them. Its
# features match its Python pipeline bit for bit, so it keeps its own strict
# floating-point flags: no contracting into fused multiply-adds, no fast-math.
add_library(humanbro STATIC
    third_party/humanbro/src/humanizer.cpp
    third_party/humanbro/src/midi_file.cpp
    third_party/humanbro/src/model.cpp
    third_party/humanbro/src/pipeline.cpp)
target_include_directories(humanbro SYSTEM PUBLIC third_party/humanbro/include)
target_include_directories(humanbro PRIVATE third_party/humanbro/src)
target_link_libraries(humanbro PUBLIC Threads::Threads)
set_target_properties(humanbro PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (MSVC)
    target_compile_options(humanbro PRIVATE /fp:precise /W4 /permissive- /utf-8)
else()
    target_compile_options(humanbro PRIVATE -ffp-contract=off -fno-fast-math -Wall -Wextra)
endif()

# The models (models/), next to the executables in bin/models, where the
# application looks for them (copied again when one changes). Multi-config
# generators (Visual Studio) put the executables in bin/<config>: a copy in each.
if (CMAKE_CONFIGURATION_TYPES)
    foreach (_config IN LISTS CMAKE_CONFIGURATION_TYPES)
        configure_file(models/velocity.hbm ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${_config}/models/velocity.hbm COPYONLY)
    endforeach()
else()
    configure_file(models/velocity.hbm ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/models/velocity.hbm COPYONLY)
endif()
