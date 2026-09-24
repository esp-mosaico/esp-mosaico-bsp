set(MOSAICO_CAMERA_IDF_PATCH "${CMAKE_CURRENT_LIST_DIR}/patches/esp-idf-dvp-frame-capture-stability.patch")
find_program(MOSAICO_CAMERA_GIT git)

if(NOT MOSAICO_CAMERA_GIT)
    message(WARNING "mosaico_module_camera: Git not found; skipping ESP-IDF DVP patch")
elseif(NOT DEFINED ENV{IDF_PATH} OR "$ENV{IDF_PATH}" STREQUAL "")
    message(WARNING "mosaico_module_camera: IDF_PATH is unavailable; skipping ESP-IDF DVP patch")
else()
    set(MOSAICO_CAMERA_IDF_PATH "$ENV{IDF_PATH}")
    execute_process(
        COMMAND "${MOSAICO_CAMERA_GIT}" -C "${MOSAICO_CAMERA_IDF_PATH}" apply --reverse --check "${MOSAICO_CAMERA_IDF_PATCH}"
        RESULT_VARIABLE MOSAICO_CAMERA_PATCH_APPLIED
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(MOSAICO_CAMERA_PATCH_APPLIED STREQUAL "0")
        message(STATUS "mosaico_module_camera: ESP-IDF DVP patch already applied")
    else()
        execute_process(
            COMMAND "${MOSAICO_CAMERA_GIT}" -C "${MOSAICO_CAMERA_IDF_PATH}" apply "${MOSAICO_CAMERA_IDF_PATCH}"
            RESULT_VARIABLE MOSAICO_CAMERA_PATCH_RESULT
            ERROR_VARIABLE MOSAICO_CAMERA_PATCH_ERROR
            ERROR_STRIP_TRAILING_WHITESPACE
        )
        if(MOSAICO_CAMERA_PATCH_RESULT STREQUAL "0")
            message(STATUS "mosaico_module_camera: applied ESP-IDF DVP patch")
        else()
            message(WARNING "mosaico_module_camera: failed to apply ESP-IDF DVP patch; continuing build: ${MOSAICO_CAMERA_PATCH_ERROR}")
        endif()
    endif()
endif()

idf_component_register(
    SRCS
        "mosaico_camera.c"
        "mosaico_camera_jpeg.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_cam
        esp_driver_jpeg
        esp_driver_ppa
    PRIV_REQUIRES
        esp_hw_support
)
