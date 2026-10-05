# The test VST3 plug-ins (vst3_plugins/) and, with the ASIO SDK, the fake ASIO
# driver (asio_driver/). Sets SUBSTATION_TEST_PLUGINS_BUNDLE to the bundle's path.
sub_vst3_sdk_library(vst3_plugin_sdk
    ${VST3_SDK}/public.sdk/source/common/pluginview.cpp
    ${VST3_SDK}/public.sdk/source/main/pluginfactory.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstaudioeffect.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstbus.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstcomponent.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstcomponentbase.cpp
    ${VST3_SDK}/public.sdk/source/vst/vsteditcontroller.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstparameters.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstsinglecomponenteffect.cpp)
target_link_libraries(vst3_plugin_sdk PUBLIC vst3_base)
# vsteditcontroller.cpp is compiled on its own, not inside vstsinglecomponenteffect.cpp.
target_compile_definitions(vst3_plugin_sdk PRIVATE PROJECT_INCLUDES_VSTEDITCONTROLLER=1)

set(_bundle ${CMAKE_BINARY_DIR}/testplugins/SUBTestPlugins.vst3)
if (WIN32)
    set(_main ${VST3_SDK}/public.sdk/source/main/dllmain.cpp)
    set(_arch_dir x86_64-win)
    set(_suffix .vst3)
else()
    set(_main ${VST3_SDK}/public.sdk/source/main/linuxmain.cpp)
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_arch_dir aarch64-linux)
    else()
        set(_arch_dir x86_64-linux)
    endif()
    set(_suffix .so)
endif()

add_library(sub_test_plugins MODULE
    vst3_plugins/test_plugins.cpp
    vst3_plugins/test_effect.cpp
    vst3_plugins/test_sidechain.cpp
    ${_main})
target_link_libraries(sub_test_plugins PRIVATE vst3_plugin_sdk)
target_compile_definitions(sub_test_plugins PRIVATE UNICODE _UNICODE NOMINMAX)
set_target_properties(sub_test_plugins PROPERTIES
    OUTPUT_NAME "SUBTestPlugins" PREFIX "" SUFFIX "${_suffix}"
    LIBRARY_OUTPUT_DIRECTORY ${_bundle}/Contents/${_arch_dir}
    CXX_VISIBILITY_PRESET hidden)
foreach (_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(sub_test_plugins PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${_config} ${_bundle}/Contents/${_arch_dir})
endforeach()
if (WIN32)
    target_link_libraries(sub_test_plugins PRIVATE user32 gdi32)
endif()
if (MSVC)
    target_compile_options(sub_test_plugins PRIVATE /W3 /utf-8 /Zc:__cplusplus)
endif()
set(SUBSTATION_TEST_PLUGINS_BUNDLE ${_bundle})

# A fake ASIO driver, loaded by the tests through SUBSTATION_ASIO_DRIVERS; it is never registered.
if (SUBSTATION_ASIO_SDK_DIR)
    add_library(sub_test_asio MODULE asio_driver/test_asio_driver.cpp asio_driver/test_asio_driver.def)
    target_include_directories(sub_test_asio SYSTEM PRIVATE "${SUBSTATION_ASIO_SDK_DIR}/common")
    target_link_libraries(sub_test_asio PRIVATE ole32 uuid)
    target_compile_definitions(sub_test_asio PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(sub_test_asio PROPERTIES OUTPUT_NAME "SUBTestAsio" PREFIX "")
    if (MSVC)
        target_compile_options(sub_test_asio PRIVATE /W3 /utf-8)
    endif()
endif()
