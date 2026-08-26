cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${CMAKE_CURRENT_LIST_DIR}/../../common_components/mosaico_module_mgr"
    "${CMAKE_CURRENT_LIST_DIR}/../../expansion_modules/mosaico_module_camera"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ai_model_gallery)

# Newer GCC promotes this esp-dl warning to an error.
idf_build_get_property(build_components BUILD_COMPONENTS)
if("espressif__esp-dl" IN_LIST build_components)
    idf_component_get_property(esp_dl_lib espressif__esp-dl COMPONENT_LIB)
    target_compile_options(${esp_dl_lib} PRIVATE -Wno-error=non-c-typedef-for-linkage)
endif()
