# Qt Test: each test_*.cpp is an executable of its own (QTEST_MAIN or
# QTEST_GUILESS_MAIN), run offscreen. App-layer tests link sub_app; tests whose
# file name starts with test_ui_ link the UI (sub_ui) and Qt Quick Test too, and
# run on a real (virtual) display where there is one: Qt's offscreen platform
# renders Qt Quick in software, which draws none of the UI's scene-graph
# geometry. On Linux they run under xvfb-run with the xcb platform (Mesa renders
# OpenGL in software).
find_package(Qt6 REQUIRED COMPONENTS Test QuickTest)
find_program(XVFB_RUN xvfb-run)
file(GLOB APP_TEST_SOURCES CONFIGURE_DEPENDS test_*.cpp)
file(GLOB APP_TEST_SUPPORT CONFIGURE_DEPENDS support/*.cpp support/*.h)
if (APP_TEST_SUPPORT)
    add_library(sub_app_test_support STATIC ${APP_TEST_SUPPORT})
    target_include_directories(sub_app_test_support PUBLIC support)
    target_link_libraries(sub_app_test_support PUBLIC sub_app Qt6::Test)
endif()
foreach (_source IN LISTS APP_TEST_SOURCES)
    get_filename_component(_name ${_source} NAME_WE)
    qt_add_executable(${_name} ${_source})
    target_link_libraries(${_name} PRIVATE sub_app Qt6::Test)
    if (TARGET sub_app_test_support)
        target_link_libraries(${_name} PRIVATE sub_app_test_support)
    endif()
    if (_name MATCHES "^test_ui_")
        target_link_libraries(${_name} PRIVATE sub_ui sub_uiplugin Qt6::QuickTest)
    endif()
    if (TARGET sub_test_plugins)
        add_dependencies(${_name} sub_test_plugins)
        target_compile_definitions(${_name} PRIVATE SUBSTATION_TEST_PLUGINS_BUNDLE="${SUBSTATION_TEST_PLUGINS_BUNDLE}")
    endif()
    target_compile_definitions(${_name} PRIVATE SUBSTATION_SCANNER="$<TARGET_FILE:substation-scan>")
    add_dependencies(${_name} substation-scan)
    set_target_properties(${_name} PROPERTIES WIN32_EXECUTABLE OFF)
    # QT_FORCE_STDERR_LOGGING: on Windows, Qt Test and qWarning write to the
    # debugger, not the output ctest shows, when the process has no console window.
    if (_name MATCHES "^test_ui_" AND XVFB_RUN AND NOT WIN32)
        add_test(NAME ${_name} COMMAND ${XVFB_RUN} -a -s "-screen 0 1920x1080x24" $<TARGET_FILE:${_name}>)
        set_tests_properties(${_name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=xcb;QT_FORCE_STDERR_LOGGING=1")
    elseif (_name MATCHES "^test_ui_")
        add_test(NAME ${_name} COMMAND ${_name})
        set_tests_properties(${_name} PROPERTIES ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
    else()
        add_test(NAME ${_name} COMMAND ${_name})
        set_tests_properties(${_name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1")
    endif()
    # On Windows the tests showing windows share one desktop, where one window at
    # a time is active (has the keyboard): they take turns. (The UI's tests, and
    # those opening plug-in editors. On Linux each UI test has an X server of its own.)
    if (WIN32 AND _name MATCHES "^test_ui_|^test_bridge_plugins$|^test_session_renders$")
        set_tests_properties(${_name} PROPERTIES RESOURCE_LOCK desktop)
    endif()
endforeach()
