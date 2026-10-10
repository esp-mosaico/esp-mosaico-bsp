/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mosaico_matrix_led.h"
#include "esp_check.h"

static const char *TAG = "matrix_led";

esp_err_t mosaico_matrix_led_new(bsp_subboard_slot_t slot, led_strip_handle_t *out_strip)
{
    ESP_RETURN_ON_FALSE(out_strip, ESP_ERR_INVALID_ARG, TAG, "null output handle");
    *out_strip = NULL;
    ESP_RETURN_ON_FALSE(slot >= BSP_SUBBOARD_SLOT_LEFT && slot < BSP_SUBBOARD_SLOT_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid slot");

    const led_strip_config_t strip_config = {
        .strip_gpio_num = bsp_subboard_map_gpio(slot, GPIO_NUM_48),
        .max_leds = MOSAICO_MATRIX_LED_WIDTH * MOSAICO_MATRIX_LED_HEIGHT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = true,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, out_strip), TAG, "create RMT strip failed");
    ESP_LOGI(TAG, "8x8 matrix ready: slot=%s GPIO%d", slot == BSP_SUBBOARD_SLOT_LEFT ? "left" : "right", strip_config.strip_gpio_num);
    return ESP_OK;
}

esp_err_t mosaico_matrix_led_set_pixel(led_strip_handle_t strip, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    ESP_RETURN_ON_FALSE(strip && x >= 0 && x < MOSAICO_MATRIX_LED_WIDTH && y >= 0 && y < MOSAICO_MATRIX_LED_HEIGHT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid pixel");
    /* N-shaped wiring: LED0 is bottom-left; each column runs bottom to top. */
    return led_strip_set_pixel(strip, x * MOSAICO_MATRIX_LED_HEIGHT + y, r, g, b);
}
