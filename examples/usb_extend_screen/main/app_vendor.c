/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <string.h>
#include <sys/param.h>

#include "esp_log.h"
#include "esp_check.h"
#include "app_usb.h"
#include "app_lcd.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_frame.h"

static const char *TAG = "app_vendor";
static frame_t *current_frame = NULL;
static uint16_t s_screen_width;
static uint16_t s_screen_height;

//--------------------------------------------------------------------+
// Vendor callbacks
//--------------------------------------------------------------------+

#define CONFIG_USB_VENDOR_RX_BUFSIZE  VENDOR_BUF_SIZE

// -- Display Packets
#define UDISP_TYPE_RGB565  0
#define UDISP_TYPE_RGB888  1
#define UDISP_TYPE_YUV420  2
#define UDISP_TYPE_JPG     3
#define UDISP_TYPE_END     0xff

typedef struct {
    uint16_t crc16;
    uint8_t  type;
    uint8_t cmd;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t frame_id: 10;
    uint32_t payload_total: 22; //padding 32bit align
} __attribute__((packed)) udisp_frame_header_t;

static uint8_t s_header_buf[sizeof(udisp_frame_header_t)];
static size_t s_header_len;
static bool s_skip_frame;
static frame_info_t s_skip_frame_info;

void transfer_task(void *pvParameter)
{
    frame_t *usr_frame = NULL;
    while (1) {
        usr_frame = frame_get_filled();
        if (!usr_frame) {
            ESP_LOGW(TAG, "Failed to get a filled frame");
            vTaskDelay(1);
            continue;
        }
        if (usr_frame->info.type == UDISP_TYPE_RGB565) {
            app_lcd_draw_rgb565(usr_frame->data, usr_frame->info.total,
                                usr_frame->info.width, usr_frame->info.height);
        } else {
            app_lcd_draw(usr_frame->data, usr_frame->info.total,
                         usr_frame->info.width, usr_frame->info.height);
        }
        frame_return_empty(usr_frame);

        /*
         * When input is faster than JPEG decode + RGB frame copy, the filled
         * queue never becomes empty. Block for one tick so the CPU1 idle task
         * can run and service the task watchdog.
         */
        vTaskDelay(1);
    }
}

static bool buffer_skip(frame_info_t *frame_info, uint32_t len)
{
    if (frame_info->received + len >= frame_info->total) {
        return true;
    }
    frame_info->received += len;
    return false;
}

static bool start_skip_frame(frame_info_t *frame_info, uint32_t total, uint32_t received)
{
    memset(frame_info, 0, sizeof(*frame_info));
    frame_info->total = total;
    return !buffer_skip(frame_info, received);
}

