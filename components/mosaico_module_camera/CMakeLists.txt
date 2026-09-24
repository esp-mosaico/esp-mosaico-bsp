set(MOSAICO_CAMERA_IDF_PATCH "${CMAKE_CURRENT_LIST_DIR}/patches/esp-idf-dvp-frame-capture-stability.patch")
set(MOSAICO_CAMERA_SENSOR_PATCH "${CMAKE_CURRENT_LIST_DIR}/patches/esp-cam-sensor-sc101iot-pclk-delay.patch")
find_program(MOSAICO_CAMERA_GIT git)

function(mosaico_camera_apply_patch patch_name source_dir patch_file)
    if(NOT MOSAICO_CAMERA_GIT)
        message(WARNING "mosaico_module_camera: Git not found; skipping ${patch_name} patch")
        return()
    endif()
    if(NOT IS_DIRECTORY "${source_dir}")
        message(WARNING "mosaico_module_camera: ${source_dir} not found; skipping ${patch_name} patch")
        return()
    endif()
    execute_process(
        COMMAND "${MOSAICO_CAMERA_GIT}" -C "${source_dir}" apply --no-index --reverse --check "${patch_file}"
        RESULT_VARIABLE MOSAICO_CAMERA_PATCH_APPLIED
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(MOSAICO_CAMERA_PATCH_APPLIED STREQUAL "0")
        message(STATUS "mosaico_module_camera: ${patch_name} patch already applied")
    else()
        execute_process(
            COMMAND "${MOSAICO_CAMERA_GIT}" -C "${source_dir}" apply --no-index "${patch_file}"
            RESULT_VARIABLE MOSAICO_CAMERA_PATCH_RESULT
            ERROR_VARIABLE MOSAICO_CAMERA_PATCH_ERROR
            ERROR_STRIP_TRAILING_WHITESPACE
        )
        if(MOSAICO_CAMERA_PATCH_RESULT STREQUAL "0")
            message(STATUS "mosaico_module_camera: applied ${patch_name} patch")
        else()
            message(WARNING "mosaico_module_camera: failed to apply ${patch_name} patch; continuing build: ${MOSAICO_CAMERA_PATCH_ERROR}")
        endif()
    endif()
endfunction()

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

if(NOT DEFINED ENV{IDF_PATH} OR "$ENV{IDF_PATH}" STREQUAL "")
    message(WARNING "mosaico_module_camera: IDF_PATH is unavailable; skipping ESP-IDF DVP patch")
else()
    mosaico_camera_apply_patch("ESP-IDF DVP" "$ENV{IDF_PATH}" "${MOSAICO_CAMERA_IDF_PATCH}")
endif()

mosaico_camera_apply_patch("esp_cam_sensor SC101IOT PCLK delay" "${PROJECT_DIR}" "${MOSAICO_CAMERA_SENSOR_PATCH}")
