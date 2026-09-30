/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "preview_geometry.h"

#include "bsp/display.h"
#include "driver/ppa.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "mosaico_module_camera.h"

#define PREVIEW_WIDTH              BSP_LCD_H_RES
#define PREVIEW_HEIGHT             BSP_LCD_V_RES
#define PREVIEW_BUFFER_ALIGNMENT   128
#define CAPTURE_FAILURE_LIMIT      3
#define DMA_TRANSFER_WARN_MS       1000
#define SHUTDOWN_TIMEOUT_MS        15000
#define STATS_FRAME_INTERVAL       120
#define CAMERA_RETRY_DELAY_MS      500

static const char *TAG = "camera_lcd";

typedef struct {
    mosaico_camera_handle_t camera;
    ppa_client_handle_t ppa;
    esp_lcd_panel_handle_t panel;
    SemaphoreHandle_t lcd_transfer_done;
    SemaphoreHandle_t ppa_transfer_done;
    SemaphoreHandle_t shutdown_done;
    atomic_bool shutdown_requested;
    esp_err_t shutdown_result;
    uint16_t *ppa_buffer;
    uint16_t *lcd_buffer;
    size_t buffer_size;
} preview_context_t;

/* app_main owns capture/PPA/LCD; BSP owns the panel/IO singleton. The
 * process-wide shutdown hook only requests stop and waits for the owner.
 * Publish the context after initialization; retain it until reboot. */
static _Atomic(preview_context_t *) shutdown_context;

