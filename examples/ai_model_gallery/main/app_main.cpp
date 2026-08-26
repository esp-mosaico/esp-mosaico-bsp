/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file app_main.cpp
 * @brief Cycle Face / COCO / idle detection modes with the AI button.
 *
 * Camera JPEG buffers and the hardware JPEG RGB888 frame fragment PSRAM.
 * Tear them down before constructing a model, then reopen capture. Do not
 * use the CCW90 decode path: its second RGB888 buffer will not fit beside
 * YOLO11n.
 */

#include <atomic>

#include "bsp/esp_mosaico.h"
#include "coco_detect.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "human_face_detect.hpp"
#include "iot_button.h"
#include "mosaico_camera.h"

static const char *TAG = "ai_gallery";

enum class Mode : int {
    Face = 0,
    Coco = 1,
    Idle = 2,
    Count,
};

static std::atomic<int> s_mode{(int)Mode::Face};
static std::atomic<bool> s_mode_changed{true};

static const char *mode_name(Mode mode)
{
    switch (mode) {
    case Mode::Face:
        return "face";
    case Mode::Coco:
        return "coco";
    case Mode::Idle:
        return "idle";
    default:
        return "?";
    }
}

static void button_event_cb(void *button_handle, void *user_data)
{
    (void)user_data;
    if (iot_button_get_event(static_cast<button_handle_t>(button_handle)) != BUTTON_SINGLE_CLICK) {
        return;
    }
    const int next = (s_mode.load() + 1) % (int)Mode::Count;
    s_mode.store(next);
    s_mode_changed.store(true);
    ESP_LOGI(TAG, "mode -> %s", mode_name((Mode)next));
}

static esp_err_t setup_button(void)
{
    button_handle_t buttons[BSP_BUTTON_NUM] = {};
    int count = 0;
    ESP_ERROR_CHECK(bsp_iot_button_create(buttons, &count, BSP_BUTTON_NUM));
    return iot_button_register_cb(buttons[BSP_BUTTON_AI], BUTTON_SINGLE_CLICK, NULL,
                                  button_event_cb, NULL);
}

static void close_capture(mosaico_camera_handle_t *camera,
                          mosaico_camera_jpeg_decoder_handle_t *decoder)
{
    if (camera && *camera) {
        ESP_ERROR_CHECK(mosaico_camera_del(*camera));
        *camera = NULL;
    }
    if (decoder && *decoder) {
        ESP_ERROR_CHECK(mosaico_camera_jpeg_decoder_del(*decoder));
        *decoder = NULL;
    }
}

static void open_capture(const mosaico_camera_config_t *config,
                         mosaico_camera_handle_t *camera,
                         mosaico_camera_jpeg_decoder_handle_t *decoder)
{
    ESP_ERROR_CHECK(mosaico_camera_jpeg_decoder_new(
        config->width, config->height, decoder));
    while (mosaico_camera_new(config, camera) != ESP_OK) {
        ESP_LOGW(TAG, "Camera not found, retrying...");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void log_psram_headroom(const char *checkpoint)
{
    ESP_LOGI(TAG, "%s: PSRAM free=%u largest=%u simd_largest=%u",
             checkpoint,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_SIMD));
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "AI model gallery — press AI to cycle face/coco/idle");
    ESP_ERROR_CHECK(bsp_led_init());
    ESP_ERROR_CHECK(setup_button());

    mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    config.pixel_format = MOSAICO_CAMERA_PIXEL_FORMAT_JPEG;
    config.buffer_count = 2;
    config.allow_unidentified = true;

    mosaico_camera_jpeg_decoder_handle_t decoder = NULL;
    mosaico_camera_handle_t camera = NULL;
    HumanFaceDetect *face = nullptr;
    COCODetect *coco = nullptr;
    Mode active = Mode::Count;
    unsigned idle_frames = 0;

    auto teardown_models = [&]() {
        delete face;
        face = nullptr;
        delete coco;
        coco = nullptr;
    };

    auto ensure_mode = [&](Mode mode) {
        if (mode == active && !s_mode_changed.load()) {
            return;
        }

        close_capture(&camera, &decoder);
        teardown_models();
        log_psram_headroom("before model");

        active = mode;
        s_mode_changed.store(false);
        if (mode == Mode::Face) {
            face = new HumanFaceDetect(
                static_cast<HumanFaceDetect::model_type_t>(CONFIG_DEFAULT_HUMAN_FACE_DETECT_MODEL), false);
            (void)bsp_led_set(true);
        } else if (mode == Mode::Coco) {
            coco = new COCODetect(
                static_cast<COCODetect::model_type_t>(CONFIG_DEFAULT_COCO_DETECT_MODEL), false);
            (void)bsp_led_set(true);
        } else {
            (void)bsp_led_set(false);
        }

        log_psram_headroom("after model");
        open_capture(&config, &camera, &decoder);
        ESP_LOGI(TAG, "active model=%s", mode_name(mode));
        idle_frames = 0;
    };

    while (true) {
        ensure_mode((Mode)s_mode.load());

        mosaico_camera_frame_t frame = {};
        if (mosaico_camera_get_frame(camera, &frame) != ESP_OK) {
            continue;
        }

        if (active != Mode::Idle) {
            mosaico_camera_rgb888_frame_t rgb = {};
            /* Face fits two RGB888 buffers; COCO does not. Skip rotation for COCO. */
            const esp_err_t ret = (active == Mode::Coco)
                ? mosaico_camera_jpeg_decode_rgb888(decoder, &frame, &rgb)
                : mosaico_camera_jpeg_decode_rgb888_ccw90(decoder, &frame, &rgb);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(ret));
                ESP_ERROR_CHECK(mosaico_camera_return_frame(camera, &frame));
                continue;
            }
            dl::image::img_t img = {
                .data = rgb.data,
                .width = (uint16_t)rgb.width,
                .height = (uint16_t)rgb.height,
                .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
            };

            if (active == Mode::Face && face) {
                auto &results = face->run(img);
                if (results.empty()) {
                    if ((++idle_frames % 25U) == 0U) {
                        ESP_LOGI(TAG, "face: no detection");
                    }
                } else {
                    idle_frames = 0;
                    for (const auto &res : results) {
                        ESP_LOGI(TAG, "face score=%.3f box=[%d,%d,%d,%d]",
                                 res.score, res.box[0], res.box[1], res.box[2], res.box[3]);
                    }
                }
            } else if (active == Mode::Coco && coco) {
                auto &results = coco->run(img);
                if (results.empty()) {
                    if ((++idle_frames % 25U) == 0U) {
                        ESP_LOGI(TAG, "coco: no detection");
                    }
                } else {
                    idle_frames = 0;
                    for (const auto &res : results) {
                        ESP_LOGI(TAG, "coco cat=%d score=%.3f box=[%d,%d,%d,%d]",
                                 res.category, res.score,
                                 res.box[0], res.box[1], res.box[2], res.box[3]);
                    }
                }
            }
        }

        mosaico_camera_return_frame(camera, &frame);
    }
}
