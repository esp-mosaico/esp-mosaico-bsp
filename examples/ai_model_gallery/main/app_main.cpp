/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file app_main.cpp
 * @brief Cycle Face / COCO / idle detection modes with the AI button.
 *
 * Camera and PPA conversion buffers fragment PSRAM. Tear them down before
 * constructing a model, then reopen capture.
 */

#include <atomic>
#include <inttypes.h>
#include <stdio.h>

#include "bsp/esp_mosaico.h"
#include "coco_detect.hpp"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "human_face_detect.hpp"
#include "iot_button.h"
#include "linux/videodev2.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "mosaico_module_camera.h"
#include "widgets/canvas/lv_canvas.h"

static const char *TAG = "ai_gallery";

enum class Mode : int {
    Face = 0,
    Coco = 1,
    Idle = 2,
    Count,
};

static std::atomic<int> s_mode{(int)Mode::Face};
static std::atomic<bool> s_mode_changed{true};

#define FRAME_BUFFER_ALIGNMENT 128
#define PREVIEW_WIDTH BSP_LCD_H_RES
#define PREVIEW_HEIGHT BSP_LCD_V_RES

typedef struct {
    mosaico_camera_handle_t camera;
    ppa_client_handle_t ppa;
    uint8_t *rgb888;
    size_t rgb888_size;
} capture_context_t;

typedef struct {
    uint16_t *buffer;
    size_t buffer_size;
    lv_obj_t *canvas;
    lv_obj_t *status;
} display_context_t;

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

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static uint32_t align_down_even(uint32_t value)
{
    return value & ~1U;
}

