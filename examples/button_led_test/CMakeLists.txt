# For more information about the ESP-IDF build system, see:
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/../..
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(button_led_test)
