# The benchmarks (README.md here): programs to run by hand, not tests (ctest
# doesn't run them). Built with -DSUBSTATION_BUILD_BENCHMARKS=ON.
#
#   parallel_render_bench   tracks rendered on one thread and on several: the
#                           engine alone, no Qt (with the engine tests' helpers)

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

