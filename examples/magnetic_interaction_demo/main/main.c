/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "bsp/esp_mosaico.h"
#include "esp_log.h"
#include "interaction_controller.h"
#include "interaction_ui.h"

static const char *TAG = "mag_interact_demo";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting magnetic interaction demo");

    lv_display_t *display = bsp_display_start();
    if (!display) {
        ESP_LOGE(TAG, "start display failed");
        return;
    }

    ESP_ERROR_CHECK(interaction_controller_start());

    if (!bsp_display_lock(-1)) {
        ESP_LOGE(TAG, "lock display failed");
        return;
    }
    interaction_ui_create();
    bsp_display_unlock();

    ESP_LOGI(TAG, "Magnetic interaction UI ready: %dx%d", BSP_LCD_H_RES, BSP_LCD_V_RES);
}
