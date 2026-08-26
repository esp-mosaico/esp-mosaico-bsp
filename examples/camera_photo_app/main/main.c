/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/display.h"
#include "bsp/esp_mosaico.h"
#include "camera_settings.h"
#include "camera_ui.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_enc.h"
#include "linux/videodev2.h"
#include "mosaico_button_led.h"
#include "mosaico_camera.h"
#include "mosaico_module_mgr.h"
#include "photo_store.h"
#include "photo_usb_msc.h"

#define PREVIEW_WIDTH              BSP_LCD_H_RES
#define PREVIEW_HEIGHT             BSP_LCD_V_RES
#define PREVIEW_CROP_WIDTH         768
#define PREVIEW_CROP_HEIGHT        768
#define PREVIEW_SCALE              (480.0f / 768.0f)
#define PREVIEW_BUFFER_ALIGNMENT   128
#define CAPTURE_FAILURE_LIMIT      3
#define CAMERA_RETRY_DELAY_MS      500
#define FLASH_EXPOSURE_SETTLE_FRAMES 2
#define FLASH_PIPELINE_FLUSH_FRAMES  1
#define JPEG_QUALITY               80
#define MAX_JPEG_SIZE              (256 * 1024)
#define BUTTON_SUBBOARD_POLL_MS    50
#define PREVIEW_HEARTBEAT_MS       5000
#define PREVIEW_SLOW_GET_FRAME_MS  200
#define PREVIEW_SLOW_CONVERT_MS    80
#define PREVIEW_STALL_WARN_MS      1500

static const char *TAG = "camera_photo";

typedef enum {
    APP_MODE_CAMERA = 0,
    APP_MODE_GALLERY,
} app_mode_t;

typedef struct {
    mosaico_camera_handle_t camera;
    ppa_client_handle_t ppa;
    uint16_t *ppa_buffer;
    uint16_t *thumb_rgb_buffer;
    uint8_t *jpeg_buffer;
    uint8_t *jpeg_load_buffer;
    size_t buffer_size;
    app_mode_t mode;
    bool flash_enabled;
    bool preview_flip;
    bool camera_power_on;
    uint32_t gallery_index;
} app_context_t;

static app_context_t s_app;
static volatile bool s_button_board_removed;
static volatile bool s_capture_pending;
static volatile bool s_gallery_load_pending;
static volatile uint32_t s_gallery_load_index;
static volatile bool s_thumb_refresh_pending;

static volatile bool s_thumb_refresh_pending;

typedef struct {
    uint32_t preview_ok;
    uint32_t preview_fail;
    uint32_t capture_ok;
    uint32_t capture_fail;
    uint32_t stream_restarts;
    TickType_t session_start_tick;
    TickType_t last_preview_ok_tick;
    TickType_t last_heartbeat_tick;
    TickType_t last_preview_fail_tick;
    esp_err_t last_preview_fail_err;
} preview_diag_t;

static preview_diag_t s_preview_diag;
static uint32_t s_last_heartbeat_preview_ok;
static uint32_t s_last_heartbeat_ui_async_ok;

static esp_err_t perform_capture(void);

static uint32_t ticks_to_ms(TickType_t ticks)
{
    return (uint32_t)(ticks * portTICK_PERIOD_MS);
}

static uint32_t elapsed_ms_since(TickType_t since)
{
    return ticks_to_ms(xTaskGetTickCount() - since);
}

static void preview_diag_reset(void)
{
    memset(&s_preview_diag, 0, sizeof(s_preview_diag));
    const TickType_t now = xTaskGetTickCount();
    s_preview_diag.session_start_tick = now;
    s_preview_diag.last_preview_ok_tick = now;
    s_preview_diag.last_heartbeat_tick = now;
    s_last_heartbeat_preview_ok = 0;
    s_last_heartbeat_ui_async_ok = 0;
    camera_ui_reset_preview_refresh_stats();
}

static void log_camera_pipeline(const char *reason)
{
    if (!s_app.camera) {
        ESP_LOGW(TAG, "%s: camera handle is null", reason);
        return;
    }

    mosaico_camera_pipeline_stats_t stats = {0};
    const esp_err_t ret =
        mosaico_camera_get_pipeline_stats(s_app.camera, &stats);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s: read pipeline stats failed: %s", reason,
                 esp_err_to_name(ret));
        return;
    }

    ESP_LOGW(TAG,
             "%s: streaming=%d power_down=%d outstanding=%u/%u",
             reason, stats.streaming, stats.power_down,
             stats.outstanding_count, stats.buffer_count);
}

