/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file main.c
 * @brief Deep-sleep template with timer wake; CHIP_PU/reset wakes immediately.
 */

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>

static const char *TAG = "lp_wakeup";

/** Seconds to stay awake before sleeping again. */
#define AWAKE_SEC        3
/** Timer wake period while sleeping. */
#define WAKE_TIMER_SEC   10

static const char *wakeup_cause_str(uint32_t causes)
{
    if (causes == 0) {
        return "power-on/reset";
    }
    if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
        return "timer";
    }
    return "other";
}

static esp_err_t configure_wake_sources(void)
{
    /* Boot is GPIO61, a digital pad. EXT1 and deep-sleep GPIO wakeup on
     * ESP32-S31 only accept RTC IOs (GPIO0-7), so the Boot button cannot
     * wake deep sleep. Press CHIP_PU/reset for an immediate restart;
     * otherwise the timer wakes the chip. */
    ESP_RETURN_ON_ERROR(
        esp_sleep_enable_timer_wakeup((uint64_t)WAKE_TIMER_SEC * 1000000ULL),
        TAG, "timer wake");
    return ESP_OK;
}

static esp_err_t enter_sleep(void)
{
    /* Prepare while VCC_3V3 is still up (CO5300/NAND skipped if unused). */
    esp_err_t ret = bsp_power_prepare_sleep();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "prepare_sleep: %s (continuing)", esp_err_to_name(ret));
    }

    ESP_RETURN_ON_ERROR(bsp_power_set_codec_3v3(false), TAG, "codec rail off");
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(false), TAG, "vcc rail off");

    ESP_LOGI(TAG, "deep sleep; wake on timer (%ds) or CHIP_PU/reset", WAKE_TIMER_SEC);
    vTaskDelay(pdMS_TO_TICKS(50));
    bsp_power_enter_deep_sleep(); /* noreturn */
    return ESP_OK;
}

void app_main(void)
{
    const uint32_t causes = esp_sleep_get_wakeup_causes();
    ESP_LOGI(TAG, "wake causes=%s (0x%08" PRIx32 ")", wakeup_cause_str(causes), causes);

    ESP_ERROR_CHECK(bsp_power_init());
    ESP_ERROR_CHECK(bsp_power_set_vcc_3v3(true));
    ESP_ERROR_CHECK(bsp_led_init());

    /* Visible “awake” pulse. */
    for (int i = 0; i < 3; ++i) {
        (void)bsp_led_set(true);
        vTaskDelay(pdMS_TO_TICKS(120));
        (void)bsp_led_set(false);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    ESP_LOGI(TAG, "awake for %ds…", AWAKE_SEC);
    vTaskDelay(pdMS_TO_TICKS(AWAKE_SEC * 1000));

    ESP_ERROR_CHECK(configure_wake_sources());
    ESP_ERROR_CHECK(enter_sleep());
}