static esp_err_t init_display(display_context_t *display)
{
    display->buffer_size = align_up(PREVIEW_WIDTH * PREVIEW_HEIGHT * sizeof(uint16_t), FRAME_BUFFER_ALIGNMENT);
    display->buffer = static_cast<uint16_t *>(heap_caps_aligned_calloc(
        FRAME_BUFFER_ALIGNMENT, 1, display->buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(display->buffer, ESP_ERR_NO_MEM, TAG, "allocate preview buffer failed");

    bsp_display_config_t config = BSP_DISPLAY_DEFAULT_CONFIG();
    config.enable_touch = false;
    ESP_RETURN_ON_FALSE(bsp_display_start_with_config(&config), ESP_FAIL, TAG, "start display failed");
    ESP_RETURN_ON_FALSE(bsp_display_lock(-1), ESP_ERR_TIMEOUT, TAG, "lock display failed");

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    display->canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(display->canvas, display->buffer, PREVIEW_WIDTH, PREVIEW_HEIGHT, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(display->canvas);

    display->status = lv_label_create(screen);
    lv_obj_set_style_text_color(display->status, lv_color_white(), 0);
    lv_obj_set_style_bg_color(display->status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(display->status, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(display->status, 8, 0);
    lv_obj_align(display->status, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_label_set_text(display->status, "STARTING");
    bsp_display_unlock();
    return ESP_OK;
}

static esp_err_t show_preview(capture_context_t *capture, display_context_t *display,
                              const mosaico_camera_frame_t *frame, Mode mode, size_t detections)
{
    ESP_RETURN_ON_FALSE(capture && capture->ppa && display && display->buffer && frame && frame->data,
                        ESP_ERR_INVALID_ARG, TAG, "invalid preview state");
    ESP_RETURN_ON_FALSE(frame->pixel_format == V4L2_PIX_FMT_UYVY, ESP_ERR_NOT_SUPPORTED, TAG,
                        "unsupported preview format 0x%08" PRIx32, frame->pixel_format);

    const uint32_t crop_size = align_down_even(frame->width < frame->height ? frame->width : frame->height);
    const uint32_t bytes_per_line = frame->bytes_per_line ? frame->bytes_per_line : frame->width * 2U;
    ESP_RETURN_ON_FALSE(crop_size > 0 && (bytes_per_line % 2U) == 0 && bytes_per_line / 2U >= frame->width,
                        ESP_ERR_INVALID_SIZE, TAG,
                        "invalid preview frame: %" PRIu32 "x%" PRIu32 " stride=%" PRIu32,
                        frame->width, frame->height, bytes_per_line);
    ESP_RETURN_ON_ERROR(esp_cache_msync((void *)frame->data, frame->size,
                                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE),
                        TAG, "invalidate preview input failed");
    ESP_RETURN_ON_FALSE(bsp_display_lock(-1), ESP_ERR_TIMEOUT, TAG, "lock display failed");

    esp_err_t ret = esp_cache_msync(display->buffer, display->buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (ret == ESP_OK) {
        ppa_srm_oper_config_t operation = {};
        operation.in.buffer = frame->data;
        operation.in.pic_w = bytes_per_line / 2U;
        operation.in.pic_h = frame->height;
        operation.in.block_w = crop_size;
        operation.in.block_h = crop_size;
        operation.in.block_offset_x = align_down_even((frame->width - crop_size) / 2U);
        operation.in.block_offset_y = align_down_even((frame->height - crop_size) / 2U);
        operation.in.srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY;
        operation.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
        operation.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
        operation.out.buffer = display->buffer;
        operation.out.buffer_size = display->buffer_size;
        operation.out.pic_w = PREVIEW_WIDTH;
        operation.out.pic_h = PREVIEW_HEIGHT;
        operation.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        operation.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
        operation.scale_x = (float)PREVIEW_WIDTH / (float)crop_size;
        operation.scale_y = (float)PREVIEW_HEIGHT / (float)crop_size;
        operation.mirror_y = true;
        operation.mode = PPA_TRANS_MODE_BLOCKING;
        ret = ppa_do_scale_rotate_mirror(capture->ppa, &operation);
    }
    if (ret == ESP_OK) {
        ret = esp_cache_msync(display->buffer, display->buffer_size,
                              ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    }
    if (ret == ESP_OK) {
        lv_draw_buf_t *draw_buffer = lv_canvas_get_draw_buf(display->canvas);
        if (draw_buffer) {
            lv_draw_buf_invalidate_cache(draw_buffer, NULL);
            lv_image_cache_drop(draw_buffer);
        }
        lv_label_set_text_fmt(display->status, "%s  %u", mode_name(mode), (unsigned)detections);
        lv_obj_invalidate(display->canvas);
    }
    bsp_display_unlock();
    return ret;
}

static void close_capture(capture_context_t *capture)
{
    if (capture->camera) {
        ESP_ERROR_CHECK(mosaico_camera_stop_stream(capture->camera));
        ESP_ERROR_CHECK(mosaico_camera_close(capture->camera));
        ESP_ERROR_CHECK(mosaico_camera_del(capture->camera));
        capture->camera = NULL;
    }
    if (capture->ppa) {
        ESP_ERROR_CHECK(ppa_unregister_client(capture->ppa));
        capture->ppa = NULL;
    }
    heap_caps_free(capture->rgb888);
    capture->rgb888 = NULL;
    capture->rgb888_size = 0;
}

static void open_capture(const mosaico_camera_config_t *config, capture_context_t *capture)
{
    while (true) {
        esp_err_t ret = mosaico_camera_new(config, &capture->camera);
        if (ret == ESP_OK) {
            ret = mosaico_camera_open(capture->camera);
        }
        mosaico_camera_info_t info = {};
        if (ret == ESP_OK) {
            ret = mosaico_camera_get_info(capture->camera, &info);
        }
        if (ret == ESP_OK && info.pixel_format != V4L2_PIX_FMT_UYVY) {
            ESP_LOGE(TAG, "Camera negotiated unsupported format 0x%08" PRIx32, info.pixel_format);
            ret = ESP_ERR_NOT_SUPPORTED;
        }
        if (ret == ESP_OK) {
            capture->rgb888_size = align_up((size_t)info.width * info.height * 3U, FRAME_BUFFER_ALIGNMENT);
            capture->rgb888 = static_cast<uint8_t *>(heap_caps_aligned_calloc(
                FRAME_BUFFER_ALIGNMENT, 1, capture->rgb888_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
            ret = capture->rgb888 ? ESP_OK : ESP_ERR_NO_MEM;
        }
        if (ret == ESP_OK) {
            ppa_client_config_t ppa_config = {};
            ppa_config.oper_type = PPA_OPERATION_SRM;
            ppa_config.max_pending_trans_num = 1;
            ret = ppa_register_client(&ppa_config, &capture->ppa);
        }
        if (ret == ESP_OK) {
            ret = mosaico_camera_start_stream(capture->camera);
        }
        if (ret == ESP_OK) {
            return;
        }
        if (capture->ppa) {
            ESP_ERROR_CHECK(ppa_unregister_client(capture->ppa));
            capture->ppa = NULL;
        }
        heap_caps_free(capture->rgb888);
        capture->rgb888 = NULL;
        capture->rgb888_size = 0;
        if (capture->camera) {
            ESP_ERROR_CHECK(mosaico_camera_del(capture->camera));
            capture->camera = NULL;
        }
        ESP_LOGW(TAG, "Open camera failed, retrying: %s", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static esp_err_t convert_frame(capture_context_t *capture, const mosaico_camera_frame_t *frame, bool rotate,
                               dl::image::img_t *out_image)
{
    ESP_RETURN_ON_FALSE(capture && capture->ppa && capture->rgb888 && frame && frame->data && out_image,
                        ESP_ERR_INVALID_ARG, TAG, "invalid frame conversion state");
    ESP_RETURN_ON_FALSE(frame->pixel_format == V4L2_PIX_FMT_UYVY, ESP_ERR_NOT_SUPPORTED, TAG,
                        "unsupported camera format 0x%08" PRIx32, frame->pixel_format);
    const uint32_t bytes_per_line = frame->bytes_per_line ? frame->bytes_per_line : frame->width * 2U;
    ESP_RETURN_ON_FALSE((bytes_per_line % 2U) == 0 && bytes_per_line / 2U >= frame->width,
                        ESP_ERR_INVALID_SIZE, TAG, "invalid camera stride: %" PRIu32, bytes_per_line);

    const uint32_t output_width = rotate ? frame->height : frame->width;
    const uint32_t output_height = rotate ? frame->width : frame->height;
    ESP_RETURN_ON_FALSE((size_t)output_width * output_height * 3U <= capture->rgb888_size,
                        ESP_ERR_INVALID_SIZE, TAG, "camera frame exceeds conversion buffer");
    ESP_RETURN_ON_ERROR(esp_cache_msync((void *)frame->data, frame->size,
                                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE),
                        TAG, "invalidate camera frame cache failed");

    ppa_srm_oper_config_t operation = {};
    operation.in.buffer = frame->data;
    operation.in.pic_w = bytes_per_line / 2U;
    operation.in.pic_h = frame->height;
    operation.in.block_w = frame->width;
    operation.in.block_h = frame->height;
    operation.in.srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY;
    operation.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
    operation.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    operation.out.buffer = capture->rgb888;
    operation.out.buffer_size = capture->rgb888_size;
    operation.out.pic_w = output_width;
    operation.out.pic_h = output_height;
    operation.out.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
    operation.rotation_angle = rotate ? PPA_SRM_ROTATION_ANGLE_90 : PPA_SRM_ROTATION_ANGLE_0;
    operation.scale_x = 1.0f;
    operation.scale_y = 1.0f;
    operation.mode = PPA_TRANS_MODE_BLOCKING;
    ESP_RETURN_ON_ERROR(ppa_do_scale_rotate_mirror(capture->ppa, &operation), TAG, "convert camera frame with PPA failed");
    ESP_RETURN_ON_ERROR(esp_cache_msync(capture->rgb888, capture->rgb888_size,
                                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE),
                        TAG, "invalidate converted frame cache failed");

    *out_image = {
        .data = capture->rgb888,
        .width = static_cast<uint16_t>(output_width),
        .height = static_cast<uint16_t>(output_height),
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_BGR888,
    };
    return ESP_OK;
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
    const esp_err_t led_ret = bsp_led_init();
    if (led_ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Status LED unavailable; continuing without LED feedback");
    } else {
        ESP_ERROR_CHECK(led_ret);
    }
    const bool led_available = led_ret == ESP_OK;
    ESP_ERROR_CHECK(setup_button());

    display_context_t display = {};
    ESP_ERROR_CHECK(init_display(&display));

    mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    config.buffer_count = 1;
    config.allow_unidentified = true;

    capture_context_t capture = {};
    HumanFaceDetect *face = nullptr;
    COCODetect *coco = nullptr;
    Mode active = Mode::Count;
    unsigned idle_frames = 0;
    bool preview_started = false;

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

        close_capture(&capture);
        teardown_models();
        log_psram_headroom("before model");

        active = mode;
        s_mode_changed.store(false);
        if (mode == Mode::Face) {
            face = new HumanFaceDetect(
                static_cast<HumanFaceDetect::model_type_t>(CONFIG_DEFAULT_HUMAN_FACE_DETECT_MODEL), false);
            if (led_available) {
                (void)bsp_led_set(true);
            }
        } else if (mode == Mode::Coco) {
            coco = new COCODetect(
                static_cast<COCODetect::model_type_t>(CONFIG_DEFAULT_COCO_DETECT_MODEL), false);
            if (led_available) {
                (void)bsp_led_set(true);
            }
        } else {
            if (led_available) {
                (void)bsp_led_set(false);
            }
        }

        log_psram_headroom("after model");
        open_capture(&config, &capture);
        ESP_LOGI(TAG, "active model=%s", mode_name(mode));
        idle_frames = 0;
    };

    while (true) {
        ensure_mode((Mode)s_mode.load());

        mosaico_camera_frame_t frame = {};
        if (mosaico_camera_get_frame(capture.camera, &frame) != ESP_OK) {
            continue;
        }

        size_t detections = 0;
        if (active != Mode::Idle) {
            dl::image::img_t img = {};
            const esp_err_t ret = convert_frame(&capture, &frame, active == Mode::Face, &img);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Frame conversion failed: %s", esp_err_to_name(ret));
                ESP_ERROR_CHECK(mosaico_camera_return_frame(capture.camera, &frame));
                continue;
            }

            if (active == Mode::Face && face) {
                auto &results = face->run(img);
                detections = results.size();
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
                detections = results.size();
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

        const esp_err_t preview_ret = show_preview(&capture, &display, &frame, active, detections);
        if (preview_ret != ESP_OK) {
            ESP_LOGE(TAG, "Preview failed: %s", esp_err_to_name(preview_ret));
        } else if (!preview_started) {
            preview_started = true;
            ESP_LOGI(TAG, "LCD preview started");
        }

        ESP_ERROR_CHECK(mosaico_camera_return_frame(capture.camera, &frame));
    }
}
