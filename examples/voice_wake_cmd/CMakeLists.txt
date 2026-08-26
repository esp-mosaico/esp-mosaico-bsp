cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(voice_wake_cmd)

idf_build_get_property(build_components BUILD_COMPONENTS)
if("espressif__esp-dl" IN_LIST build_components)
    idf_component_get_property(esp_dl_lib espressif__esp-dl COMPONENT_LIB)
    target_compile_options(${esp_dl_lib} PRIVATE -Wno-error=non-c-typedef-for-linkage)
endif()
