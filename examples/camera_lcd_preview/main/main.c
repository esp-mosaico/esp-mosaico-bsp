/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp/display.h"
#include "driver/ppa.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "mosaico_camera.h"

#define PREVIEW_WIDTH              BSP_LCD_H_RES
#define PREVIEW_HEIGHT             BSP_LCD_V_RES
#define PREVIEW_CROP_WIDTH         640
#define PREVIEW_CROP_HEIGHT        640
#define PREVIEW_SCALE              0.75f
#define PREVIEW_BUFFER_ALIGNMENT   128
#define CAPTURE_FAILURE_LIMIT      3
#define LCD_TRANSFER_WARN_MS       100
#define STATS_FRAME_INTERVAL       120
#define CAMERA_RETRY_DELAY_MS      500

static const char *TAG = "camera_lcd";

typedef struct {
    mosaico_camera_handle_t camera;
    ppa_client_handle_t ppa;
    esp_lcd_panel_handle_t panel;
    SemaphoreHandle_t lcd_transfer_done;
    uint16_t *ppa_buffer;
    uint16_t *lcd_buffer;
    size_t buffer_size;
} preview_context_t;

static preview_context_t s_preview;

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static uint32_t align_down_even(uint32_t value)
{
    return value & ~1U;
}

static uint16_t rgb565_to_lcd_wire(uint16_t pixel)
{
    /* PPA stays in RGB565; only the LCD wire byte order is exchanged. */
    return __builtin_bswap16(pixel);
}

static bool IRAM_ATTR lcd_color_transfer_done(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_ctx)
{
    (void)panel_io;
    (void)event_data;

    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &high_task_woken);
    return high_task_woken == pdTRUE;
}

static esp_err_t preview_alloc_buffers(void)
{
    s_preview.buffer_size = align_up(
        PREVIEW_WIDTH * PREVIEW_HEIGHT * sizeof(uint16_t),
        PREVIEW_BUFFER_ALIGNMENT);
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;

    s_preview.ppa_buffer = heap_caps_aligned_calloc(
        PREVIEW_BUFFER_ALIGNMENT, 1, s_preview.buffer_size, caps);
    s_preview.lcd_buffer = heap_caps_aligned_calloc(
        PREVIEW_BUFFER_ALIGNMENT, 1, s_preview.buffer_size, caps);
    ESP_RETURN_ON_FALSE(
        s_preview.ppa_buffer && s_preview.lcd_buffer,
        ESP_ERR_NO_MEM, TAG, "allocate preview buffers failed");

    s_preview.lcd_transfer_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(
        s_preview.lcd_transfer_done,
        ESP_ERR_NO_MEM, TAG, "create LCD transfer semaphore failed");
    return ESP_OK;
}

static esp_err_t preview_init(void)
{
    bsp_display_config_t display_config = BSP_DISPLAY_DEFAULT_CONFIG();
    display_config.enable_touch = false;
    ESP_RETURN_ON_ERROR(
        bsp_display_new(&display_config, &s_preview.panel),
        TAG, "initialize display failed");

    const esp_lcd_panel_io_handle_t panel_io = bsp_display_get_panel_io();
    ESP_RETURN_ON_FALSE(panel_io, ESP_ERR_INVALID_STATE, TAG,
                        "LCD panel IO is unavailable");

    ESP_RETURN_ON_ERROR(
        preview_alloc_buffers(), TAG, "allocate preview resources failed");

    const esp_lcd_panel_io_callbacks_t lcd_callbacks = {
        .on_color_trans_done = lcd_color_transfer_done,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_register_event_callbacks(panel_io, &lcd_callbacks,
                                                  s_preview.lcd_transfer_done),
        TAG, "register LCD transfer callback failed");

    const ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ESP_RETURN_ON_ERROR(
        ppa_register_client(&ppa_config, &s_preview.ppa),
        TAG, "register PPA client failed");

    return ESP_OK;
}