static void log_app_state(const char *reason)
{
    ESP_LOGI(TAG,
             "%s: mode=%d power_on=%d flash=%d cap_pending=%d "
             "gal_load=%d thumb_refresh=%d heap=%u min_heap=%u",
             reason, (int)s_app.mode, s_app.camera_power_on,
             s_app.flash_enabled, (int)s_capture_pending,
             (int)s_gallery_load_pending, (int)s_thumb_refresh_pending,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
}

static void preview_diag_note_failure(esp_err_t err, const char *stage)
{
    ++s_preview_diag.preview_fail;
    s_preview_diag.last_preview_fail_err = err;
    s_preview_diag.last_preview_fail_tick = xTaskGetTickCount();
    ESP_LOGW(TAG, "Preview %s failed: %s (fail=%" PRIu32 ")",
             stage, esp_err_to_name(err), s_preview_diag.preview_fail);
    log_camera_pipeline(stage);
    log_app_state("preview failure");
}

static void preview_diag_maybe_heartbeat(void)
{
    const TickType_t now = xTaskGetTickCount();
    if (elapsed_ms_since(s_preview_diag.last_heartbeat_tick) <
        PREVIEW_HEARTBEAT_MS) {
        return;
    }

    s_preview_diag.last_heartbeat_tick = now;
    const uint32_t session_ms =
        ticks_to_ms(now - s_preview_diag.session_start_tick);
    const uint32_t since_ok_ms =
        elapsed_ms_since(s_preview_diag.last_preview_ok_tick);
    const float fps =
        session_ms > 0
            ? (1000.0f * (float)s_preview_diag.preview_ok / (float)session_ms)
            : 0.0f;

    camera_ui_preview_refresh_stats_t ui_stats = {0};
    camera_ui_get_preview_refresh_stats(&ui_stats);
    const uint32_t preview_delta =
        s_preview_diag.preview_ok - s_last_heartbeat_preview_ok;
    const uint32_t ui_async_delta =
        ui_stats.async_ok - s_last_heartbeat_ui_async_ok;
    s_last_heartbeat_preview_ok = s_preview_diag.preview_ok;
    s_last_heartbeat_ui_async_ok = ui_stats.async_ok;

    ESP_LOGI(TAG,
             "Preview heartbeat: ok=%" PRIu32 "(+%" PRIu32 ") fail=%" PRIu32
             " capture_ok=%" PRIu32 " capture_fail=%" PRIu32
             " restarts=%" PRIu32 " fps=%.1f since_ok=%" PRIu32 "ms",
             s_preview_diag.preview_ok, preview_delta,
             s_preview_diag.preview_fail,
             s_preview_diag.capture_ok, s_preview_diag.capture_fail,
             s_preview_diag.stream_restarts, fps, since_ok_ms);
    ESP_LOGI(TAG,
             "UI refresh: async_ok=%" PRIu32 "(+%" PRIu32 ") coalesced=%" PRIu32
             " async_fail=%" PRIu32,
             ui_stats.async_ok, ui_async_delta, ui_stats.coalesced,
             ui_stats.async_fail);
    log_camera_pipeline("heartbeat");
    log_app_state("heartbeat");

    if (s_app.camera_power_on && preview_delta > 0U && ui_async_delta == 0U) {
        ESP_LOGE(TAG,
                 "UI refresh stalled: preview advanced by %" PRIu32
                 " frames but no LVGL refresh executed",
                 preview_delta);
    }

    if (s_app.camera_power_on && since_ok_ms >= PREVIEW_STALL_WARN_MS) {
        ESP_LOGW(TAG,
                 "Preview stall suspected: no successful frame for %" PRIu32
                 "ms (last_fail=%s)",
                 since_ok_ms,
                 esp_err_to_name(s_preview_diag.last_preview_fail_err));
    }

    if (s_app.camera) {
        mosaico_camera_pipeline_stats_t stats = {0};
        if (mosaico_camera_get_pipeline_stats(s_app.camera, &stats) == ESP_OK &&
            stats.outstanding_count >= stats.buffer_count &&
            stats.buffer_count > 0) {
            ESP_LOGE(TAG,
                     "All camera buffers outstanding (%u/%u), preview may stall",
                     stats.outstanding_count, stats.buffer_count);
        }
    }
}

static void request_capture(void)
{
    ESP_LOGI(TAG, "Capture requested");
    s_capture_pending = true;
}

static void app_set_preview_flip(bool flipped)
{
    s_app.preview_flip = flipped;
    camera_ui_set_preview_flip(flipped);
    const esp_err_t ret = camera_settings_save_preview_flip(flipped);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Save preview flip setting failed: %s",
                 esp_err_to_name(ret));
    }
}

static void app_set_flash_enabled(bool enabled)
{
    s_app.flash_enabled = enabled;
    camera_ui_set_flash_enabled(enabled);
    if (!enabled && s_app.camera) {
        (void)mosaico_camera_flash_stop(s_app.camera);
    }
}

static void app_toggle_flash(void)
{
    app_set_flash_enabled(!s_app.flash_enabled);
    ESP_LOGI(TAG, "Flash %s", s_app.flash_enabled ? "enabled" : "disabled");
}

static void show_camera_preview_blank(void)
{
    memset(s_app.ppa_buffer, 0, s_app.buffer_size);
    camera_ui_set_camera_power_on(false);
}

static void app_toggle_camera_power(void)
{
    if (!s_app.camera) {
        return;
    }

    const bool enable = !s_app.camera_power_on;
    if (!enable) {
        s_app.camera_power_on = false;
        (void)mosaico_camera_flash_stop(s_app.camera);
        show_camera_preview_blank();
    }

    const esp_err_t ret =
        mosaico_camera_set_power_down(s_app.camera, !enable);
    if (ret != ESP_OK) {
        if (!enable) {
            s_app.camera_power_on = true;
            camera_ui_set_camera_power_on(true);
        }
        ESP_LOGW(TAG, "Toggle camera power failed: %s", esp_err_to_name(ret));
        return;
    }

    if (enable) {
        s_app.camera_power_on = true;
        camera_ui_set_camera_power_on(true);
        s_preview_diag.last_preview_ok_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "Camera enabled (PWDN=0, normal mode)");
    } else {
        ESP_LOGI(TAG, "Camera disabled (PWDN=1, sleep mode)");
    }
    log_camera_pipeline("camera power toggled");
    log_app_state("camera power toggled");
}

