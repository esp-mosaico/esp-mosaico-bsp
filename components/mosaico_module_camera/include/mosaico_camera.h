/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mosaico_module_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOSAICO_CAMERA_DEFAULT_CONFIG() {                        \
    .slot = MOSAICO_MODULE_MGR_SLOT_AUTO,                          \
    .width = 1024,                                               \
    .height = 768,                                               \
    .pixel_format = MOSAICO_CAMERA_PIXEL_FORMAT_UYVY,            \
    .buffer_count = 4,                                           \
    .frame_timeout_ms = 1000,                                    \
    .discovery_timeout_ms = 1500,                                \
    .apply_module_tuning = true,                                 \
    .allow_unidentified = false,                                 \
}

typedef struct mosaico_camera_t *mosaico_camera_handle_t;
typedef struct mosaico_camera_jpeg_decoder_t *mosaico_camera_jpeg_decoder_handle_t;

typedef enum {
    MOSAICO_CAMERA_PIXEL_FORMAT_UYVY = 0,
    MOSAICO_CAMERA_PIXEL_FORMAT_RGB565,
    MOSAICO_CAMERA_PIXEL_FORMAT_JPEG,
} mosaico_camera_pixel_format_t;

typedef struct {
    mosaico_module_mgr_slot_t slot;
    uint32_t width;
    uint32_t height;
    mosaico_camera_pixel_format_t pixel_format;
    uint8_t buffer_count;
    uint32_t frame_timeout_ms;
    uint32_t discovery_timeout_ms;
    bool apply_module_tuning;
    /* Open the left slot even when its EEPROM identified no camera board. The
     * pins are driven as DVP regardless of what is plugged in, so this is for
     * bring-up on boards with an unprogrammed descriptor only. */
    bool allow_unidentified;
} mosaico_camera_config_t;

typedef struct {
    const void *data;
    size_t size;
    uint32_t index;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_line;
    uint32_t pixel_format;
} mosaico_camera_frame_t;

typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t width;
    uint32_t height;
} mosaico_camera_rgb888_frame_t;

typedef struct {
    mosaico_module_mgr_slot_t slot;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_line;
    uint32_t pixel_format;
    uint32_t frame_rate;
    size_t frame_buffer_size;
    uint8_t buffer_count;
} mosaico_camera_info_t;

typedef struct {
    uint8_t buffer_count;
    uint8_t outstanding_count;
    bool streaming;
    bool power_down;
} mosaico_camera_pipeline_stats_t;

/**
 * @brief Discover, claim and start the Mosaico OV3640 camera subboard
 *
 * Pass NULL for automatic defaults. The default path discovers the camera,
 * selects its supported slot, claims shared resources and starts streaming.
 */
esp_err_t mosaico_camera_new(const mosaico_camera_config_t *config,
                             mosaico_camera_handle_t *out_camera);

esp_err_t mosaico_camera_get_info(mosaico_camera_handle_t camera,
                                  mosaico_camera_info_t *out_info);

esp_err_t mosaico_camera_get_pipeline_stats(
    mosaico_camera_handle_t camera,
    mosaico_camera_pipeline_stats_t *out_stats);

esp_err_t mosaico_camera_get_frame(mosaico_camera_handle_t camera,
                                   mosaico_camera_frame_t *out_frame);

esp_err_t mosaico_camera_return_frame(mosaico_camera_handle_t camera,
                                      const mosaico_camera_frame_t *frame);

esp_err_t mosaico_camera_discard_frames(mosaico_camera_handle_t camera,
                                        uint32_t count);

esp_err_t mosaico_camera_jpeg_decoder_new(
    uint32_t max_width, uint32_t max_height,
    mosaico_camera_jpeg_decoder_handle_t *out_decoder);

esp_err_t mosaico_camera_jpeg_decode_rgb888(
    mosaico_camera_jpeg_decoder_handle_t decoder,
    const mosaico_camera_frame_t *jpeg_frame,
    mosaico_camera_rgb888_frame_t *out_frame);

/**
 * @brief Decode JPEG to RGB888 and rotate the result counter-clockwise by 90 degrees
 *
 * The returned frame remains owned by the decoder and is valid until the next
 * decode call using the same decoder.
 */
esp_err_t mosaico_camera_jpeg_decode_rgb888_ccw90(
    mosaico_camera_jpeg_decoder_handle_t decoder,
    const mosaico_camera_frame_t *jpeg_frame,
    mosaico_camera_rgb888_frame_t *out_frame);

esp_err_t mosaico_camera_jpeg_decoder_del(
    mosaico_camera_jpeg_decoder_handle_t decoder);

esp_err_t mosaico_camera_get_exposure(mosaico_camera_handle_t camera,
                                      int32_t *out_value);

esp_err_t mosaico_camera_set_exposure(mosaico_camera_handle_t camera,
                                      int32_t value);

typedef struct {
    int32_t exposure;
    uint8_t aec_ctrl_3012;
    uint8_t aec_agc_ctrl;
    uint8_t exposure_reg_l;
    uint8_t gain_h;
    uint8_t gain_l;
    bool exposure_via_v4l2;
    bool exposure_adjusted;
    bool gain_adjusted;
    bool prepared;
} mosaico_camera_flash_state_t;

/**
 * @brief Pause AEC/AGC and raise exposure for a flash-lit still capture
 */
esp_err_t mosaico_camera_prepare_flash_capture(
    mosaico_camera_handle_t camera, mosaico_camera_flash_state_t *state);

/**
 * @brief Restore preview exposure/AEC settings after a flash capture
 */
esp_err_t mosaico_camera_restore_flash_capture(
    mosaico_camera_handle_t camera, const mosaico_camera_flash_state_t *state);

/**
 * @brief Turn the flash LED on via host GPIO (active-low hardware)
 */
esp_err_t mosaico_camera_flash_trigger(mosaico_camera_handle_t camera);

/**
 * @brief Turn the flash LED off via host GPIO
 */
esp_err_t mosaico_camera_flash_stop(mosaico_camera_handle_t camera);

/** @deprecated Use mosaico_camera_flash_trigger() */
esp_err_t mosaico_camera_strobe_trigger(mosaico_camera_handle_t camera);

/** @deprecated Use mosaico_camera_flash_stop() */
esp_err_t mosaico_camera_strobe_stop(mosaico_camera_handle_t camera);

esp_err_t mosaico_camera_set_power_down(mosaico_camera_handle_t camera,
                                        bool sleep);

bool mosaico_camera_is_power_down(mosaico_camera_handle_t camera);

esp_err_t mosaico_camera_restart(mosaico_camera_handle_t camera);

/**
 * @brief Stop streaming and release every camera and subboard resource
 */
esp_err_t mosaico_camera_del(mosaico_camera_handle_t camera);

#ifdef __cplusplus
}
#endif
