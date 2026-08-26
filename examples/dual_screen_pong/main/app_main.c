/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mosaico_module_mgr.h"
#include "pong_game.h"
#include "pong_ui.h"
#include "sdkconfig.h"

#define PONG_RENDER_PERIOD_MS CONFIG_LV_DEF_REFR_PERIOD

static const char *TAG = "dual_screen_pong";

static void render_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    pong_render_snapshot_t snapshot;
    if (pong_game_get_render_snapshot(&snapshot)) {
        pong_ui_set_viewport(snapshot.local_role);
        pong_ui_update(&snapshot);
    }
}

static esp_err_t board_bringup(void)
{
    ESP_RETURN_ON_ERROR(bsp_power_init(), TAG, "power init failed");
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG,
                        "enable VCC_3V3 failed");
    ESP_RETURN_ON_ERROR(bsp_power_set_codec_3v3(true), TAG,
                        "enable codec 3V3 failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(mosaico_module_mgr_init(NULL), TAG,
                        "subboard manager init failed");
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting dual-screen Pong");
    ESP_ERROR_CHECK(board_bringup());

    bsp_display_config_t display_config = BSP_DISPLAY_DEFAULT_CONFIG();
    display_config.enable_touch = false;
    if (bsp_display_start_with_config(&display_config) == NULL) {
        ESP_LOGE(TAG, "Display initialization failed");
        return;
    }

    if (!bsp_display_lock(-1)) {
        ESP_LOGE(TAG, "LVGL lock failed");
        return;
    }
    esp_err_t err = pong_ui_create(NULL, PONG_ROLE_LEFT);
    if (err == ESP_OK) {
        lv_timer_create(render_timer_cb, PONG_RENDER_PERIOD_MS, NULL);
    }
    bsp_display_unlock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UI initialization failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK(pong_game_start());
}