static void top_button_cb(void *button_handle, void *user_data)
{
    (void)user_data;
    if (iot_button_get_event(button_handle) != BUTTON_LONG_PRESS_START) {
        return;
    }
    app_toggle_camera_power();
}

static esp_err_t init_top_button(void)
{
    button_handle_t buttons[BSP_BUTTON_NUM] = {0};
    int button_count = 0;
    ESP_RETURN_ON_ERROR(
        bsp_iot_button_create(buttons, &button_count, BSP_BUTTON_NUM),
        TAG, "create top button failed");
    ESP_RETURN_ON_ERROR(
        iot_button_register_cb(
            buttons[BSP_BUTTON_AI], BUTTON_LONG_PRESS_START, NULL,
            top_button_cb, NULL),
        TAG, "register top button callback failed");
    ESP_LOGI(TAG, "Top button ready on GPIO%d (long press toggles camera)",
             BSP_BUTTON_AI_GPIO);
    return ESP_OK;
}

static void subboard_event_callback(mosaico_module_mgr_event_t event,
                                    const mosaico_module_mgr_info_t *info,
                                    void *user_data)
{
    (void)user_data;

    if (event == MOSAICO_MODULE_MGR_EVENT_REMOVED &&
        info->eeprom.board_type == MOSAICO_BOARD_TYPE_BUTTON_LED) {
        s_button_board_removed = true;
        ESP_LOGI(TAG, "Button sub-board removed from %s slot",
                 mosaico_module_mgr_slot_to_name(info->slot));
    }
}

static esp_err_t init_subboard_manager(void)
{
    const mosaico_module_mgr_config_t manager_config = {
        .scan_period_ms = 200,
        .debounce_count = 3,
        .event_callback = subboard_event_callback,
        .event_user_data = NULL,
    };
    return mosaico_module_mgr_init(&manager_config);
}

