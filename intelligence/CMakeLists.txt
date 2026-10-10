# The intelligence module: what SUBstation works out about music and sound,
# on threads of its own, independent of the engine, the browser and Qt (the
# application layer, app/src/intelligence, wraps it in Qt objects). Sound
# similarity, harmony (a song's chords and key from its MIDI, and parts made
# from them) and humanizing (velocities by machine learning); MIDI generation
# by machine learning and an MCP server for agents are to come (see
# docs/intelligence.md).
#
#   core/        what its parts share: decoding audio (miniaudio), hashing
#                (what it needs of the operating system is sub_platform's: platform/)
#   similarity/  sound similarity: fingerprints (made with Essentia), comparing
#                them, and the index of a sample library kept up to date in the
#                background
#   harmony/     chords and keys: inferring them from notes, block chords and
#                bass lines from them
#   humanize/    a part's velocities from HUMANBRO's model (models/velocity.hbm)
add_library(sub_intelligence STATIC
    src/core/AudioReader.cpp
    src/harmony/Accompaniment.cpp
    src/harmony/ChordInference.cpp
    src/harmony/Chords.cpp
    src/humanize/VelocityModel.cpp
    src/similarity/EssentiaExtractor.cpp
    src/similarity/FeatureExtractor.cpp
    src/similarity/FeatureSchema.cpp
    src/similarity/Similarity.cpp
    src/similarity/SoundIndex.cpp
    src/similarity/SoundStore.cpp)
if (WIN32)
    # <windows.h> comes in through miniaudio's and Essentia's headers: without
    # NOMINMAX its min and max macros break std::min and std::max.
    target_compile_definitions(sub_intelligence PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()
add_library(sub::intelligence ALIAS sub_intelligence)
target_include_directories(sub_intelligence PUBLIC src)
# The operating system: sub_platform (platform/), which the other layers share.
# Decoding: miniaudio, a third-party target the engine's folder holds
# (engine/CMakeLists.txt); nothing of the engine itself. Essentia and
# HUMANBRO's runtime, below.
target_link_libraries(sub_intelligence PUBLIC sub_platform Threads::Threads PRIVATE miniaudio essentia humanbro)
set_target_properties(sub_intelligence PROPERTIES POSITION_INDEPENDENT_CODE ON)
sub_project_warnings(sub_intelligence)

# Essentia (third_party/essentia, 2.1-beta5, AGPLv3: see its VERSION.txt and
# docs/licensing.md): the algorithms the sound similarity's fingerprints are
# made with (similarity/EssentiaExtractor.cpp). Only its core library and the
# algorithms the extractor runs, with KISS FFT: no FFTW, FFmpeg, libsamplerate,
# YAML, TagLib, Gaia or TensorFlow (the module decodes and resamples itself and
# hands Essentia PCM). Built as upstream builds it: the C++ of its time, its
# warnings not shown. ESSENTIA_STATIC: a static library, exporting nothing.
# DEBUGGING_ENABLED=0 compiles its logger out, whose global state isn't
# thread-safe (several analysers run its algorithms at once).
set(_essentia ${CMAKE_CURRENT_SOURCE_DIR}/third_party/essentia/src)
file(GLOB _essentia_sources CONFIGURE_DEPENDS
     ${_essentia}/essentia/*.cpp ${_essentia}/essentia/scheduler/*.cpp ${_essentia}/essentia/streaming/*.cpp
     ${_essentia}/essentia/streaming/algorithms/*.cpp ${_essentia}/essentia/utils/*.cpp ${_essentia}/algorithms/*.cpp
     ${_essentia}/algorithms/*/*.cpp)
add_library(essentia STATIC ${_essentia_sources}
    ${_essentia}/3rdparty/kiss_fft130/kiss_fft.c
    ${_essentia}/3rdparty/kiss_fft130/tools/kiss_fftr.c)
# Its sources include each other by bare name, from these folders (as its
# wscript sets them); the module includes <essentia/...> from src only.
target_include_directories(essentia SYSTEM PUBLIC ${_essentia})
target_include_directories(essentia PRIVATE ${_essentia}/essentia ${_essentia}/essentia/scheduler
    ${_essentia}/essentia/streaming ${_essentia}/essentia/streaming/algorithms ${_essentia}/essentia/utils
    ${_essentia}/3rdparty ${_essentia}/3rdparty/kiss_fft130)
target_compile_definitions(essentia PUBLIC ESSENTIA_STATIC DEBUGGING_ENABLED=0)
if (WIN32)
    target_compile_definitions(essentia PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
endif()
set_target_properties(essentia PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if (MSVC)
    target_compile_options(essentia PRIVATE /w /utf-8 /EHsc)
else()
    target_compile_options(essentia PRIVATE -w)
endif()
target_link_libraries(essentia PUBLIC Threads::Threads)

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
