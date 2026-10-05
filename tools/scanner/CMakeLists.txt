# substation-scan: reads VST3 files in a process of its own (see main.cpp). No Qt.
add_executable(substation-scan main.cpp)
target_link_libraries(substation-scan PRIVATE sub_engine)
sub_project_warnings(substation-scan)
if (WIN32)
    set_target_properties(substation-scan PROPERTIES WIN32_EXECUTABLE OFF)
endif()