static void button_subboard_task(void *arg)
{
    (void)arg;

    while (true) {
        mosaico_button_led_handle_t board = NULL;
        const esp_err_t open_ret = mosaico_button_led_new(NULL, &board);
        if (open_ret != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        mosaico_button_led_info_t info = {0};
        if (mosaico_button_led_get_info(board, &info) == ESP_OK) {
            ESP_LOGI(TAG,
                     "Button sub-board ready in %s slot (KEY1=flash, KEY2=capture)",
                     mosaico_module_mgr_slot_to_name(info.slot));
        }

        bool last_key1 = false;
        bool last_key2 = false;
        s_button_board_removed = false;

        while (!s_button_board_removed) {
            bool key1 = false;
            bool key2 = false;
            const esp_err_t read_ret =
                mosaico_button_led_read_keys(board, &key1, &key2);
            if (read_ret != ESP_OK) {
                ESP_LOGW(TAG, "Read button sub-board keys failed: %s",
                         esp_err_to_name(read_ret));
                break;
            }

            if (key2 && !last_key2 && s_app.mode == APP_MODE_CAMERA) {
                request_capture();
            }

            if (key1 && !last_key1) {
                app_toggle_flash();
            }

            last_key1 = key1;
            last_key2 = key2;
            vTaskDelay(pdMS_TO_TICKS(BUTTON_SUBBOARD_POLL_MS));
        }

        const esp_err_t close_ret = mosaico_button_led_del(board);
        if (close_ret != ESP_OK) {
            ESP_LOGW(TAG, "Close button sub-board failed: %s",
                     esp_err_to_name(close_ret));
        }
    }
}

static esp_err_t start_button_subboard_task(void)
{
    const BaseType_t created = xTaskCreate(
        button_subboard_task, "btn_subboard", 4096, NULL, 4, NULL);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static uint32_t align_down_even(uint32_t value)
{
    return value & ~1U;
}

static esp_err_t app_alloc_buffers(void)
{
    s_app.buffer_size = align_up(
        PREVIEW_WIDTH * PREVIEW_HEIGHT * sizeof(uint16_t),
        PREVIEW_BUFFER_ALIGNMENT);
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;

    s_app.ppa_buffer = heap_caps_aligned_calloc(
        PREVIEW_BUFFER_ALIGNMENT, 1, s_app.buffer_size, caps);
    s_app.thumb_rgb_buffer = heap_caps_aligned_calloc(
        PREVIEW_BUFFER_ALIGNMENT, 1, s_app.buffer_size, caps);
    s_app.jpeg_buffer = heap_caps_malloc(MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_app.jpeg_load_buffer = heap_caps_malloc(MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_RETURN_ON_FALSE(
        s_app.ppa_buffer && s_app.thumb_rgb_buffer && s_app.jpeg_buffer &&
            s_app.jpeg_load_buffer,
        ESP_ERR_NO_MEM, TAG, "allocate app buffers failed");
    return ESP_OK;
}

static esp_err_t app_init(void)
{
    ESP_RETURN_ON_ERROR(app_alloc_buffers(), TAG, "allocate buffers failed");

    bsp_display_config_t display_config = BSP_DISPLAY_DEFAULT_CONFIG();
    display_config.enable_touch = true;
    lv_display_t *display = bsp_display_start_with_config(&display_config);
    ESP_RETURN_ON_FALSE(display, ESP_FAIL, TAG, "start LVGL display failed");

    const ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ESP_RETURN_ON_ERROR(
        ppa_register_client(&ppa_config, &s_app.ppa),
        TAG, "register PPA client failed");

    return ESP_OK;
}

static esp_err_t camera_wait_and_open(void)
{
    mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    config.allow_unidentified = true;

    while (true) {
        const esp_err_t ret = mosaico_camera_new(&config, &s_app.camera);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Opening the camera in the LEFT slot failed, retrying: %s",
                 esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(CAMERA_RETRY_DELAY_MS));
    }
}

static esp_err_t preview_convert_frame(const mosaico_camera_frame_t *frame)
{
    ESP_RETURN_ON_FALSE(
        frame && frame->data, ESP_ERR_INVALID_ARG, TAG, "invalid camera frame");
    ESP_RETURN_ON_FALSE(
        frame->pixel_format == V4L2_PIX_FMT_UYVY,
        ESP_ERR_NOT_SUPPORTED, TAG,
        "unsupported camera format 0x%08" PRIx32, frame->pixel_format);
    ESP_RETURN_ON_FALSE(
        frame->width >= PREVIEW_CROP_WIDTH && frame->height >= PREVIEW_CROP_HEIGHT,
        ESP_ERR_INVALID_SIZE, TAG,
        "camera frame is smaller than the crop window");

    const uint32_t bytes_per_line =
        frame->bytes_per_line ? frame->bytes_per_line : frame->width * 2U;

    ESP_RETURN_ON_ERROR(
        esp_cache_msync(
            (void *)frame->data, frame->size,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C |
            ESP_CACHE_MSYNC_FLAG_INVALIDATE),
        TAG, "invalidate camera frame cache failed");
    ESP_RETURN_ON_ERROR(
        esp_cache_msync(
            s_app.ppa_buffer, s_app.buffer_size,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M),
        TAG, "clean PPA output cache failed");

    const ppa_srm_oper_config_t operation = {
        .in = {
            .buffer = frame->data,
            .pic_w = bytes_per_line / 2U,
            .pic_h = frame->height,
            .block_w = PREVIEW_CROP_WIDTH,
            .block_h = PREVIEW_CROP_HEIGHT,
            .block_offset_x =
                align_down_even((frame->width - PREVIEW_CROP_WIDTH) / 2U),
            .block_offset_y =
                align_down_even((frame->height - PREVIEW_CROP_HEIGHT) / 2U),
            .srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = s_app.ppa_buffer,
            .buffer_size = s_app.buffer_size,
            .pic_w = PREVIEW_WIDTH,
            .pic_h = PREVIEW_HEIGHT,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
        .scale_x = PREVIEW_SCALE,
        .scale_y = PREVIEW_SCALE,
        .mirror_x = s_app.preview_flip,
        .mirror_y = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ESP_RETURN_ON_ERROR(
        ppa_do_scale_rotate_mirror(s_app.ppa, &operation),
        TAG, "convert camera frame with PPA failed");
    ESP_RETURN_ON_ERROR(
        esp_cache_msync(
            s_app.ppa_buffer, s_app.buffer_size,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C |
            ESP_CACHE_MSYNC_FLAG_INVALIDATE),
        TAG, "invalidate PPA output cache failed");
    return ESP_OK;
}

static esp_err_t encode_preview_jpeg(int *out_size)
{
    jpeg_enc_config_t enc_config = DEFAULT_JPEG_ENC_CONFIG();
    enc_config.width = PREVIEW_WIDTH;
    enc_config.height = PREVIEW_HEIGHT;
    enc_config.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    enc_config.subsampling = JPEG_SUBSAMPLE_420;
    enc_config.quality = JPEG_QUALITY;

    jpeg_enc_handle_t encoder = NULL;
    jpeg_error_t jpeg_ret = jpeg_enc_open(&enc_config, &encoder);
    ESP_RETURN_ON_FALSE(jpeg_ret == JPEG_ERR_OK, ESP_FAIL, TAG,
                        "open JPEG encoder failed: %d", jpeg_ret);

    const size_t input_size = PREVIEW_WIDTH * PREVIEW_HEIGHT * 2U;
    jpeg_ret = jpeg_enc_process(
        encoder, (uint8_t *)s_app.ppa_buffer, (int)input_size,
        s_app.jpeg_buffer, MAX_JPEG_SIZE, out_size);
    jpeg_enc_close(encoder);

    ESP_RETURN_ON_FALSE(jpeg_ret == JPEG_ERR_OK && *out_size > 0, ESP_FAIL, TAG,
                        "JPEG encode failed: %d", jpeg_ret);
    return ESP_OK;
}

static void flash_capture_cleanup(const mosaico_camera_flash_state_t *flash_state)
{
    if (!s_app.camera) {
        return;
    }
    const esp_err_t ret =
        mosaico_camera_restore_flash_capture(s_app.camera, flash_state);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Restore camera after flash capture failed: %s",
                 esp_err_to_name(ret));
    }
}

static esp_err_t perform_capture(void)
{
    const TickType_t capture_start = xTaskGetTickCount();
    const bool use_flash = s_app.flash_enabled;
    mosaico_camera_flash_state_t flash_state = {0};
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "Capture begin: flash=%d", use_flash);
    log_camera_pipeline("capture begin");

    if (use_flash) {
        const esp_err_t prep_ret =
            mosaico_camera_prepare_flash_capture(s_app.camera, &flash_state);
        if (prep_ret != ESP_OK) {
            ESP_LOGW(TAG, "Prepare flash capture failed: %s",
                     esp_err_to_name(prep_ret));
            return prep_ret;
        }
        for (uint32_t i = 0; i < FLASH_EXPOSURE_SETTLE_FRAMES; ++i) {
            ret = mosaico_camera_discard_frames(s_app.camera, 1);
            if (ret != ESP_OK) {
                flash_capture_cleanup(&flash_state);
                ESP_RETURN_ON_ERROR(
                    ret, TAG,
                    "discard flash exposure settle frame %" PRIu32 " failed",
                    i + 1U);
            }
        }
        const esp_err_t flash_ret = mosaico_camera_flash_trigger(s_app.camera);
        if (flash_ret != ESP_OK) {
            ESP_LOGW(TAG, "Enable GPIO34 flash failed: %s",
                     esp_err_to_name(flash_ret));
            flash_capture_cleanup(&flash_state);
            return flash_ret;
        }
        ret = mosaico_camera_discard_frames(s_app.camera,
                                            FLASH_PIPELINE_FLUSH_FRAMES);
        if (ret != ESP_OK) {
            flash_capture_cleanup(&flash_state);
            ESP_RETURN_ON_ERROR(ret, TAG,
                                "flush pre-flash pipeline frames failed");
        }
    }

    mosaico_camera_frame_t frame = {0};

    ret = mosaico_camera_get_frame(s_app.camera, &frame);
    if (ret != ESP_OK) {
        if (use_flash) {
            flash_capture_cleanup(&flash_state);
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "get camera frame for capture failed");
    }
    if (use_flash) {
        (void)mosaico_camera_flash_stop(s_app.camera);
    }

    ret = preview_convert_frame(&frame);
    const esp_err_t return_ret =
        mosaico_camera_return_frame(s_app.camera, &frame);
    if (return_ret != ESP_OK) {
        if (use_flash) {
            flash_capture_cleanup(&flash_state);
        }
        ESP_RETURN_ON_ERROR(return_ret, TAG,
                            "return camera frame after capture failed");
    }
    if (ret != ESP_OK) {
        if (use_flash) {
            flash_capture_cleanup(&flash_state);
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "prepare capture frame failed");
    }

    ESP_RETURN_ON_ERROR(
        esp_cache_msync(
            s_app.ppa_buffer, s_app.buffer_size,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C |
            ESP_CACHE_MSYNC_FLAG_INVALIDATE),
        TAG, "invalidate capture buffer before encode failed");

    int jpeg_size = 0;
    ret = encode_preview_jpeg(&jpeg_size);
    if (ret != ESP_OK) {
        if (use_flash) {
            flash_capture_cleanup(&flash_state);
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "encode photo failed");
    }

    photo_store_info_t saved = {0};
    ret = photo_store_save_jpeg(s_app.jpeg_buffer, (size_t)jpeg_size, &saved);
    if (ret != ESP_OK) {
        if (use_flash) {
            flash_capture_cleanup(&flash_state);
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "save photo failed");
    }

    if (use_flash) {
        flash_capture_cleanup(&flash_state);
    }

    const esp_err_t usb_export_ret =
        photo_usb_msc_export_jpeg(saved.filename, s_app.jpeg_buffer,
                                  (size_t)jpeg_size);
    if (usb_export_ret != ESP_OK &&
        usb_export_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Export %s to USB storage failed: %s", saved.filename,
                 esp_err_to_name(usb_export_ret));
    }

    camera_ui_set_thumb(s_app.ppa_buffer, PREVIEW_WIDTH, PREVIEW_HEIGHT);
    camera_ui_invalidate_preview();
    ++s_preview_diag.capture_ok;
    ESP_LOGI(TAG, "Capture done in %" PRIu32 "ms: %s (%d bytes)",
             elapsed_ms_since(capture_start), saved.filename, jpeg_size);
    log_camera_pipeline("capture done");
    return ESP_OK;
}

static esp_err_t decode_jpeg_to_rgb565(const uint8_t *jpeg_data, size_t jpeg_size,
                                       uint16_t *out_buffer, size_t out_buffer_size,
                                       int *out_width, int *out_height)
{
    jpeg_dec_config_t dec_config = DEFAULT_JPEG_DEC_CONFIG();
    dec_config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;

    jpeg_dec_handle_t decoder = NULL;
    jpeg_error_t jpeg_ret = jpeg_dec_open(&dec_config, &decoder);
    ESP_RETURN_ON_FALSE(jpeg_ret == JPEG_ERR_OK, ESP_FAIL, TAG,
                        "open JPEG decoder failed: %d", jpeg_ret);

    jpeg_dec_io_t io = {
        .inbuf = (uint8_t *)jpeg_data,
        .inbuf_len = (int)jpeg_size,
        .inbuf_remain = 0,
        .outbuf = (uint8_t *)out_buffer,
        .out_size = 0,
    };

    jpeg_dec_header_info_t header = {0};
    jpeg_ret = jpeg_dec_parse_header(decoder, &io, &header);
    if (jpeg_ret != JPEG_ERR_OK) {
        jpeg_dec_close(decoder);
        ESP_RETURN_ON_FALSE(false, ESP_FAIL, TAG, "parse JPEG header failed: %d",
                            jpeg_ret);
    }

    int outbuf_len = 0;
    jpeg_ret = jpeg_dec_get_outbuf_len(decoder, &outbuf_len);
    if (jpeg_ret != JPEG_ERR_OK || (size_t)outbuf_len > out_buffer_size) {
        jpeg_dec_close(decoder);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_SIZE, TAG,
                            "decoded JPEG exceeds buffer");
    }

    io.outbuf = (uint8_t *)out_buffer;
    jpeg_ret = jpeg_dec_process(decoder, &io);
    jpeg_dec_close(decoder);
    ESP_RETURN_ON_FALSE(jpeg_ret == JPEG_ERR_OK, ESP_FAIL, TAG,
                        "JPEG decode failed: %d", jpeg_ret);

    *out_width = header.width;
    *out_height = header.height;
    return ESP_OK;
}

