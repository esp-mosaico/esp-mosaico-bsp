/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/**
 * @file main.c
 * @brief Basic HMI example for the ESP-Mosaico BSP
 */

#include "bsp/esp_mosaico.h"
#include "esp_log.h"
#include "lv_demos.h"

static const char *TAG = "mosaico_hmi";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP-Mosaico HMI demo");

    lv_display_t *display = bsp_display_start();
    if (!display) {
        ESP_LOGE(TAG, "Failed to start the Mosaico display");
        return;
    }

    if (!bsp_display_lock(-1)) {
        ESP_LOGE(TAG, "Failed to lock LVGL");
        return;
    }

    lv_demo_benchmark();
    bsp_display_unlock();
    ESP_LOGI(TAG, "LVGL benchmark started: resolution=%dx%d touch=enabled",
             BSP_LCD_H_RES, BSP_LCD_V_RES);
}
