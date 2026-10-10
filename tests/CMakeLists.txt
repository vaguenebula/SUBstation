# The layers' boundaries: the platform layer, the engine, the browser and the intelligence module include
# nothing of Qt; the application layer nothing of Qt Quick or QML (cmake/CheckBoundaries.cmake).
add_test(NAME boundaries
         COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/cmake/CheckBoundaries.cmake)

# --- Test plug-ins ------------------------------------------------------------------
# One VST3 bundle with a test instrument and effects (vst3_plugins/), next to the
# test executables, laid out as a VST3 bundle is on this platform.
if (SUBSTATION_TEST_PLUGINS)
    include(TestPlugins.cmake)
endif()

# --- Engine tests: Qt-free -------------------------------------------------------------
file(GLOB ENGINE_TEST_SOURCES CONFIGURE_DEPENDS engine/*.cpp)
add_executable(engine_tests engine/harness/Main.cpp ${ENGINE_TEST_SOURCES})
target_include_directories(engine_tests PRIVATE engine)
target_link_libraries(engine_tests PRIVATE sub_engine)
sub_project_warnings(engine_tests)
if (TARGET sub_test_plugins)
    add_dependencies(engine_tests sub_test_plugins)
    target_compile_definitions(engine_tests PRIVATE SUBSTATION_TEST_PLUGINS_BUNDLE="${SUBSTATION_TEST_PLUGINS_BUNDLE}")
endif()
if (TARGET sub_test_asio)
    add_dependencies(engine_tests sub_test_asio)
    target_compile_definitions(engine_tests PRIVATE SUBSTATION_TEST_ASIO_DRIVER="$<TARGET_FILE:sub_test_asio>")
endif()
add_test(NAME engine COMMAND engine_tests)
if (WIN32)
    set_tests_properties(engine PROPERTIES RESOURCE_LOCK desktop)  # (it opens plug-in editors: see app/)
endif()

# --- Platform tests: Qt-free, with the engine tests' harness ------------------------------
file(GLOB PLATFORM_TEST_SOURCES CONFIGURE_DEPENDS platform/*.cpp)
add_executable(platform_tests engine/harness/Main.cpp ${PLATFORM_TEST_SOURCES})
target_include_directories(platform_tests PRIVATE engine)  # (harness/Test.h)
target_link_libraries(platform_tests PRIVATE sub_platform)
sub_project_warnings(platform_tests)
add_test(NAME platform COMMAND platform_tests)

# --- Intelligence tests: Qt-free, with the engine tests' harness (Test.h, Main.cpp) --------
file(GLOB INTELLIGENCE_TEST_SOURCES CONFIGURE_DEPENDS intelligence/*.cpp)
add_executable(intelligence_tests engine/harness/Main.cpp ${INTELLIGENCE_TEST_SOURCES} intelligence/Sounds.h)
target_include_directories(intelligence_tests PRIVATE engine intelligence)  # (harness/Test.h: never the harness folder itself, whose Signal.h is <signal.h> where names ignore case)
target_link_libraries(intelligence_tests PRIVATE sub_intelligence)
target_compile_definitions(intelligence_tests PRIVATE SUBSTATION_VELOCITY_MODEL="${PROJECT_SOURCE_DIR}/intelligence/models/velocity.hbm")
sub_project_warnings(intelligence_tests)
add_test(NAME intelligence COMMAND intelligence_tests)

# --- Application-layer and UI tests: Qt Test, one executable per file ---------------------
if (SUBSTATION_BUILD_APP)
    add_subdirectory(app)
endif()