static void format_gallery_title(const photo_store_info_t *info, char *out,
                                 size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (!info || info->filename[0] == '\0') {
        out[0] = '\0';
        return;
    }

    const unsigned kb = (unsigned)((info->size_bytes + 511U) / 1024U);
    snprintf(out, out_size, "%s(%uKB)", info->filename, kb);
}

static esp_err_t load_gallery_photo(uint32_t index)
{
    if (photo_store_get_count() == 0) {
        memset(s_app.ppa_buffer, 0, s_app.buffer_size);
        camera_ui_set_gallery_image(s_app.ppa_buffer, PREVIEW_WIDTH, PREVIEW_HEIGHT);
        camera_ui_set_gallery_title("");
        return ESP_OK;
    }

    if (index >= photo_store_get_count()) {
        index = photo_store_get_count() - 1U;
    }
    s_app.gallery_index = index;

    photo_store_info_t info = {0};
    ESP_RETURN_ON_ERROR(
        photo_store_get_info(index, &info),
        TAG, "get gallery photo info failed");

    size_t jpeg_size = 0;
    ESP_RETURN_ON_ERROR(
        photo_store_load_jpeg(
            index, s_app.jpeg_load_buffer, MAX_JPEG_SIZE, &jpeg_size),
        TAG, "load gallery JPEG failed");

    int width = 0;
    int height = 0;
    ESP_RETURN_ON_ERROR(
        decode_jpeg_to_rgb565(
            s_app.jpeg_load_buffer, jpeg_size, s_app.ppa_buffer,
            s_app.buffer_size, &width, &height),
        TAG, "decode gallery JPEG failed");

    camera_ui_set_gallery_image(s_app.ppa_buffer, width, height);

    char title[40];
    format_gallery_title(&info, title, sizeof(title));
    camera_ui_set_gallery_title(title);
    return ESP_OK;
}

