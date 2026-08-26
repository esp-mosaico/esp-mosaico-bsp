cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${CMAKE_CURRENT_LIST_DIR}/../../common_components/mosaico_module_mgr"
    "${CMAKE_CURRENT_LIST_DIR}/../../expansion_modules/mosaico_module_camera"
    "${CMAKE_CURRENT_LIST_DIR}/../../expansion_modules/mosaico_module_button_led"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(camera_photo_app)
