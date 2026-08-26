/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "app_usb.h"
#include "usb_descriptors.h"
#include "esp_log.h"
#include "bsp/esp_mosaico.h"
#if CONFIG_HID_TOUCH_ENABLE
#include "app_touch.h"
#endif
#include "app_lcd.h"

static const char *TAG = "usb_extend_screen";

void app_main(void)
{
    uint16_t usb_width = 0;
    uint16_t usb_height = 0;
    ESP_LOGI(TAG, "USB extend screen example for ESP-Mosaico");
    ESP_ERROR_CHECK(app_lcd_init());
    ESP_ERROR_CHECK(app_lcd_get_resolution(&usb_width, &usb_height));
    ESP_LOGI(TAG, "USB display: %ux%u, physical panel: %dx%d",
             usb_width, usb_height, BSP_LCD_H_RES, BSP_LCD_V_RES);
    ESP_ERROR_CHECK(app_usb_init());
#if CONFIG_HID_TOUCH_ENABLE
    ESP_ERROR_CHECK(app_touch_init());
#endif
}