static bool buffer_fill(frame_t *frame, uint8_t *buf, uint32_t len)
{
    if (0 == len) {
        return false;
    }

    if (frame_add_data(frame, buf, len) != ESP_OK) {
        ESP_LOGW(TAG, "Drop frame: payload overflow, total=%"PRIu32", received=%"PRIu32", len=%"PRIu32,
                 frame->info.total, frame->info.received, len);
        frame_return_empty(frame);
        return true;
    }
    frame->info.received += len;

    if (frame->info.received == frame->info.total) {
        if (frame_send_filled(frame) != ESP_OK) {
            frame_return_empty(frame);
        }
        return true;
    }
    return false;
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const* buffer, uint16_t bufsize)
{
    static uint8_t rx_buf[CONFIG_USB_VENDOR_RX_BUFSIZE];

    while (tud_vendor_n_available(itf)) {
        int read_res = tud_vendor_n_read(itf, rx_buf, CONFIG_USB_VENDOR_RX_BUFSIZE);
        if (read_res <= 0) {
            break;
        }

        size_t offset = 0;
        while (offset < (size_t)read_res) {
            if (current_frame) {
                uint32_t remaining = current_frame->info.total - current_frame->info.received;
                size_t chunk_len = MIN((size_t)remaining, (size_t)read_res - offset);
                if (buffer_fill(current_frame, &rx_buf[offset], chunk_len)) {
                    current_frame = NULL;
                }
                offset += chunk_len;
                continue;
            }

            if (s_skip_frame) {
                uint32_t remaining = s_skip_frame_info.total - s_skip_frame_info.received;
                size_t chunk_len = MIN((size_t)remaining, (size_t)read_res - offset);
                s_skip_frame_info.received += chunk_len;
                offset += chunk_len;
                if (s_skip_frame_info.received == s_skip_frame_info.total) {
                    s_skip_frame = false;
                }
                continue;
            }

            size_t header_chunk = MIN(sizeof(s_header_buf) - s_header_len,
                                      (size_t)read_res - offset);
            memcpy(s_header_buf + s_header_len, rx_buf + offset, header_chunk);
            s_header_len += header_chunk;
            offset += header_chunk;
            if (s_header_len < sizeof(s_header_buf)) {
                continue;
            }

            udisp_frame_header_t header;
            memcpy(&header, s_header_buf, sizeof(header));
            s_header_len = 0;

            switch (header.type) {
            case UDISP_TYPE_RGB888:
            case UDISP_TYPE_YUV420:
                ESP_LOGW(TAG, "Drop unsupported frame type: %u", header.type);
                s_skip_frame = start_skip_frame(&s_skip_frame_info, header.payload_total, 0);
                break;
            case UDISP_TYPE_RGB565:
            case UDISP_TYPE_JPG: {
                if (header.x != 0 || header.y != 0 ||
                        header.width != s_screen_width || header.height != s_screen_height) {
                    ESP_LOGW(TAG, "Drop frame with unexpected area: x=%u y=%u w=%u h=%u",
                             header.x, header.y, header.width, header.height);
                    s_skip_frame = start_skip_frame(&s_skip_frame_info, header.payload_total, 0);
                    continue;
                }
                uint32_t expected_rgb_size = (uint32_t)header.width * header.height * 2U;
                if ((header.type == UDISP_TYPE_RGB565 &&
                     header.payload_total != expected_rgb_size) ||
                    header.payload_total == 0 ||
                    header.payload_total > CONFIG_USB_EXTEND_SCREEN_FRAME_LIMIT_B) {
                    ESP_LOGW(TAG, "Drop frame: payload_total=%"PRIu32", limit=%u",
                             (uint32_t)header.payload_total, CONFIG_USB_EXTEND_SCREEN_FRAME_LIMIT_B);
                    s_skip_frame = start_skip_frame(&s_skip_frame_info, header.payload_total, 0);
                    break;
                }

                static int fps_count = 0;
                static int64_t start_time = 0;
                if (start_time == 0) {
                    start_time = esp_timer_get_time();
                }
                fps_count++;
                if (fps_count == 50) {
                    int64_t end_time = esp_timer_get_time();
                    ESP_LOGI(TAG, "Input fps: %f", 1000000.0 / ((end_time - start_time) / 50.0));
                    start_time = end_time;
                    fps_count = 0;
                }

                current_frame = frame_get_empty();
                if (current_frame) {
                    current_frame->info.width = header.width;
                    current_frame->info.height = header.height;
                    current_frame->info.type = header.type;
                    current_frame->info.total = header.payload_total;
                    current_frame->info.received = 0;
                } else {
                    static uint32_t dropped_frames;
                    s_skip_frame = start_skip_frame(&s_skip_frame_info, header.payload_total, 0);
                    dropped_frames++;
                    if (dropped_frames == 1 || dropped_frames % 50 == 0) {
                        ESP_LOGW(TAG, "Drop frame: decoder is busy, dropped=%"PRIu32, dropped_frames);
                    }
                }
                break;
            }
            case UDISP_TYPE_END:
                if (header.payload_total > 0) {
                    s_skip_frame = start_skip_frame(&s_skip_frame_info, header.payload_total, 0);
                }
                break;
            default:
                ESP_LOGW(TAG, "Drop unknown frame type: %u", header.type);
                if (header.payload_total > 0) {
                    s_skip_frame = start_skip_frame(&s_skip_frame_info, header.payload_total, 0);
                }
                break;
            }
        }
    }
}

void app_vendor_reset(void)
{
    if (current_frame) {
        frame_return_empty(current_frame);
        current_frame = NULL;
    }
    s_header_len = 0;
    s_skip_frame = false;
    memset(&s_skip_frame_info, 0, sizeof(s_skip_frame_info));
}

esp_err_t app_vendor_init(void)
{
    ESP_RETURN_ON_ERROR(app_lcd_get_resolution(&s_screen_width, &s_screen_height),
                        TAG, "get display resolution failed");
    ESP_RETURN_ON_ERROR(frame_allocate(6, CONFIG_USB_EXTEND_SCREEN_FRAME_LIMIT_B),
                        TAG, "allocate frame buffers failed");
    BaseType_t task_created = xTaskCreatePinnedToCore(transfer_task, "transfer_task", 4096, NULL,
                                                      CONFIG_VENDOR_TASK_PRIORITY, NULL, 1);
    ESP_RETURN_ON_FALSE(task_created == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "create transfer task failed");
    return ESP_OK;
}
