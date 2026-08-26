/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/param.h>

#include "app_lcd.h"
#include "app_usb.h"
#include "bsp/display.h"
#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_descriptors.h"

static const char *TAG = "app_touch";

static esp_lcd_touch_handle_t tp = NULL;
static uint16_t s_touch_width;
static uint16_t s_touch_height;
static uint16_t s_display_width;
static uint16_t s_display_height;

static uint16_t app_touch_scale(uint16_t coordinate, uint16_t source_max, uint16_t target_max)
{
    if (source_max == 0 || source_max == target_max) {
        return MIN(coordinate, target_max);
    }
    return MIN((uint32_t)coordinate * target_max / source_max, target_max);
}

static void app_touch_task(void *arg)
{
    uint8_t touchpad_cnt = 0;
    bool send_press = false;
    while (1) {
        if (esp_lcd_touch_read_data(tp) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        esp_lcd_touch_point_data_t touch_points[CONFIG_ESP_LCD_TOUCH_MAX_POINTS] = {0};
        if (esp_lcd_touch_get_data(tp, touch_points, &touchpad_cnt,
                                   CONFIG_ESP_LCD_TOUCH_MAX_POINTS) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        hid_report_t report = {0};
        if (touchpad_cnt > 0) {
            report.report_id = REPORT_ID_TOUCH;
            for (int i = 0; i < touchpad_cnt; i++) {
                report.touch_report.data[i].index = touch_points[i].track_id;
                report.touch_report.data[i].press_down = 1;
                report.touch_report.data[i].x = app_touch_scale(touch_points[i].x, s_touch_width,
                                                                s_display_width);
                report.touch_report.data[i].y = app_touch_scale(touch_points[i].y, s_touch_height,
                                                                s_display_height);
                report.touch_report.data[i].width = touch_points[i].strength;
                report.touch_report.data[i].height = touch_points[i].strength;
            }
            report.touch_report.cnt = touchpad_cnt;
#if CFG_TUD_HID
            tinyusb_hid_keyboard_report(report);
#endif
            send_press = true;
        } else if (send_press) {
            send_press = false;
            report.report_id = REPORT_ID_TOUCH;
#if CFG_TUD_HID
            tinyusb_hid_keyboard_report(report);
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t app_touch_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_touch_new(bsp_display_get_rotation(), &tp), TAG,
                        "initialize CST9217 failed");

    s_touch_width = BSP_LCD_H_RES - 1;
    s_touch_height = BSP_LCD_V_RES - 1;
    ESP_RETURN_ON_ERROR(app_lcd_get_resolution(&s_display_width, &s_display_height),
                        TAG, "get display resolution failed");
    s_display_width--;
    s_display_height--;

    BaseType_t task_created = xTaskCreate(app_touch_task, "app_touch_task", 4096, NULL,
                                          CONFIG_TOUCH_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(task_created == pdPASS, ESP_ERR_NO_MEM, TAG, "create touch task failed");
    ESP_LOGI(TAG, "CST9217 touch ready: source=%ux%u display=%ux%u",
             s_touch_width + 1, s_touch_height + 1, s_display_width + 1, s_display_height + 1);
    return ESP_OK;
}
