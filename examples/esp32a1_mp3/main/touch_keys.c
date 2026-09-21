/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "touch_keys.h"

#include <inttypes.h>
#include <stdbool.h>
#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "si12t.h"

#define TOUCH_FIRST_CHANNEL    5U
#define TOUCH_LAST_CHANNEL     9U
#define TOUCH_CHANNEL_MASK     0x01f0U
#define TOUCH_TRACK_KEY_MASK   (1U << (6U - 1U))
#define TOUCH_NEXT_KEY_MASK    (1U << (8U - 1U))
#define TOUCH_POLL_PERIOD_MS    20U
/* ChHL=0, ChM=000: maximum sensitivity (same as si12t_set_max_sensitivity). */
#define TOUCH_SENSITIVITY       0x02U
/* TK6: ChHL=0, ChM=100; TK8: ChHL=0, ChM=010. */
#define TOUCH_TRACK_SENSITIVITY 0x05U
#define TOUCH_NEXT_SENSITIVITY  0x04U
#define TOUCH_TASK_STACK_SIZE  4096U
#define TOUCH_TASK_PRIORITY    5U

/* ESP32-A1 is installed in the right subboard slot; H12 maps from GPIO4 to GPIO5. */
#define AUDIO_BOARD_SLOT       BSP_SUBBOARD_SLOT_RIGHT
#define TOUCH_IRQ_LEFT_GPIO    GPIO_NUM_4

static const char *TAG = "touch_keys";
static TaskHandle_t s_task;
static si12t_handle_t s_touch;
static touch_key_callback_t s_callback;
static void *s_user_data;
static gpio_num_t s_irq_gpio;

static void touch_irq_handler(void *arg)
{
    (void)arg;
    if (s_task) {
        BaseType_t task_woken = pdFALSE;
        vTaskNotifyGiveFromISR(s_task, &task_woken);
        portYIELD_FROM_ISR(task_woken);
    }
}

static void report_changes(uint16_t previous, uint16_t current)
{
    const uint16_t changed = (previous ^ current) & TOUCH_CHANNEL_MASK;
    for (uint8_t channel = TOUCH_FIRST_CHANNEL; channel <= TOUCH_LAST_CHANNEL; ++channel) {
        const uint16_t bit = (uint16_t)(1U << (channel - 1U));
        if ((changed & bit) == 0) {
            continue;
        }

        const bool pressed = (current & bit) != 0;
        ESP_LOGI(TAG, "TK%u %s", channel, pressed ? "pressed" : "released");
        if (pressed && s_callback) {
            s_callback(channel, s_user_data);
        }
    }
}

static void touch_task(void *arg)
{
    uint16_t previous = (uint16_t)(uintptr_t)arg;
    unsigned consecutive_errors = 0;

    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TOUCH_POLL_PERIOD_MS));

        uint16_t current;
        const esp_err_t ret = si12t_read_pressed(s_touch, &current);
        if (ret != ESP_OK) {
            ++consecutive_errors;
            if (consecutive_errors == 1 || consecutive_errors % 20 == 0) {
                ESP_LOGE(TAG, "read failed (%u): %s", consecutive_errors,
                         esp_err_to_name(ret));
            }
            continue;
        }

        if (consecutive_errors > 0) {
            ESP_LOGI(TAG, "reads recovered after %u error(s)", consecutive_errors);
            consecutive_errors = 0;
        }
        current &= TOUCH_CHANNEL_MASK;
        report_changes(previous, current);
        previous = current;
    }
}

esp_err_t touch_keys_start(touch_key_callback_t callback, void *user_data)
{
    ESP_RETURN_ON_FALSE(callback, ESP_ERR_INVALID_ARG, TAG, "callback is null");
    ESP_RETURN_ON_FALSE(!s_task && !s_touch, ESP_ERR_INVALID_STATE, TAG,
                        "touch keys already started");

    /*
     * On V1.2, bsp_subboard_init() creates I2C1 on GPIO0/GPIO1. This is
     * intentionally not bsp_i2c_init(), which owns the mainboard I2C0 bus
     * used by ES8311 and the on-board sensors.
     */
    ESP_RETURN_ON_ERROR(bsp_subboard_init(), TAG, "initialize V1.2 subboard bus failed");
    i2c_master_bus_handle_t bus = bsp_subboard_get_i2c_bus();
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_STATE, TAG, "subboard I2C handle is null");

    const si12t_config_t touch_config = SI12T_CONFIG_DEFAULT(bus);
    ESP_RETURN_ON_ERROR(si12t_create(&touch_config, &s_touch), TAG,
                        "initialize Si12T failed");
    esp_err_t ret = si12t_set_sensitivity(s_touch, TOUCH_CHANNEL_MASK, TOUCH_SENSITIVITY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set TK5-TK9 sensitivity failed: %s",
                 esp_err_to_name(ret));
        goto fail;
    }

    ret = si12t_set_sensitivity(s_touch, TOUCH_TRACK_KEY_MASK, TOUCH_TRACK_SENSITIVITY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set TK6 sensitivity failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = si12t_set_sensitivity(s_touch, TOUCH_NEXT_KEY_MASK, TOUCH_NEXT_SENSITIVITY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set TK8 sensitivity failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    uint16_t initial_state;
    ret = si12t_read_pressed(s_touch, &initial_state);
    if (ret != ESP_OK) {
        (void)si12t_delete(s_touch);
        s_touch = NULL;
        return ret;
    }
    initial_state &= TOUCH_CHANNEL_MASK;

    s_irq_gpio = bsp_subboard_map_gpio(AUDIO_BOARD_SLOT, TOUCH_IRQ_LEFT_GPIO);
    const gpio_config_t irq_config = {
        .pin_bit_mask = BIT64(s_irq_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ret = gpio_config(&irq_config);
    if (ret != ESP_OK) {
        goto fail;
    }

    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        goto fail;
    }
    ret = gpio_isr_handler_add(s_irq_gpio, touch_irq_handler, NULL);
    if (ret != ESP_OK) {
        goto fail;
    }

    s_callback = callback;
    s_user_data = user_data;
    if (xTaskCreate(touch_task, "si12t_keys", TOUCH_TASK_STACK_SIZE,
                    (void *)(uintptr_t)initial_state, TOUCH_TASK_PRIORITY,
                    &s_task) != pdPASS) {
        ret = ESP_ERR_NO_MEM;
        (void)gpio_isr_handler_remove(s_irq_gpio);
        goto fail;
    }

    ESP_LOGI(TAG,
             "ready: subboard I2C%d SDA=GPIO%d SCL=GPIO%d, Si12T=0x%02x, IRQ=GPIO%d, "
             "TK5/TK7/TK9 SEN=0x%01x, TK6 SEN=0x%01x, TK8 SEN=0x%01x, direct key events",
             BSP_SUBBOARD_I2C_PORT_V1_2, BSP_SUBBOARD_I2C_SDA,
             BSP_SUBBOARD_I2C_SCL, SI12T_I2C_ADDRESS, s_irq_gpio,
             TOUCH_SENSITIVITY, TOUCH_TRACK_SENSITIVITY, TOUCH_NEXT_SENSITIVITY);
    return ESP_OK;

fail:
    (void)si12t_delete(s_touch);
    s_touch = NULL;
    s_callback = NULL;
    s_user_data = NULL;
    return ret;
}