static esp_err_t refresh_latest_thumb(void)
{
    if (photo_store_get_count() == 0) {
        camera_ui_clear_thumb();
        return ESP_OK;
    }

    const uint32_t index = photo_store_get_count() - 1U;
    size_t jpeg_size = 0;
    ESP_RETURN_ON_ERROR(
        photo_store_load_jpeg(
            index, s_app.jpeg_load_buffer, MAX_JPEG_SIZE, &jpeg_size),
        TAG, "load latest photo for thumbnail failed");

    int width = 0;
    int height = 0;
    ESP_RETURN_ON_ERROR(
        decode_jpeg_to_rgb565(
            s_app.jpeg_load_buffer, jpeg_size, s_app.thumb_rgb_buffer,
            s_app.buffer_size, &width, &height),
        TAG, "decode thumbnail JPEG failed");

    camera_ui_set_thumb(s_app.thumb_rgb_buffer, width, height);
    return ESP_OK;
}

static void schedule_gallery_load(uint32_t index)
{
    s_gallery_load_index = index;
    s_gallery_load_pending = true;
}

static void schedule_thumb_refresh(void)
{
    s_thumb_refresh_pending = true;
}

static void service_gallery_pending(void)
{
    if (s_thumb_refresh_pending) {
        s_thumb_refresh_pending = false;
        const esp_err_t ret = refresh_latest_thumb();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Refresh thumbnail failed: %s", esp_err_to_name(ret));
        }
    }

    if (!s_gallery_load_pending) {
        return;
    }
    s_gallery_load_pending = false;
    const esp_err_t ret = load_gallery_photo(s_gallery_load_index);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Load gallery photo failed: %s", esp_err_to_name(ret));
    }
}

static void ui_capture_cb(void *user_data)
{
    (void)user_data;
    request_capture();
}

static void ui_flash_toggle_cb(bool enabled, void *user_data)
{
    (void)user_data;
    app_set_flash_enabled(enabled);
    ESP_LOGI(TAG, "Flash %s", enabled ? "enabled" : "disabled");
}

static void ui_preview_flip_cb(void *user_data)
{
    (void)user_data;
    app_set_preview_flip(!s_app.preview_flip);
    ESP_LOGI(TAG, "Preview flip %s", s_app.preview_flip ? "on" : "off");
}

static void ui_gallery_open_cb(void *user_data)
{
    (void)user_data;
    s_app.mode = APP_MODE_GALLERY;
    s_app.gallery_index =
        photo_store_get_count() > 0 ? photo_store_get_count() - 1U : 0;
    camera_ui_show_gallery();
    schedule_gallery_load(s_app.gallery_index);
    ESP_LOGI(TAG, "Enter gallery (%" PRIu32 " photos), index=%" PRIu32,
             photo_store_get_count(), s_app.gallery_index);
}

static void ui_gallery_close_cb(void *user_data)
{
    (void)user_data;
    s_app.mode = APP_MODE_CAMERA;
    camera_ui_show_camera();
    ESP_LOGI(TAG, "Return to camera preview");
}

static void ui_gallery_nav_cb(int direction, void *user_data)
{
    (void)user_data;
    if (photo_store_get_count() == 0) {
        return;
    }

    int32_t next = (int32_t)s_app.gallery_index + direction;
    if (next < 0) {
        next = 0;
    }
    if ((uint32_t)next >= photo_store_get_count()) {
        next = (int32_t)photo_store_get_count() - 1;
    }
    if ((uint32_t)next == s_app.gallery_index) {
        return;
    }

    s_app.gallery_index = (uint32_t)next;
    schedule_gallery_load(s_app.gallery_index);
    ESP_LOGI(TAG, "Gallery photo index=%" PRIu32, s_app.gallery_index);
}

