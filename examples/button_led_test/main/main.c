/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/**
 * @file main.c
 * @brief GPIO button and status LED test for ESP-Mosaico BSP
 */

#include <inttypes.h>
#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button_led_test";
static uint32_t s_pressed_buttons;

static const char *button_name(bsp_button_t button)
{
    switch (button) {
    case BSP_BUTTON_AI:
        return "AI";
    default:
        return "UNKNOWN";
    }
}

static void update_led(bsp_button_t button, bool pressed)
{
    const uint32_t mask = BIT(button);

    if (pressed) {
        s_pressed_buttons |= mask;
    } else {
        s_pressed_buttons &= ~mask;
    }

    esp_err_t ret = bsp_led_set(s_pressed_buttons != 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set status LED failed: %s", esp_err_to_name(ret));
    }
}

static void button_event_cb(void *button_handle, void *user_data)
{
    const bsp_button_t button = (bsp_button_t)(intptr_t)user_data;
    const button_event_t event = iot_button_get_event(button_handle);

    ESP_LOGI(TAG, "%s: %s", button_name(button), iot_button_get_event_str(event));

    if (event == BUTTON_PRESS_DOWN) {
        update_led(button, true);
    } else if (event == BUTTON_PRESS_UP || event == BUTTON_PRESS_END) {
        update_led(button, false);
    }

    if (event == BUTTON_PRESS_REPEAT || event == BUTTON_PRESS_REPEAT_DONE) {
        ESP_LOGI(TAG, "%s: repeat=%d", button_name(button), iot_button_get_repeat(button_handle));
    }

    if (event == BUTTON_PRESS_UP || event == BUTTON_LONG_PRESS_HOLD || event == BUTTON_LONG_PRESS_UP) {
        ESP_LOGI(TAG, "%s: pressed_time=%" PRIu32 " ms",
                 button_name(button), iot_button_get_pressed_time(button_handle));
    }
}

static esp_err_t register_button_events(button_handle_t button, bsp_button_t index)
{
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_PRESS_DOWN, NULL, button_event_cb,
                                               (void *)(intptr_t)index),
                        TAG, "register %s press-down callback failed", button_name(index));
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_PRESS_UP, NULL, button_event_cb,
                                               (void *)(intptr_t)index),
                        TAG, "register %s press-up callback failed", button_name(index));
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_SINGLE_CLICK, NULL, button_event_cb,
                                               (void *)(intptr_t)index),
                        TAG, "register %s single-click callback failed", button_name(index));
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_DOUBLE_CLICK, NULL, button_event_cb,
                                               (void *)(intptr_t)index),
                        TAG, "register %s double-click callback failed", button_name(index));
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_PRESS_REPEAT, NULL, button_event_cb,
                                               (void *)(intptr_t)index),
                        TAG, "register %s repeat callback failed", button_name(index));
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_PRESS_REPEAT_DONE, NULL, button_event_cb,
                                               (void *)(intptr_t)index),
                        TAG, "register %s repeat-done callback failed", button_name(index));
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_LONG_PRESS_START, NULL, button_event_cb,
                                               (void *)(intptr_t)index),
                        TAG, "register %s long-press-start callback failed", button_name(index));
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_LONG_PRESS_HOLD, NULL, button_event_cb,
                                               (void *)(intptr_t)index),
                        TAG, "register %s long-press-hold callback failed", button_name(index));
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_LONG_PRESS_UP, NULL, button_event_cb,
                                               (void *)(intptr_t)index),
                        TAG, "register %s long-press-up callback failed", button_name(index));
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_PRESS_END, NULL, button_event_cb,
                                               (void *)(intptr_t)index),
                        TAG, "register %s press-end callback failed", button_name(index));
    return ESP_OK;
}

void app_main(void)
{
    button_handle_t buttons[BSP_BUTTON_NUM] = {0};
    int button_count = 0;

    ESP_ERROR_CHECK(bsp_led_init());
    ESP_ERROR_CHECK(bsp_led_set(false));
    ESP_ERROR_CHECK(bsp_iot_button_create(buttons, &button_count, BSP_BUTTON_NUM));
    ESP_LOGI(TAG, "Created %d GPIO buttons", button_count);

    for (int i = 0; i < button_count; i++) {
        ESP_ERROR_CHECK(register_button_events(buttons[i], (bsp_button_t)i));
        ESP_LOGI(TAG, "Registered %s button", button_name((bsp_button_t)i));
    }

    ESP_LOGI(TAG, "Press AI to turn on the status LED");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
