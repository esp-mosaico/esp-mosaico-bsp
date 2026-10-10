/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "matrix_ui.h"

#include "bsp/display.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define VIEW_SIZE 240
#define UI_REFRESH_MS 50

static const char *TAG = "matrix_ui";
static lv_obj_t *s_pixels[MOSAICO_MATRIX_LED_WIDTH * MOSAICO_MATRIX_LED_HEIGHT];
static lv_obj_t *s_effect;
static lv_obj_t *s_slot;

static lv_obj_t *create_label(lv_obj_t *screen, const char *text, const lv_font_t *font, uint32_t color, lv_align_t align, int y)
{
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_align(label, align, 0, y);
    return label;
}

esp_err_t matrix_ui_init(void)
{
    bsp_display_config_t config = BSP_DISPLAY_DEFAULT_CONFIG();
    config.enable_touch = false;
    ESP_RETURN_ON_FALSE(bsp_display_start_with_config(&config), ESP_FAIL, TAG, "LCD initialization failed");
    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(80), TAG, "LCD brightness failed");
    ESP_RETURN_ON_FALSE(bsp_display_lock(-1), ESP_ERR_TIMEOUT, TAG, "display lock failed");

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x071522), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    create_label(screen, "Matrix LED", &lv_font_montserrat_28, 0x55C2FF, LV_ALIGN_TOP_MID, 14);
    s_effect = create_label(screen, "Waiting for matrix board", &lv_font_montserrat_16, 0x90A4B7, LV_ALIGN_TOP_MID, 374);
    s_slot = create_label(screen, "Insert into either slot", &lv_font_montserrat_16, 0x90A4B7, LV_ALIGN_BOTTOM_MID, -16);

    const int cell = VIEW_SIZE / MOSAICO_MATRIX_LED_WIDTH;
    for (int y = 0; y < MOSAICO_MATRIX_LED_HEIGHT; ++y) {
        for (int x = 0; x < MOSAICO_MATRIX_LED_WIDTH; ++x) {
            /* Reference preview orientation: LED0 top-right, LED63 bottom-left. */
            const int index = (MOSAICO_MATRIX_LED_WIDTH - 1 - x) * MOSAICO_MATRIX_LED_HEIGHT + y;
            lv_obj_t *pixel = lv_obj_create(screen);
            lv_obj_remove_style_all(pixel);
            lv_obj_set_pos(pixel, (BSP_LCD_H_RES - VIEW_SIZE) / 2 + x * cell, 58 + y * cell);
            lv_obj_set_size(pixel, cell, cell);
            lv_obj_set_style_bg_opa(pixel, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(pixel, lv_color_black(), 0);
            lv_obj_remove_flag(pixel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(pixel, LV_OBJ_FLAG_HIDDEN);
            s_pixels[index] = pixel;
        }
    }
    bsp_display_unlock();
    ESP_LOGI(TAG, "LCD preview ready: 240x240, 8x8 pixels");
    return ESP_OK;
}

void matrix_ui_set_slot(bsp_subboard_slot_t slot)
{
    if (!bsp_display_lock(-1)) {
        return;
    }
    const bool waiting = slot == BSP_SUBBOARD_SLOT_COUNT;
    for (int i = 0; i < MOSAICO_MATRIX_LED_WIDTH * MOSAICO_MATRIX_LED_HEIGHT; ++i) {
        lv_obj_set_style_bg_color(s_pixels[i], lv_color_black(), 0);
        if (waiting) {
            lv_obj_add_flag(s_pixels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_pixels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_label_set_text(s_effect, waiting ? "Waiting for matrix board" : "plasma  <-");
    lv_label_set_text(s_slot, waiting ? "Insert into either slot" : slot == BSP_SUBBOARD_SLOT_LEFT ? "Left slot" : "Right slot");
    bsp_display_unlock();
}

void matrix_ui_update(const uint8_t *red, const uint8_t *green, const uint8_t *blue, const char *effect)
{
    static TickType_t last_update;
    const TickType_t now = xTaskGetTickCount();
    if (now - last_update < pdMS_TO_TICKS(UI_REFRESH_MS) || !bsp_display_lock(-1)) {
        return;
    }
    /* LVGL owns the pixel colors after this call; no shared image buffer is needed. */
    for (int i = 0; i < MOSAICO_MATRIX_LED_WIDTH * MOSAICO_MATRIX_LED_HEIGHT; ++i) {
        lv_obj_set_style_bg_color(s_pixels[i], lv_color_make(red[i], green[i], blue[i]), 0);
    }
    lv_label_set_text_fmt(s_effect, "%s  <-", effect);
    bsp_display_unlock();
    last_update = now;
}