ESP_SHUTDOWN_HANDLER_REGISTER(preview_shutdown, 100)
{
    (void)user_arg;
    (void)ctx;
    preview_context_t *preview = atomic_load(&shutdown_context);
    if (!preview) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Shutdown: waiting for camera/PPA/LCD to stop");
    atomic_store(&preview->shutdown_requested, true);
    if (xSemaphoreTake(preview->shutdown_done, pdMS_TO_TICKS(SHUTDOWN_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Shutdown: DMA did not drain within %d ms", SHUTDOWN_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "Shutdown: preview stopped (%s)", esp_err_to_name(preview->shutdown_result));
    return preview->shutdown_result;
}

static bool IRAM_ATTR ppa_transfer_done(ppa_client_handle_t client,
                                      ppa_event_data_t *event, void *user_ctx)
{
    (void)client;
    (void)event;
    preview_context_t *ctx = user_ctx;
    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->ppa_transfer_done, &high_task_woken);
    return high_task_woken == pdTRUE;
}

static void preview_wait_for_dma(SemaphoreHandle_t done, const char *stage)
{
    /* A timeout must not recycle the camera frame or DMA destination: hardware
     * may still own them. Keep waiting, with a diagnostic instead of a silent
     * portMAX_DELAY inside the PPA driver. Shutdown also has a bounded wait. */
    while (xSemaphoreTake(done, pdMS_TO_TICKS(DMA_TRANSFER_WARN_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "%s DMA completion exceeds %d ms", stage, DMA_TRANSFER_WARN_MS);
    }
}

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

static esp_err_t preview_alloc_buffers(preview_context_t *ctx)
{
    ctx->buffer_size = align_up(
        PREVIEW_WIDTH * PREVIEW_HEIGHT * sizeof(uint16_t),
        PREVIEW_BUFFER_ALIGNMENT);
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;

    ctx->ppa_buffer = heap_caps_aligned_calloc(
        PREVIEW_BUFFER_ALIGNMENT, 1, ctx->buffer_size, caps);
    ctx->lcd_buffer = heap_caps_aligned_calloc(
        PREVIEW_BUFFER_ALIGNMENT, 1, ctx->buffer_size, caps);
    ESP_RETURN_ON_FALSE(
        ctx->ppa_buffer && ctx->lcd_buffer,
        ESP_ERR_NO_MEM, TAG, "allocate preview buffers failed");

    ctx->lcd_transfer_done = xSemaphoreCreateBinary();
    ctx->ppa_transfer_done = xSemaphoreCreateBinary();
    ctx->shutdown_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(
        ctx->lcd_transfer_done && ctx->ppa_transfer_done && ctx->shutdown_done,
        ESP_ERR_NO_MEM, TAG, "create preview semaphores failed");
    return ESP_OK;
}

static esp_err_t preview_init(preview_context_t *ctx)
{
    bsp_display_config_t display_config = BSP_DISPLAY_DEFAULT_CONFIG();
    display_config.enable_touch = false;
    ESP_RETURN_ON_ERROR(
        bsp_display_new(&display_config, &ctx->panel),
        TAG, "initialize display failed");

    const esp_lcd_panel_io_handle_t panel_io = bsp_display_get_panel_io();
    ESP_RETURN_ON_FALSE(panel_io, ESP_ERR_INVALID_STATE, TAG,
                        "LCD panel IO is unavailable");

    ESP_RETURN_ON_ERROR(
        preview_alloc_buffers(ctx), TAG, "allocate preview resources failed");

    const ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ESP_RETURN_ON_ERROR(
        ppa_register_client(&ppa_config, &ctx->ppa),
        TAG, "register PPA client failed");

    const ppa_event_callbacks_t ppa_callbacks = {
        .on_trans_done = ppa_transfer_done,
    };
    ESP_RETURN_ON_ERROR(ppa_client_register_event_callbacks(ctx->ppa, &ppa_callbacks),
                        TAG, "register PPA completion callback failed");

    const esp_lcd_panel_io_callbacks_t lcd_callbacks = {
        .on_color_trans_done = lcd_color_transfer_done,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_register_event_callbacks(panel_io, &lcd_callbacks,
                                                  ctx->lcd_transfer_done),
        TAG, "register LCD transfer callback failed");

    return ESP_OK;
}

static esp_err_t camera_wait_and_open(preview_context_t *ctx)
{
    mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    /* Bring-up boards ship with an unprogrammed subboard EEPROM. */
    config.allow_unidentified = true;

    while (!atomic_load(&ctx->shutdown_requested)) {
        esp_err_t ret = mosaico_camera_new(&config, &ctx->camera);
        if (ret == ESP_OK) {
            ret = mosaico_camera_open(ctx->camera);
        }
        if (ret == ESP_OK) {
            ret = mosaico_camera_start_stream(ctx->camera);
        }
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        if (ctx->camera) {
            const esp_err_t cleanup_ret = mosaico_camera_del(ctx->camera);
            if (cleanup_ret == ESP_OK) {
                ctx->camera = NULL;
            } else {
                return cleanup_ret;
            }
        }
        ESP_LOGW(TAG, "Opening the camera in the LEFT slot failed, retrying: %s",
                 esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(CAMERA_RETRY_DELAY_MS));
    }
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t preview_convert_frame(preview_context_t *ctx, const mosaico_camera_frame_t *frame)
{
    ESP_RETURN_ON_FALSE(
        frame && frame->data, ESP_ERR_INVALID_ARG, TAG, "invalid camera frame");
    ESP_RETURN_ON_FALSE(
        frame->pixel_format == V4L2_PIX_FMT_UYVY,
        ESP_ERR_NOT_SUPPORTED, TAG,
        "unsupported camera format 0x%08" PRIx32, frame->pixel_format);
    const uint32_t crop_size = preview_crop_size(frame->width, frame->height, PREVIEW_WIDTH);
    ESP_RETURN_ON_FALSE(crop_size > 0, ESP_ERR_INVALID_SIZE, TAG, "invalid camera frame size: %" PRIu32 "x%" PRIu32,
                        frame->width, frame->height);
    const uint32_t bytes_per_line = frame->bytes_per_line ? frame->bytes_per_line : frame->width * 2U;
    ESP_RETURN_ON_FALSE((bytes_per_line % 2U) == 0, ESP_ERR_INVALID_SIZE, TAG, "camera stride is not pixel aligned");

    ESP_RETURN_ON_ERROR(
        esp_cache_msync(
            (void *)frame->data, frame->size,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C |
            ESP_CACHE_MSYNC_FLAG_INVALIDATE),
        TAG, "invalidate camera frame cache failed");
    ESP_RETURN_ON_ERROR(
        esp_cache_msync(
            ctx->ppa_buffer, ctx->buffer_size,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M),
        TAG, "clean PPA output cache failed");

    /* About 200 bytes on app_main's explicitly configured 20 KiB stack. */
    const ppa_srm_oper_config_t operation = {
        .in = {
            .buffer = frame->data,
            .pic_w = bytes_per_line / 2U,
            .pic_h = frame->height,
            .block_w = crop_size,
            .block_h = crop_size,
            .block_offset_x = align_down_even((frame->width - crop_size) / 2U),
            .block_offset_y = align_down_even((frame->height - crop_size) / 2U),
            .srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = ctx->ppa_buffer,
            .buffer_size = ctx->buffer_size,
            .pic_w = PREVIEW_WIDTH,
            .pic_h = PREVIEW_HEIGHT,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
        .scale_x = (float)PREVIEW_WIDTH / (float)crop_size,
        .scale_y = (float)PREVIEW_HEIGHT / (float)crop_size,
        .mirror_y = true,
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
        .user_data = ctx,
    };
    ESP_RETURN_ON_ERROR(
        ppa_do_scale_rotate_mirror(ctx->ppa, &operation),
        TAG, "convert camera frame with PPA failed");
    preview_wait_for_dma(ctx->ppa_transfer_done, "PPA");
    ESP_RETURN_ON_ERROR(
        esp_cache_msync(
            ctx->ppa_buffer, ctx->buffer_size,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C |
            ESP_CACHE_MSYNC_FLAG_INVALIDATE),
        TAG, "invalidate PPA output cache failed");

    const size_t pixel_count = PREVIEW_WIDTH * PREVIEW_HEIGHT;
    for (size_t i = 0; i < pixel_count; ++i) {
        ctx->lcd_buffer[i] =
            rgb565_to_lcd_wire(ctx->ppa_buffer[i]);
    }
    return ESP_OK;
}

static esp_err_t preview_draw(preview_context_t *ctx)
{
    ESP_RETURN_ON_ERROR(
        esp_cache_msync(
            ctx->lcd_buffer, ctx->buffer_size,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M |
            ESP_CACHE_MSYNC_FLAG_UNALIGNED),
        TAG, "clean LCD frame cache failed");

    (void)xSemaphoreTake(ctx->lcd_transfer_done, 0);
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_draw_bitmap(
            ctx->panel, 0, 0, PREVIEW_WIDTH, PREVIEW_HEIGHT,
            ctx->lcd_buffer),
        TAG, "submit LCD frame failed");

    preview_wait_for_dma(ctx->lcd_transfer_done, "LCD");
    return ESP_OK;
}

static esp_err_t preview_run(preview_context_t *ctx)
{
    uint32_t frame_count = 0;
    uint32_t dropped_frame_count = 0;
    uint32_t capture_failures = 0;
    bool restart_attempted = false;
    TickType_t log_start = xTaskGetTickCount();

    while (!atomic_load(&ctx->shutdown_requested)) {
        mosaico_camera_frame_t frame = {0};
        esp_err_t ret = mosaico_camera_get_frame(ctx->camera, &frame);
        if (ret == ESP_ERR_INVALID_SIZE) {
            dropped_frame_count++;
            capture_failures = 0;
            restart_attempted = false;
            continue;
        }
        if (ret != ESP_OK) {
            if (atomic_load(&ctx->shutdown_requested)) {
                break;
            }
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
                    mosaico_camera_restart(ctx->camera),
                    TAG, "restart camera stream failed");
                ESP_LOGI(TAG, "Camera stream restarted");
                restart_attempted = true;
                capture_failures = 0;
            }
            continue;
        }
        capture_failures = 0;
        restart_attempted = false;

        const size_t expected_size = (size_t)frame.width * frame.height * 2U;
        if (frame.size < expected_size) {
            ESP_LOGW(TAG, "Drop invalid camera frame: received=%zu expected=%zu", frame.size, expected_size);
            ESP_RETURN_ON_ERROR(mosaico_camera_return_frame(ctx->camera, &frame), TAG, "return invalid camera frame failed");
            dropped_frame_count++;
            continue;
        }

        if (frame_count == 0) {
            ESP_LOGI(TAG, "First frame captured; starting PPA conversion");
        }
        ret = preview_convert_frame(ctx, &frame);
        const esp_err_t return_ret = mosaico_camera_return_frame(ctx->camera, &frame);
        ESP_RETURN_ON_ERROR(
            return_ret, TAG, "return camera frame failed");
        if (ret == ESP_ERR_INVALID_SIZE) {
            dropped_frame_count++;
            continue;
        }
        ESP_RETURN_ON_ERROR(
            ret, TAG, "prepare preview frame failed");
        if (frame_count == 0) {
            ESP_LOGI(TAG, "First frame converted; starting LCD transfer");
        }
        ESP_RETURN_ON_ERROR(
            preview_draw(ctx), TAG, "draw preview frame failed");

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
                TAG, "Preview frames=%" PRIu32 " dropped=%" PRIu32 " rate=%.1f fps",
                frame_count, dropped_frame_count, fps);
            log_start = now;
        }
    }
    return ESP_OK;
}

