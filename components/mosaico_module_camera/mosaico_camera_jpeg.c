/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_camera.h"

#include <inttypes.h>
#include <stdlib.h>

#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_log.h"
#include "linux/videodev2.h"

static const char *TAG = "mosaico_cam_jpeg";

struct mosaico_camera_jpeg_decoder_t {
    jpeg_decoder_handle_t engine;
    uint8_t *output;
    size_t output_capacity;
    ppa_client_handle_t rotation_engine;
    uint8_t *rotated_output;
    size_t rotated_output_capacity;
    uint32_t max_width;
    uint32_t max_height;
    bool first_frame_logged;
    bool first_rotation_logged;
};

static size_t rgb888_capacity(uint32_t width, uint32_t height)
{
    const size_t padded_width = (width + 15U) & ~15U;
    const size_t padded_height = (height + 15U) & ~15U;
    return padded_width * padded_height * 3U;
}

esp_err_t mosaico_camera_jpeg_decoder_new(
    uint32_t max_width, uint32_t max_height,
    mosaico_camera_jpeg_decoder_handle_t *out_decoder)
{
    ESP_RETURN_ON_FALSE(max_width && max_height && out_decoder,
                        ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    mosaico_camera_jpeg_decoder_handle_t decoder = calloc(1, sizeof(*decoder));
    ESP_RETURN_ON_FALSE(decoder, ESP_ERR_NO_MEM, TAG, "allocate decoder context");

    const jpeg_decode_engine_cfg_t engine_config = {
        .intr_priority = 0,
        .timeout_ms = 80,
    };
    esp_err_t ret = jpeg_new_decoder_engine(&engine_config, &decoder->engine);
    if (ret != ESP_OK) {
        free(decoder);
        ESP_RETURN_ON_ERROR(ret, TAG, "create hardware JPEG decoder");
    }

    decoder->max_width = max_width;
    decoder->max_height = max_height;
    const size_t required = rgb888_capacity(max_width, max_height);
    const jpeg_decode_memory_alloc_cfg_t memory_config = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    decoder->output = jpeg_alloc_decoder_mem(
        required, &memory_config, &decoder->output_capacity);
    if (!decoder->output || decoder->output_capacity < required) {
        jpeg_del_decoder_engine(decoder->engine);
        free(decoder->output);
        free(decoder);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG,
                            "allocate %u-byte RGB888 output", (unsigned)required);
    }

    *out_decoder = decoder;
    ESP_LOGI(TAG, "Hardware JPEG decoder ready: max=%" PRIu32 "x%" PRIu32
                  " output=%u bytes",
             max_width, max_height, (unsigned)decoder->output_capacity);
    return ESP_OK;
}

esp_err_t mosaico_camera_jpeg_decode_rgb888(
    mosaico_camera_jpeg_decoder_handle_t decoder,
    const mosaico_camera_frame_t *jpeg_frame,
    mosaico_camera_rgb888_frame_t *out_frame)
{
    ESP_RETURN_ON_FALSE(decoder && jpeg_frame && jpeg_frame->data &&
                            jpeg_frame->size && out_frame,
                        ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(jpeg_frame->pixel_format == V4L2_PIX_FMT_JPEG,
                        ESP_ERR_NOT_SUPPORTED, TAG, "camera frame is not JPEG");

    jpeg_decode_picture_info_t picture = {0};
    ESP_RETURN_ON_ERROR(
        jpeg_decoder_get_info(jpeg_frame->data, jpeg_frame->size, &picture),
        TAG, "parse JPEG header");
    ESP_RETURN_ON_FALSE(picture.width <= decoder->max_width &&
                            picture.height <= decoder->max_height,
                        ESP_ERR_INVALID_SIZE, TAG,
                        "JPEG dimensions exceed decoder capacity");

    const jpeg_decode_cfg_t decode_config = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t output_size = 0;
    ESP_RETURN_ON_ERROR(
        jpeg_decoder_process(decoder->engine, &decode_config,
                             jpeg_frame->data, jpeg_frame->size,
                             decoder->output, decoder->output_capacity,
                             &output_size),
        TAG, "decode JPEG frame");

    *out_frame = (mosaico_camera_rgb888_frame_t) {
        .data = decoder->output,
        .size = output_size,
        .width = picture.width,
        .height = picture.height,
    };
    if (!decoder->first_frame_logged) {
        decoder->first_frame_logged = true;
        ESP_LOGI(TAG, "JPEG hardware decode active: %" PRIu32 "x%" PRIu32
                      " -> RGB888, %" PRIu32 " bytes",
                 picture.width, picture.height, output_size);
    }
    return ESP_OK;
}

static esp_err_t ensure_rotation_resources(
    mosaico_camera_jpeg_decoder_handle_t decoder)
{
    if (decoder->rotation_engine) {
        return ESP_OK;
    }

    const size_t required = rgb888_capacity(decoder->max_width, decoder->max_height);
    const jpeg_decode_memory_alloc_cfg_t memory_config = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    decoder->rotated_output = jpeg_alloc_decoder_mem(
        required, &memory_config, &decoder->rotated_output_capacity);
    if (!decoder->rotated_output || decoder->rotated_output_capacity < required) {
        free(decoder->rotated_output);
        decoder->rotated_output = NULL;
        decoder->rotated_output_capacity = 0;
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG,
                            "allocate rotated RGB888 output");
    }

    const ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t ret = ppa_register_client(&ppa_config, &decoder->rotation_engine);
    if (ret != ESP_OK) {
        free(decoder->rotated_output);
        decoder->rotated_output = NULL;
        decoder->rotated_output_capacity = 0;
        ESP_RETURN_ON_ERROR(ret, TAG, "register PPA rotation client");
    }
    return ESP_OK;
}

