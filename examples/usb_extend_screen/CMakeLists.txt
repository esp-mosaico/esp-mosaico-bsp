cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../.."
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_compile_options(-Wno-ignored-qualifiers)
project(usb_extend_screen)