/* Initialization rollback only, before publishing callbacks to shutdown. */
static void preview_free(preview_context_t *ctx)
{
    if (ctx->ppa) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ppa_unregister_client(ctx->ppa));
    }
    if (ctx->lcd_transfer_done) {
        vSemaphoreDelete(ctx->lcd_transfer_done);
    }
    if (ctx->ppa_transfer_done) {
        vSemaphoreDelete(ctx->ppa_transfer_done);
    }
    if (ctx->shutdown_done) {
        vSemaphoreDelete(ctx->shutdown_done);
    }
    free(ctx->ppa_buffer);
    free(ctx->lcd_buffer);
    free(ctx);
}

void app_main(void)
{
    preview_context_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        ESP_LOGE(TAG, "No memory for preview");
        return;
    }
    atomic_init(&ctx->shutdown_requested, false);
    esp_err_t ret = preview_init(ctx);
    if (ret != ESP_OK) {
        preview_free(ctx);
        ESP_LOGE(TAG, "Preview initialization failed: %s",
                 esp_err_to_name(ret));
        return;
    }
    atomic_store(&shutdown_context, ctx);
    ESP_LOGI(TAG, "Camera LCD preview: LEFT slot, 480x480 center crop");
    while (!atomic_load(&ctx->shutdown_requested)) {
        ret = camera_wait_and_open(ctx);
        if (ret != ESP_OK) {
            if (!atomic_load(&ctx->shutdown_requested)) {
                ESP_LOGE(TAG, "Camera cleanup failed: %s", esp_err_to_name(ret));
            }
            break;
        }
        ESP_LOGI(TAG, "Preview started: Camera UYVY -> PPA RGB565 -> CO5300");
        ret = preview_run(ctx);
        if (atomic_load(&ctx->shutdown_requested)) {
            break;
        }
        ESP_LOGW(TAG, "Preview stopped: %s; waiting for camera reconnect", esp_err_to_name(ret));
        ret = mosaico_camera_del(ctx->camera);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Camera cleanup failed: %s", esp_err_to_name(ret));
            break;
        }
        ctx->camera = NULL;
        vTaskDelay(pdMS_TO_TICKS(CAMERA_RETRY_DELAY_MS));
    }
    /* Only the capture owner closes the stream. At this point any borrowed
     * frame has been returned and PPA/LCD completion has been observed. Stop
     * continuous camera DMA before releasing PPA and acknowledging shutdown. */
    ctx->shutdown_result = ESP_OK;
    if (ctx->camera) {
        ctx->shutdown_result = mosaico_camera_del(ctx->camera);
        if (ctx->shutdown_result == ESP_OK) {
            ctx->camera = NULL;
        }
    }
    if (ctx->ppa) {
        esp_err_t ppa_ret = ppa_unregister_client(ctx->ppa);
        if (ppa_ret == ESP_OK) {
            ctx->ppa = NULL;
        } else if (ctx->shutdown_result == ESP_OK) {
            ctx->shutdown_result = ppa_ret;
        }
    }
    xSemaphoreGive(ctx->shutdown_done);
}