static void ui_gallery_delete_cb(void *user_data)
{
    (void)user_data;
    if (photo_store_get_count() == 0) {
        return;
    }

    photo_store_info_t deleted = {0};
    if (photo_store_get_info(s_app.gallery_index, &deleted) != ESP_OK) {
        ESP_LOGW(TAG, "Get photo info before delete failed");
        return;
    }

    const esp_err_t ret = photo_store_delete(s_app.gallery_index);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Delete photo failed: %s", esp_err_to_name(ret));
        return;
    }

    const esp_err_t usb_remove_ret = photo_usb_msc_remove_jpeg(deleted.filename);
    if (usb_remove_ret != ESP_OK &&
        usb_remove_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Remove %s from USB storage failed: %s", deleted.filename,
                 esp_err_to_name(usb_remove_ret));
    }

    schedule_thumb_refresh();

    if (photo_store_get_count() == 0) {
        s_app.mode = APP_MODE_CAMERA;
        camera_ui_show_camera();
        ESP_LOGI(TAG, "All photos deleted, return to camera preview");
        return;
    }

    if (s_app.gallery_index >= photo_store_get_count()) {
        s_app.gallery_index = photo_store_get_count() - 1U;
    }

    schedule_gallery_load(s_app.gallery_index);
    ESP_LOGI(TAG, "Deleted photo, remaining=%" PRIu32 ", index=%" PRIu32,
             photo_store_get_count(), s_app.gallery_index);
}

