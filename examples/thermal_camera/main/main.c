/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* SPDX-License-Identifier: Apache-2.0 */
#include <inttypes.h>
#include <math.h>
#include "bsp/esp_mosaico.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mosaico_module_thermal.h"
#include "mosaico_module_mgr.h"
#include "thermal_share.h"
#include "thermal_ui.h"

static const char *TAG = "thermal";
static mosaico_thermal_frame_t s_frame;

static bool thermal_present(mosaico_module_mgr_slot_t slot)
{
    mosaico_module_mgr_info_t info;
    esp_err_t ret = mosaico_module_mgr_get_info(slot, &info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Read slot state failed: %s", esp_err_to_name(ret));
        return false;
    }
    return info.presence == MOSAICO_MODULE_PRESENCE_PRESENT &&
           info.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID &&
           info.eeprom.board_type == MOSAICO_BOARD_TYPE_THERMAL;
}

/* Run above LVGL priority on the same core as the BSP I2C ISR. */
static void thermal_task(void *arg)
{
    (void)arg;
    esp_err_t ret;
    const mosaico_thermal_config_t config = MOSAICO_THERMAL_DEFAULT_CONFIG();
    const mosaico_module_mgr_claim_config_t claim = {
        .expected_type = MOSAICO_BOARD_TYPE_THERMAL,
        .slot = MOSAICO_MODULE_MGR_SLOT_AUTO,
        .timeout_ms = 1000,
    };
    while (true) {
        mosaico_module_lease_t lease = {0};
        ret = mosaico_module_mgr_claim(&claim, &lease);
        if (ret != ESP_OK) {
            if (ret != ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "Claim thermal module failed: %s", esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            continue;
        }
        ESP_LOGI(TAG, "Thermal module claimed: %s", mosaico_module_mgr_slot_to_name(lease.slot));
        mosaico_thermal_handle_t sensor = NULL;
        bsp_subboard_slot_config_t slot_config = {0};
        ret = bsp_subboard_get_slot_config((bsp_subboard_slot_t)lease.slot, &slot_config);
        if (ret == ESP_OK) {
            ret = mosaico_thermal_new((bsp_subboard_slot_t)lease.slot, &config, &sensor);
        }
        if (ret == ESP_OK) {
            /* The sensor is mounted 180 degrees in the left slot; the right slot adds another 180. */
            const bool rotate_180 = !slot_config.rotated_180;
            ESP_LOGI(TAG, "Display rotation: slot=%s degrees=%u", mosaico_module_mgr_slot_to_name(lease.slot), rotate_180 ? 180U : 0U);
            unsigned settling = 2;
            uint32_t frames = 0;
            while (thermal_present(lease.slot) && (ret = mosaico_thermal_read(sensor, &s_frame, 1000)) == ESP_OK) {
                /* Discard a frame if removal was detected during acquisition. */
                if (!thermal_present(lease.slot)) {
                    break;
                }
                /* Discard settling frames after each initialization. */
                if (settling) {
                    --settling;
                    continue;
                }
                thermal_share_update(s_frame.pixels, s_frame.ambient_c, s_frame.supply_v, rotate_180);
                if (++frames % 32 == 0) {
                    float minimum = s_frame.pixels[0], maximum = minimum;
                    for (unsigned i = 1; i < MOSAICO_THERMAL_PIXEL_COUNT; ++i) {
                        minimum = fminf(minimum, s_frame.pixels[i]);
                        maximum = fmaxf(maximum, s_frame.pixels[i]);
                    }
                    ESP_LOGI(TAG, "frame=%" PRIu32 " Ta=%.1f C Vdd=%.2f V min=%.1f max=%.1f center=%.1f C",
                             frames, s_frame.ambient_c, s_frame.supply_v, minimum, maximum,
                             s_frame.pixels[(MOSAICO_THERMAL_HEIGHT / 2) * MOSAICO_THERMAL_WIDTH + MOSAICO_THERMAL_WIDTH / 2]);
                }
            }
        }
        thermal_share_invalidate();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Sensor unavailable, retrying: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Thermal module removed");
        }
        while (sensor && mosaico_thermal_del(sensor) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        sensor = NULL;
        /* Keep the lease until the I2C device has been removed. */
        while ((ret = mosaico_module_mgr_release(&lease)) != ESP_OK) {
            ESP_LOGE(TAG, "Release thermal slot failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(mosaico_module_mgr_init(NULL));
    bsp_display_config_t display = BSP_DISPLAY_DEFAULT_CONFIG();
    display.buffer_height = 120;
    display.task_stack_size = 16 * 1024;
    display.enable_touch = false;
    if (!bsp_display_start_with_config(&display)) {
        ESP_LOGE(TAG, "Display start failed");
        return;
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(80));
    ESP_ERROR_CHECK(thermal_share_init());
    if (!bsp_display_lock(-1)) {
        ESP_LOGE(TAG, "Display lock failed");
        return;
    }
    esp_err_t ret = thermal_ui_start();
    bsp_display_unlock();
    ESP_ERROR_CHECK(ret);

    if (xTaskCreatePinnedToCore(thermal_task, "thermal", 10240, NULL, 7, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Create thermal task failed");
    }
}
