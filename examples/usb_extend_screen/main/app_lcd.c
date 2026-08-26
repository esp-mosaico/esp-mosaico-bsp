/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "app_lcd.h"
#include "bsp/display.h"
#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if !CONFIG_SOC_JPEG_DECODE_SUPPORTED
#error "usb_extend_screen requires hardware JPEG decode support"
#endif

#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
#include "driver/jpeg_decode.h"
#else
#include "esp_jpeg_dec.h"
#endif

static const char *TAG = "app_lcd";

#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
#define LCD_DECODE_BUFFER_COUNT  CONFIG_EXAMPLE_LCD_BUF_COUNT
#define LCD_JPEG_MAX_MCU_ALIGNMENT  16U
#define LCD_ALIGN_UP(value, alignment)  (((value) + (alignment) - 1) & ~((alignment) - 1))
#else
#define LCD_DECODE_BUFFER_COUNT  1
#endif

static esp_lcd_panel_handle_t s_panel;
static void *s_decode_buffer;
static size_t s_decode_buffer_size;
static uint16_t *s_lcd_buffers[LCD_DECODE_BUFFER_COUNT];
static size_t s_lcd_buffer_len;
static uint16_t s_usb_width;
static uint16_t s_usb_height;
static uint16_t s_panel_width;
static uint16_t s_panel_height;
static uint8_t s_buffer_index;
static SemaphoreHandle_t s_available_decode_buffers;
static ppa_client_handle_t s_ppa;