static esp_err_t run_gallery_mode(void)
{
    while (s_app.mode == APP_MODE_GALLERY) {
        service_gallery_pending();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return ESP_OK;
}

static esp_err_t run_camera_mode(void)
{
    uint32_t consecutive_get_failures = 0;
    bool restart_attempted = false;

    ESP_LOGI(TAG, "Enter camera preview loop");
    log_app_state("enter camera mode");
    preview_diag_reset();

    while (s_app.mode == APP_MODE_CAMERA) {
        service_gallery_pending();
        preview_diag_maybe_heartbeat();

        if (!s_app.camera_power_on) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (s_capture_pending) {
            s_capture_pending = false;
            camera_ui_play_capture_flash();
            const esp_err_t capture_ret = perform_capture();
            if (capture_ret != ESP_OK) {
                ++s_preview_diag.capture_fail;
                ESP_LOGW(TAG, "Capture failed: %s (capture_fail=%" PRIu32 ")",
                         esp_err_to_name(capture_ret),
                         s_preview_diag.capture_fail);
                log_camera_pipeline("capture failed");
                log_app_state("capture failed");
            }
            continue;
        }

        const TickType_t frame_start = xTaskGetTickCount();
        mosaico_camera_frame_t frame = {0};
        esp_err_t ret = mosaico_camera_get_frame(s_app.camera, &frame);
        const uint32_t get_frame_ms = elapsed_ms_since(frame_start);
        if (ret != ESP_OK) {
            if (!s_app.camera_power_on) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            ++consecutive_get_failures;
            preview_diag_note_failure(ret, "get_frame");
            if (get_frame_ms >= PREVIEW_SLOW_GET_FRAME_MS) {
                ESP_LOGW(TAG, "Slow failed get_frame: %" PRIu32 "ms",
                         get_frame_ms);
            }
            if (consecutive_get_failures >= CAPTURE_FAILURE_LIMIT) {
                if (restart_attempted) {
                    ESP_LOGE(TAG,
                             "Preview loop exiting after repeated get_frame "
                             "failures");
                    log_camera_pipeline("preview loop exit");
                    return ret;
                }
                ESP_LOGW(TAG, "Attempting camera stream restart after %" PRIu32
                              " get_frame failures",
                         consecutive_get_failures);
                log_camera_pipeline("before stream restart");
                ESP_RETURN_ON_ERROR(
                    mosaico_camera_restart(s_app.camera),
                    TAG, "restart camera stream failed");
                ++s_preview_diag.stream_restarts;
                restart_attempted = true;
                consecutive_get_failures = 0;
                s_preview_diag.last_preview_ok_tick = xTaskGetTickCount();
                ESP_LOGI(TAG, "Camera stream restart complete (restarts=%" PRIu32
                              ")",
                         s_preview_diag.stream_restarts);
                log_camera_pipeline("after stream restart");
            }
            continue;
        }
        consecutive_get_failures = 0;
        restart_attempted = false;

        if (get_frame_ms >= PREVIEW_SLOW_GET_FRAME_MS) {
            ESP_LOGW(TAG, "Slow get_frame: %" PRIu32 "ms index=%" PRIu32,
                     get_frame_ms, frame.index);
        }

        if (s_app.mode != APP_MODE_CAMERA) {
            ESP_LOGI(TAG, "Leave camera preview loop for mode=%d",
                     (int)s_app.mode);
            const esp_err_t return_ret =
                mosaico_camera_return_frame(s_app.camera, &frame);
            if (return_ret != ESP_OK && s_app.camera_power_on) {
                preview_diag_note_failure(return_ret, "return_frame on leave");
                ESP_RETURN_ON_ERROR(return_ret, TAG, "return camera frame failed");
            }
            break;
        }

        const TickType_t convert_start = xTaskGetTickCount();
        ret = preview_convert_frame(&frame);
        const uint32_t convert_ms = elapsed_ms_since(convert_start);
        const esp_err_t return_ret =
            mosaico_camera_return_frame(s_app.camera, &frame);
        if (return_ret != ESP_OK && s_app.camera_power_on) {
            preview_diag_note_failure(return_ret, "return_frame");
            ESP_RETURN_ON_ERROR(return_ret, TAG, "return camera frame failed");
        }
        if (ret != ESP_OK && s_app.camera_power_on) {
            preview_diag_note_failure(ret, "preview_convert");
            ESP_RETURN_ON_ERROR(ret, TAG, "prepare preview frame failed");
        }

        ++s_preview_diag.preview_ok;
        s_preview_diag.last_preview_ok_tick = xTaskGetTickCount();
        if (convert_ms >= PREVIEW_SLOW_CONVERT_MS) {
            ESP_LOGW(TAG, "Slow preview_convert: %" PRIu32 "ms index=%" PRIu32,
                     convert_ms, frame.index);
        }

        if (s_app.camera_power_on) {
            camera_ui_invalidate_preview();
        }

        if (s_app.mode == APP_MODE_GALLERY) {
            ESP_LOGI(TAG, "Leave camera preview loop for gallery");
            break;
        }
    }
    ESP_LOGI(TAG,
             "Exit camera preview loop: ok=%" PRIu32 " fail=%" PRIu32
             " restarts=%" PRIu32,
             s_preview_diag.preview_ok, s_preview_diag.preview_fail,
             s_preview_diag.stream_restarts);
    log_app_state("exit camera mode");
    return ESP_OK;
}

static esp_err_t run_app_session(void)
{
    ESP_RETURN_ON_ERROR(camera_wait_and_open(), TAG, "open camera failed");
    s_app.camera_power_on = true;

    mosaico_camera_info_t info = {0};
    if (mosaico_camera_get_info(s_app.camera, &info) == ESP_OK) {
        ESP_LOGI(TAG,
                 "Camera session opened: %ux%u fmt=0x%08" PRIx32
                 " fps=%" PRIu32 " buffers=%u frame_size=%u",
                 info.width, info.height, info.pixel_format, info.frame_rate,
                 info.buffer_count, (unsigned)info.frame_buffer_size);
    }
    log_camera_pipeline("session opened");
    log_app_state("session opened");
    ESP_LOGI(TAG, "Camera photo app started");

    while (true) {
        if (s_app.mode == APP_MODE_CAMERA) {
            const esp_err_t ret = run_camera_mode();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Camera mode stopped: %s", esp_err_to_name(ret));
                log_camera_pipeline("camera mode stopped");
                return ret;
            }
        }
        if (s_app.mode == APP_MODE_GALLERY) {
            ESP_LOGI(TAG, "Enter gallery mode");
            ESP_RETURN_ON_ERROR(run_gallery_mode(), TAG, "gallery mode failed");
            ESP_LOGI(TAG, "Return from gallery mode to camera");
            log_app_state("return from gallery");
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG,
             "Camera photo app: Camera sub-board in LEFT slot, "
             "Button LED sub-board in RIGHT slot (optional)");
    ESP_ERROR_CHECK(app_init());
    ESP_ERROR_CHECK(camera_settings_init());
    ESP_ERROR_CHECK(photo_store_init());
    const esp_err_t usb_ret = photo_usb_msc_init();
    if (usb_ret != ESP_OK) {
        ESP_LOGW(TAG, "USB photo disk unavailable: %s", esp_err_to_name(usb_ret));
    }
    ESP_ERROR_CHECK(init_subboard_manager());
    ESP_ERROR_CHECK(init_top_button());
    ESP_ERROR_CHECK(start_button_subboard_task());

    s_app.mode = APP_MODE_CAMERA;
    s_app.flash_enabled = false;
    s_app.camera_power_on = true;
    s_app.gallery_index = 0;
    const esp_err_t flip_load_ret =
        camera_settings_load_preview_flip(&s_app.preview_flip);
    if (flip_load_ret != ESP_OK) {
        ESP_LOGW(TAG, "Load preview flip setting failed: %s, use default",
                 esp_err_to_name(flip_load_ret));
        s_app.preview_flip = false;
    }

    const camera_ui_callbacks_t ui_callbacks = {
        .on_capture = ui_capture_cb,
        .on_flash_toggle = ui_flash_toggle_cb,
        .on_preview_flip = ui_preview_flip_cb,
        .on_gallery_open = ui_gallery_open_cb,
        .on_gallery_close = ui_gallery_close_cb,
        .on_gallery_nav = ui_gallery_nav_cb,
        .on_gallery_delete = ui_gallery_delete_cb,
        .user_data = NULL,
    };
    ESP_ERROR_CHECK(camera_ui_create(
        bsp_display_get(), s_app.ppa_buffer, PREVIEW_WIDTH, PREVIEW_HEIGHT,
        &ui_callbacks));
    camera_ui_set_preview_flip(s_app.preview_flip);
    ESP_ERROR_CHECK(refresh_latest_thumb());

    while (true) {
        const esp_err_t ret = run_app_session();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "App session stopped: %s; waiting for camera reconnect",
                     esp_err_to_name(ret));
            log_app_state("app session stopped");
            if (s_app.camera) {
                log_camera_pipeline("before camera delete");
                ESP_ERROR_CHECK(mosaico_camera_del(s_app.camera));
                s_app.camera = NULL;
            }
            s_app.mode = APP_MODE_CAMERA;
            camera_ui_show_camera();
            vTaskDelay(pdMS_TO_TICKS(CAMERA_RETRY_DELAY_MS));
        }
    }
}