static esp_err_t camera_wait_and_open(void)
{
    mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    /* Bring-up boards ship with an unprogrammed subboard EEPROM. */
    config.allow_unidentified = true;

    while (true) {
        const esp_err_t ret = mosaico_camera_new(&config, &s_preview.camera);
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
        "camera frame %" PRIu32 "x%" PRIu32
        " is smaller than the %dx%d crop",
        frame->width, frame->height, PREVIEW_CROP_WIDTH, PREVIEW_CROP_HEIGHT);

    const uint32_t bytes_per_line =
        frame->bytes_per_line ? frame->bytes_per_line : frame->width * 2U;
    ESP_RETURN_ON_FALSE(
        (bytes_per_line % 2U) == 0,
        ESP_ERR_INVALID_SIZE, TAG, "camera stride is not pixel aligned");

    ESP_RETURN_ON_ERROR(
        esp_cache_msync(
            (void *)frame->data, frame->size,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C |
            ESP_CACHE_MSYNC_FLAG_INVALIDATE),
        TAG, "invalidate camera frame cache failed");
    ESP_RETURN_ON_ERROR(
        esp_cache_msync(
            s_preview.ppa_buffer, s_preview.buffer_size,
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
            .buffer = s_preview.ppa_buffer,
            .buffer_size = s_preview.buffer_size,
            .pic_w = PREVIEW_WIDTH,
            .pic_h = PREVIEW_HEIGHT,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
        .scale_x = PREVIEW_SCALE,
        .scale_y = PREVIEW_SCALE,
        .mirror_y = true,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ESP_RETURN_ON_ERROR(
        ppa_do_scale_rotate_mirror(s_preview.ppa, &operation),
        TAG, "convert camera frame with PPA failed");
    ESP_RETURN_ON_ERROR(
        esp_cache_msync(
            s_preview.ppa_buffer, s_preview.buffer_size,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C |
            ESP_CACHE_MSYNC_FLAG_INVALIDATE),
        TAG, "invalidate PPA output cache failed");

    const size_t pixel_count = PREVIEW_WIDTH * PREVIEW_HEIGHT;
    for (size_t i = 0; i < pixel_count; ++i) {
        s_preview.lcd_buffer[i] =
            rgb565_to_lcd_wire(s_preview.ppa_buffer[i]);
    }
    return ESP_OK;
}

static esp_err_t preview_draw(void)
{
    ESP_RETURN_ON_ERROR(
        esp_cache_msync(
            s_preview.lcd_buffer, s_preview.buffer_size,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M |
            ESP_CACHE_MSYNC_FLAG_UNALIGNED),
        TAG, "clean LCD frame cache failed");

    (void)xSemaphoreTake(s_preview.lcd_transfer_done, 0);
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_draw_bitmap(
            s_preview.panel, 0, 0, PREVIEW_WIDTH, PREVIEW_HEIGHT,
            s_preview.lcd_buffer),
        TAG, "submit LCD frame failed");

    const TickType_t warning_period = pdMS_TO_TICKS(LCD_TRANSFER_WARN_MS);
    while (xSemaphoreTake(s_preview.lcd_transfer_done, warning_period) != pdTRUE) {
        ESP_LOGW(TAG, "LCD transfer exceeds %d ms", LCD_TRANSFER_WARN_MS);
    }
    return ESP_OK;
}

static esp_err_t preview_run(void)
{
    uint32_t frame_count = 0;
    uint32_t capture_failures = 0;
    bool restart_attempted = false;
    TickType_t log_start = xTaskGetTickCount();

    while (true) {
        mosaico_camera_frame_t frame = {0};
        esp_err_t ret = mosaico_camera_get_frame(s_preview.camera, &frame);
        if (ret != ESP_OK) {
            capture_failures++;
            ESP_LOGW(
                TAG, "capture failed (%" PRIu32 "/%d): %s",
                capture_failures, CAPTURE_FAILURE_LIMIT,
                esp_err_to_name(ret));
            if (capture_failures >= CAPTURE_FAILURE_LIMIT) {
                if (restart_attempted) {
                    return ret;
                }
                ESP_RETURN_ON_ERROR(
                    mosaico_camera_restart(s_preview.camera),
                    TAG, "restart camera stream failed");
                ESP_LOGI(TAG, "Camera stream restarted");
                restart_attempted = true;
                capture_failures = 0;
            }
            continue;
        }
        capture_failures = 0;
        restart_attempted = false;

        ret = preview_convert_frame(&frame);
        const esp_err_t return_ret = mosaico_camera_return_frame(s_preview.camera, &frame);
        ESP_RETURN_ON_ERROR(
            return_ret, TAG, "return camera frame failed");
        ESP_RETURN_ON_ERROR(
            ret, TAG, "prepare preview frame failed");
        ESP_RETURN_ON_ERROR(
            preview_draw(), TAG, "draw preview frame failed");

        frame_count++;
        if (frame_count == 1) {
            ESP_LOGI(TAG, "First camera frame displayed");
        } else if ((frame_count % STATS_FRAME_INTERVAL) == 0) {
            const TickType_t now = xTaskGetTickCount();
            const uint32_t elapsed_ms =
                (uint32_t)((now - log_start) * portTICK_PERIOD_MS);
            const float fps = elapsed_ms > 0
                                  ? (float)STATS_FRAME_INTERVAL * 1000.0f / (float)elapsed_ms
                                  : 0.0f;
            ESP_LOGI(
                TAG, "Preview frames=%" PRIu32 " rate=%.1f fps",
                frame_count, fps);
            log_start = now;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(
        TAG,
        "Camera LCD preview: insert Camera subboard in the LEFT slot");
    ESP_ERROR_CHECK(preview_init());

    while (true) {
        ESP_ERROR_CHECK(camera_wait_and_open());
        ESP_LOGI(
            TAG,
            "Preview started: OV3640 UYVY center crop -> PPA RGB565 -> CO5300");

        const esp_err_t ret = preview_run();
        ESP_LOGW(
            TAG, "Preview stopped: %s; waiting for camera reconnect",
            esp_err_to_name(ret));
        ESP_ERROR_CHECK(mosaico_camera_del(s_preview.camera));
        s_preview.camera = NULL;
        vTaskDelay(pdMS_TO_TICKS(CAMERA_RETRY_DELAY_MS));
    }
}