static bool app_lcd_color_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                        esp_lcd_panel_io_event_data_t *edata,
                                        void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(s_available_decode_buffers, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
static jpeg_decoder_handle_t s_jpeg_decoder;

static jpeg_decode_cfg_t s_decode_config = {
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
    .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
};
#endif

static esp_err_t app_lcd_allocate_decode_buffers(void)
{
    size_t output_width = s_usb_width;
    size_t output_height = s_usb_height;
#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
    output_width = LCD_ALIGN_UP(output_width, LCD_JPEG_MAX_MCU_ALIGNMENT);
    output_height = LCD_ALIGN_UP(output_height, LCD_JPEG_MAX_MCU_ALIGNMENT);
    ESP_LOGI(TAG, "JPEG buffer: visible=%ux%u, MCU-aligned capacity=%ux%u",
             s_usb_width, s_usb_height, (unsigned)output_width, (unsigned)output_height);
#endif
    size_t decode_buffer_len = output_width * output_height * EXAMPLE_LCD_BIT_PER_PIXEL / 8;
#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
    jpeg_decode_memory_alloc_cfg_t output_memory_config = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    s_decode_buffer = jpeg_alloc_decoder_mem(decode_buffer_len, &output_memory_config,
                                             &s_decode_buffer_size);
#endif
    ESP_RETURN_ON_FALSE(s_decode_buffer, ESP_ERR_NO_MEM, TAG,
                        "allocate JPEG decode buffer failed");

    s_lcd_buffer_len = s_panel_width * s_panel_height * sizeof(uint16_t);
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    for (size_t i = 0; i < LCD_DECODE_BUFFER_COUNT; i++) {
        s_lcd_buffers[i] = heap_caps_aligned_calloc(64, 1, s_lcd_buffer_len, caps);
        ESP_RETURN_ON_FALSE(s_lcd_buffers[i], ESP_ERR_NO_MEM, TAG,
                            "allocate LCD buffer %u failed", (unsigned)i);
    }
    return ESP_OK;
}

#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
static esp_err_t app_lcd_get_jpeg_output_layout(const uint8_t *input, size_t input_len,
                                                uint16_t width, uint16_t height,
                                                uint32_t *output_width, uint32_t *output_height)
{
    jpeg_decode_picture_info_t picture_info = {0};
    ESP_RETURN_ON_ERROR(jpeg_decoder_get_info(input, input_len, &picture_info),
                        TAG, "parse JPEG header failed");
    ESP_RETURN_ON_FALSE(picture_info.width == width && picture_info.height == height,
                        ESP_ERR_INVALID_SIZE, TAG,
                        "JPEG size is %ux%u, expected %ux%u",
                        (unsigned)picture_info.width, (unsigned)picture_info.height, width, height);

    uint32_t horizontal_alignment;
    uint32_t vertical_alignment;
    switch (picture_info.sample_method) {
    case JPEG_DOWN_SAMPLING_YUV444:
        horizontal_alignment = 8;
        vertical_alignment = 8;
        break;
    case JPEG_DOWN_SAMPLING_YUV422:
        horizontal_alignment = 16;
        vertical_alignment = 8;
        break;
    case JPEG_DOWN_SAMPLING_YUV420:
        horizontal_alignment = 16;
        vertical_alignment = 16;
        break;
    case JPEG_DOWN_SAMPLING_GRAY:
        return ESP_ERR_NOT_SUPPORTED;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    *output_width = LCD_ALIGN_UP(picture_info.width, horizontal_alignment);
    *output_height = LCD_ALIGN_UP(picture_info.height, vertical_alignment);
    return ESP_OK;
}
#endif

void app_lcd_draw(uint8_t *buf, uint32_t len, uint16_t width, uint16_t height)
{
    if (width != s_usb_width || height != s_usb_height) {
        ESP_LOGW(TAG, "Drop JPEG with unexpected size: %ux%u, expected %ux%u",
                 width, height, s_usb_width, s_usb_height);
        return;
    }

    static int fps_count;
    static int64_t start_time;
    if (start_time == 0) {
        start_time = esp_timer_get_time();
    }
    if (++fps_count == 50) {
        int64_t end_time = esp_timer_get_time();
        ESP_LOGI(TAG, "fps: %f", 1000000.0 / ((end_time - start_time) / 50.0));
        start_time = end_time;
        fps_count = 0;
    }

#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
    uint32_t output_width;
    uint32_t output_height;
    esp_err_t layout_ret = app_lcd_get_jpeg_output_layout(buf, len, width, height,
                                                          &output_width, &output_height);
    if (layout_ret != ESP_OK) {
        ESP_LOGD(TAG, "Unsupported JPEG layout: %s", esp_err_to_name(layout_ret));
        return;
    }

    size_t expected_output_size = output_width * output_height * EXAMPLE_LCD_BIT_PER_PIXEL / 8;
    if (expected_output_size > s_decode_buffer_size) {
        ESP_LOGW(TAG, "JPEG output too large: %u > %u", (unsigned)expected_output_size,
                 (unsigned)s_decode_buffer_size);
        return;
    }

    uint32_t output_size = 0;
    esp_err_t decode_ret = jpeg_decoder_process(s_jpeg_decoder, &s_decode_config, buf, len,
                                                s_decode_buffer, s_decode_buffer_size, &output_size);
    if (decode_ret != ESP_OK) {
        ESP_LOGD(TAG, "JPEG decode failed: %s", esp_err_to_name(decode_ret));
        return;
    }
    if (output_size != expected_output_size) {
        ESP_LOGW(TAG, "Unexpected JPEG output size: %u, expected %u",
                 (unsigned)output_size, (unsigned)expected_output_size);
        return;
    }

    if (output_width != width) {
        uint8_t *output = s_decode_buffer;
        size_t visible_row_size = width * EXAMPLE_LCD_BIT_PER_PIXEL / 8;
        size_t decoded_row_size = output_width * EXAMPLE_LCD_BIT_PER_PIXEL / 8;
        for (size_t row = 1; row < height; row++) {
            memmove(output + row * visible_row_size, output + row * decoded_row_size,
                    visible_row_size);
        }
    }

    /*
     * The JPEG decoder emits LCD wire order. PPA byte-swaps that input back to
     * native RGB565 while scaling; the result is swapped once more below.
     */
#endif

    if (xSemaphoreTake(s_available_decode_buffers, portMAX_DELAY) != pdTRUE) {
        return;
    }

    if (width == s_panel_width && height == s_panel_height) {
        esp_err_t ret = esp_cache_msync(s_decode_buffer, expected_output_size,
                                        ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                                        ESP_CACHE_MSYNC_FLAG_INVALIDATE);
        if (ret == ESP_OK) {
            memcpy(s_lcd_buffers[s_buffer_index], s_decode_buffer, s_lcd_buffer_len);
            ret = esp_cache_msync(s_lcd_buffers[s_buffer_index], s_lcd_buffer_len,
                                  ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                  ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
        if (ret == ESP_OK) {
            ret = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, s_panel_width, s_panel_height,
                                            s_lcd_buffers[s_buffer_index]);
        }
        if (ret != ESP_OK) {
            xSemaphoreGive(s_available_decode_buffers);
            ESP_LOGD(TAG, "native LCD draw failed: %s", esp_err_to_name(ret));
            return;
        }
        s_buffer_index = (s_buffer_index + 1) % LCD_DECODE_BUFFER_COUNT;
        return;
    }

    esp_err_t ret = esp_cache_msync(s_decode_buffer, expected_output_size,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                    ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    if (ret == ESP_OK) {
        ret = esp_cache_msync(s_lcd_buffers[s_buffer_index], s_lcd_buffer_len,
                              ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                              ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
    if (ret != ESP_OK) {
        xSemaphoreGive(s_available_decode_buffers);
        ESP_LOGD(TAG, "prepare PPA cache failed: %s", esp_err_to_name(ret));
        return;
    }

    ppa_srm_oper_config_t operation = {
        .in = {
            .buffer = s_decode_buffer,
            .pic_w = width,
            .pic_h = height,
            .block_w = width,
            .block_h = height,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = s_lcd_buffers[s_buffer_index],
            .buffer_size = s_lcd_buffer_len,
            .pic_w = s_panel_width,
            .pic_h = s_panel_height,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)s_panel_width / width,
        .scale_y = (float)s_panel_height / height,
        .byte_swap = true,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ret = ppa_do_scale_rotate_mirror(s_ppa, &operation);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_available_decode_buffers);
        ESP_LOGD(TAG, "PPA scale failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_cache_msync(s_lcd_buffers[s_buffer_index], s_lcd_buffer_len,
                          ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                          ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_available_decode_buffers);
        ESP_LOGD(TAG, "finish PPA cache failed: %s", esp_err_to_name(ret));
        return;
    }

    size_t pixel_count = (size_t)s_panel_width * s_panel_height;
    for (size_t i = 0; i < pixel_count; i++) {
        s_lcd_buffers[s_buffer_index][i] = __builtin_bswap16(s_lcd_buffers[s_buffer_index][i]);
    }
    ret = esp_cache_msync(
        s_lcd_buffers[s_buffer_index], s_lcd_buffer_len,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_available_decode_buffers);
        ESP_LOGD(TAG, "prepare LCD cache failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, s_panel_width, s_panel_height,
                                    s_lcd_buffers[s_buffer_index]);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_available_decode_buffers);
        ESP_LOGD(TAG, "LCD draw failed: %s", esp_err_to_name(ret));
        return;
    }

    s_buffer_index = (s_buffer_index + 1) % LCD_DECODE_BUFFER_COUNT;
}

void app_lcd_draw_rgb565(const uint8_t *buf, uint32_t len,
                         uint16_t width, uint16_t height)
{
    size_t expected_len = (size_t)s_panel_width * s_panel_height * sizeof(uint16_t);
    if (!buf || width != s_panel_width || height != s_panel_height || len != expected_len) {
        ESP_LOGW(TAG, "Drop invalid RGB565 frame: %ux%u, len=%" PRIu32,
                 width, height, len);
        return;
    }

    if (xSemaphoreTake(s_available_decode_buffers, portMAX_DELAY) != pdTRUE) {
        return;
    }

    uint16_t *output = s_lcd_buffers[s_buffer_index];
    const uint16_t *input = (const uint16_t *)buf;
    size_t pixel_count = expected_len / sizeof(uint16_t);
    for (size_t i = 0; i < pixel_count; i++) {
        output[i] = __builtin_bswap16(input[i]);
    }

    esp_err_t ret = esp_cache_msync(output, expected_len,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                    ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, s_panel_width, s_panel_height,
                                        output);
    }
    if (ret != ESP_OK) {
        xSemaphoreGive(s_available_decode_buffers);
        ESP_LOGD(TAG, "RGB565 LCD draw failed: %s", esp_err_to_name(ret));
        return;
    }
    s_buffer_index = (s_buffer_index + 1) % LCD_DECODE_BUFFER_COUNT;
}

esp_err_t app_lcd_get_resolution(uint16_t *width, uint16_t *height)
{
    ESP_RETURN_ON_FALSE(width && height, ESP_ERR_INVALID_ARG, TAG, "resolution output is NULL");
    ESP_RETURN_ON_FALSE(s_usb_width && s_usb_height, ESP_ERR_INVALID_STATE, TAG,
                        "display is not initialized");
    *width = s_usb_width;
    *height = s_usb_height;
    return ESP_OK;
}

esp_err_t app_lcd_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG, "enable VCC_3V3 failed");

    const bsp_display_config_t config = {
        .rotation = BSP_LCD_ROTATION_DEFAULT,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT,
        .buffer_height = 0,
        .task_stack_size = 0,
        .enable_ppa_accel = false,
        .enable_touch = false,
    };
    ESP_RETURN_ON_ERROR(bsp_display_new(&config, &s_panel), TAG, "initialize CO5300 failed");

    s_panel_width = BSP_LCD_H_RES;
    s_panel_height = BSP_LCD_V_RES;
    s_usb_width = CONFIG_USB_EXTEND_SCREEN_WIDTH;
    s_usb_height = CONFIG_USB_EXTEND_SCREEN_HEIGHT;
    ESP_RETURN_ON_FALSE(s_usb_width <= s_panel_width && s_usb_height <= s_panel_height,
                        ESP_ERR_INVALID_ARG, TAG, "USB resolution exceeds panel resolution");
    ESP_LOGI(TAG, "USB display %ux%u -> Mosaico panel %ux%u (scale %.2fx%.2f)",
             s_usb_width, s_usb_height, s_panel_width, s_panel_height,
             (double)s_panel_width / s_usb_width, (double)s_panel_height / s_usb_height);

#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
    s_decode_config.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
    jpeg_decode_engine_cfg_t engine_config = {
        .intr_priority = 1,
        .timeout_ms = 50,
    };
    ESP_RETURN_ON_ERROR(jpeg_new_decoder_engine(&engine_config, &s_jpeg_decoder),
                        TAG, "create JPEG decoder failed");
#endif

    const ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_config, &s_ppa),
                        TAG, "register PPA scaler failed");

    ESP_RETURN_ON_ERROR(app_lcd_allocate_decode_buffers(), TAG, "allocate decode buffers failed");
    s_available_decode_buffers = xSemaphoreCreateCounting(LCD_DECODE_BUFFER_COUNT,
                                                           LCD_DECODE_BUFFER_COUNT);
    ESP_RETURN_ON_FALSE(s_available_decode_buffers, ESP_ERR_NO_MEM, TAG,
                        "create LCD buffer semaphore failed");
    const esp_lcd_panel_io_callbacks_t io_callbacks = {
        .on_color_trans_done = app_lcd_color_trans_done_cb,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(bsp_display_get_panel_io(),
                                                                  &io_callbacks, NULL),
                        TAG, "register LCD transfer callback failed");
    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(100), TAG, "turn LCD on failed");
    return ESP_OK;
}
