/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/display.h"
#include "bsp/esp_mosaico.h"
#include "bsp/subboard.h"
#include "eeprom_program.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "program_ui.h"

static const char *TAG = "eeprom_program_app";

#define MODULE_POWER_UP_MS  200
#define UI_BUFFER_HEIGHT    120
#define UI_TASK_STACK_SIZE  (12 * 1024)

static esp_err_t display_start(void)
{
    bsp_display_config_t cfg = BSP_DISPLAY_DEFAULT_CONFIG();
    cfg.buffer_height = UI_BUFFER_HEIGHT;
    cfg.task_stack_size = UI_TASK_STACK_SIZE;
    cfg.enable_ppa_accel = true;
    cfg.enable_touch = true;

    if (!bsp_display_start_with_config(&cfg)) {
        ESP_LOGE(TAG, "display init failed");
        return ESP_FAIL;
    }

    return bsp_display_brightness_set(80);
}

void app_main(void)
{
    bsp_board_variant_t variant;
    ESP_ERROR_CHECK(bsp_board_variant_get(&variant));
    ESP_LOGI(TAG, "Core board mapping=v1.%u, EEPROM bus=I2C%u, profiles=%u",
             variant == BSP_BOARD_VARIANT_V1_0 ? 0U : 2U,
             variant == BSP_BOARD_VARIANT_V1_0 ? 0U : 1U, (unsigned)eeprom_program_profile_count());

    /* BSP selects shared I2C0 on v1.0 and dedicated I2C1 on v1.2. */
    ESP_ERROR_CHECK(bsp_subboard_init());
    vTaskDelay(pdMS_TO_TICKS(MODULE_POWER_UP_MS));
    ESP_ERROR_CHECK(eeprom_program_init(bsp_subboard_get_i2c_bus()));
    ESP_ERROR_CHECK(display_start());

    if (!bsp_display_lock(-1)) {
        ESP_LOGE(TAG, "LVGL lock failed");
        return;
    }
    ESP_ERROR_CHECK(program_ui_start());
    bsp_display_unlock();
}