esp_err_t mosaico_camera_jpeg_decode_rgb888_ccw90(
    mosaico_camera_jpeg_decoder_handle_t decoder,
    const mosaico_camera_frame_t *jpeg_frame,
    mosaico_camera_rgb888_frame_t *out_frame)
{
    ESP_RETURN_ON_FALSE(decoder && jpeg_frame && out_frame,
                        ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    *out_frame = (mosaico_camera_rgb888_frame_t) {0};

    mosaico_camera_rgb888_frame_t decoded = {0};
    ESP_RETURN_ON_ERROR(
        mosaico_camera_jpeg_decode_rgb888(decoder, jpeg_frame, &decoded),
        TAG, "decode JPEG before rotation");
    ESP_RETURN_ON_ERROR(ensure_rotation_resources(decoder),
                        TAG, "initialize rotation resources");

    const ppa_srm_oper_config_t operation = {
        .in = {
            .buffer = decoded.data,
            .pic_w = decoded.width,
            .pic_h = decoded.height,
            .block_w = decoded.width,
            .block_h = decoded.height,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .out = {
            .buffer = decoder->rotated_output,
            .buffer_size = decoder->rotated_output_capacity,
            .pic_w = decoded.height,
            .pic_h = decoded.width,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ESP_RETURN_ON_ERROR(
        ppa_do_scale_rotate_mirror(decoder->rotation_engine, &operation),
        TAG, "rotate RGB888 counter-clockwise 90 degrees");

    *out_frame = (mosaico_camera_rgb888_frame_t) {
        .data = decoder->rotated_output,
        .size = (size_t)decoded.width * decoded.height * 3U,
        .width = decoded.height,
        .height = decoded.width,
    };
    if (!decoder->first_rotation_logged) {
        decoder->first_rotation_logged = true;
        ESP_LOGI(TAG, "RGB888 orientation corrected: %" PRIu32 "x%" PRIu32
                      " -> %" PRIu32 "x%" PRIu32 " (counter-clockwise 90 degrees)",
                 decoded.width, decoded.height, out_frame->width, out_frame->height);
    }
    return ESP_OK;
}

esp_err_t mosaico_camera_jpeg_decoder_del(
    mosaico_camera_jpeg_decoder_handle_t decoder)
{
    ESP_RETURN_ON_FALSE(decoder, ESP_ERR_INVALID_ARG, TAG, "invalid decoder");
    esp_err_t ret = ESP_OK;
    if (decoder->rotation_engine) {
        ret = ppa_unregister_client(decoder->rotation_engine);
    }
    esp_err_t jpeg_ret = jpeg_del_decoder_engine(decoder->engine);
    if (ret == ESP_OK) {
        ret = jpeg_ret;
    }
    free(decoder->rotated_output);
    free(decoder->output);
    free(decoder);
    return ret;
}
