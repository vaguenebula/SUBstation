# The benchmarks (README.md here): programs to run by hand, not tests (ctest
# doesn't run them). Built with -DSUBSTATION_BUILD_BENCHMARKS=ON.
#
#   parallel_render_bench   tracks rendered on one thread and on several: the
#                           engine alone, no Qt (with the engine tests' helpers)
#   background_freeze_bench background freezing's savings and costs on a song of
#                           real VST3 plug-ins (Linux, /usr/lib/vst3): the engine
#                           alone, through the "Manual" driver
#   browser_backend_bench   the browser's backend on a large synthetic library,
#                           checked against the reference the tests use (it
#                           needs the application layer: SUBSTATION_BUILD_APP)

add_executable(parallel_render_bench parallel_render_bench.cpp Json.h)
target_include_directories(parallel_render_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                           ${PROJECT_SOURCE_DIR}/tests/engine/harness)
target_link_libraries(parallel_render_bench PRIVATE sub_engine)
sub_project_warnings(parallel_render_bench)
# Live runs play through the fake ASIO driver the tests build (Windows, with the ASIO SDK).
if (TARGET sub_test_asio)
    add_dependencies(parallel_render_bench sub_test_asio)
    target_compile_definitions(parallel_render_bench PRIVATE SUBSTATION_TEST_ASIO_DRIVER="$<TARGET_FILE:sub_test_asio>")
endif()

add_executable(background_freeze_bench background_freeze_bench.cpp)
target_include_directories(background_freeze_bench PRIVATE ${PROJECT_SOURCE_DIR}/tests/engine/harness)
target_link_libraries(background_freeze_bench PRIVATE sub_engine)
sub_project_warnings(background_freeze_bench)

if (SUBSTATION_BUILD_APP)
    add_executable(browser_backend_bench
        browser_backend_bench.cpp
        Json.h
        LibraryGen.cpp
        LibraryGen.h
        PyRandom.h
        ${PROJECT_SOURCE_DIR}/tests/app/support/BrowserReference.cpp
        ${PROJECT_SOURCE_DIR}/tests/app/support/BrowserReference.h)
    target_include_directories(browser_backend_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                               ${PROJECT_SOURCE_DIR}/tests/app/support ${PROJECT_SOURCE_DIR}/tests/engine/harness)
    target_link_libraries(browser_backend_bench PRIVATE sub_app)
    sub_project_warnings(browser_backend_bench)
else()
    message(STATUS "browser_backend_bench needs the application layer (SUBSTATION_BUILD_APP): not built")
endif()
