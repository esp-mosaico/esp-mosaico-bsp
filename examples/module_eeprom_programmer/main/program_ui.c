/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "program_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp/display.h"
#include "eeprom_program.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define UI_SAFE_MARGIN      24
#define WRITE_TASK_STACK    6144
#define WRITE_TASK_PRIORITY 4

static const char *TAG = "program_ui";

static lv_obj_t *s_dropdown;
static lv_obj_t *s_info_label;
static lv_obj_t *s_progress_bar;
static lv_obj_t *s_progress_label;
static lv_obj_t *s_result_label;
static lv_obj_t *s_write_btn;
static char s_dropdown_options[512];
static bool s_writing;

static void build_dropdown_options(void)
{
    s_dropdown_options[0] = '\0';
    for (size_t i = 0; i < eeprom_program_profile_count(); i++) {
        if (i > 0) {
            strlcat(s_dropdown_options, "\n", sizeof(s_dropdown_options));
        }
        strlcat(s_dropdown_options, eeprom_program_get_profile(i)->label, sizeof(s_dropdown_options));
    }
}

static void update_profile_info(size_t index)
{
    const eeprom_board_profile_t *profile = eeprom_program_get_profile(index);
    if (!profile || !s_info_label) {
        return;
    }

    char buf[192];
    snprintf(buf, sizeof(buf),
             "Board Type : 0x%02X\nBoard ID   : 0x%04X\nModule HW  : V1.2 / SW: V1.0\nName       : %s",
             profile->board_type, profile->board_id, profile->board_name);
    lv_label_set_text(s_info_label, buf);
}

static void set_write_button_enabled(bool enabled)
{
    if (!s_write_btn) {
        return;
    }

    if (enabled) {
        lv_obj_remove_state(s_write_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s_write_btn, lv_color_hex(0x1E8E5A), 0);
    } else {
        lv_obj_add_state(s_write_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s_write_btn, lv_color_hex(0x35546A), 0);
    }
}

static void on_program_progress(int percent, const char *status)
{
    /* All worker access to LVGL is serialized with the display task. */
    if (bsp_display_lock(-1)) {
        lv_bar_set_value(s_progress_bar, percent, LV_ANIM_OFF);
        lv_label_set_text(s_progress_label, status);
        bsp_display_unlock();
    }
}

static void write_task(void *arg)
{
    size_t profile_index = (size_t)(uintptr_t)arg;
    eeprom_program_result_t result = {0};
    esp_err_t err = eeprom_program_run(profile_index, on_program_progress, &result);
    if (bsp_display_lock(-1)) {
        s_writing = false;
        set_write_button_enabled(true);
        lv_obj_remove_state(s_dropdown, LV_STATE_DISABLED);
        lv_label_set_text(s_result_label, result.message);
        lv_label_set_text(s_progress_label, err == ESP_OK ? "Write succeeded" : "Write failed");
        lv_obj_set_style_text_color(s_result_label, lv_color_hex(err == ESP_OK ? 0x55FFAA : 0xFF6B6B), 0);
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "Display lock failed while reporting write result");
    }
    vTaskDelete(NULL);
}

static void on_write_button_clicked(lv_event_t *event)
{
    (void)event;

    if (s_writing) {
        return;
    }

    const uint32_t selected = lv_dropdown_get_selected(s_dropdown);
    if (selected >= eeprom_program_profile_count()) {
        return;
    }

    s_writing = true;
    lv_obj_add_state(s_dropdown, LV_STATE_DISABLED);
    set_write_button_enabled(false);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    lv_label_set_text(s_progress_label, "Starting write...");
    lv_label_set_text(s_result_label, "");
    lv_obj_set_style_text_color(s_result_label, lv_color_hex(0xB0C4D8), 0);

    if (xTaskCreate(write_task, "eeprom_write", WRITE_TASK_STACK,
                    (void *)(uintptr_t)selected, WRITE_TASK_PRIORITY, NULL) != pdPASS) {
        s_writing = false;
        set_write_button_enabled(true);
        lv_obj_remove_state(s_dropdown, LV_STATE_DISABLED);
        lv_label_set_text(s_progress_label, "Write failed");
        lv_label_set_text(s_result_label, "Write failed:\ncannot create task");
        lv_obj_set_style_text_color(s_result_label, lv_color_hex(0xFF6B6B), 0);
        ESP_LOGE(TAG, "Failed to create write task");
    }
}

static void on_dropdown_changed(lv_event_t *event)
{
    lv_obj_t *dropdown = lv_event_get_target(event);
    update_profile_info(lv_dropdown_get_selected(dropdown));
}

static lv_obj_t *create_card(lv_obj_t *parent, lv_coord_t y, lv_coord_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, BSP_LCD_H_RES - UI_SAFE_MARGIN * 2, h);
    lv_obj_set_pos(card, UI_SAFE_MARGIN, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x102739), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x133045), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

esp_err_t program_ui_start(void)
{
    build_dropdown_options();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x071522), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "EEPROM Programmer");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0x55C2FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *card = create_card(screen, 58, 230);

    lv_obj_t *type_label = lv_label_create(card);
    lv_label_set_text(type_label, "Board Type");
    lv_obj_set_style_text_font(type_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(type_label, lv_color_hex(0x55C2FF), 0);
    lv_obj_align(type_label, LV_ALIGN_TOP_LEFT, 0, 0);

    s_dropdown = lv_dropdown_create(card);
    lv_dropdown_set_options(s_dropdown, s_dropdown_options);
    lv_obj_set_width(s_dropdown, BSP_LCD_H_RES - UI_SAFE_MARGIN * 2 - 36);
    lv_obj_set_height(s_dropdown, 48);
    lv_obj_align(s_dropdown, LV_ALIGN_TOP_LEFT, 0, 36);
    lv_obj_set_style_text_font(s_dropdown, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_dropdown), &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_add_event_cb(s_dropdown, on_dropdown_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_info_label = lv_label_create(card);
    lv_label_set_text(s_info_label, "");
    lv_obj_set_style_text_font(s_info_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_info_label, lv_color_hex(0xD7E3EE), 0);
    lv_obj_set_style_text_line_space(s_info_label, 4, 0);
    lv_obj_align(s_info_label, LV_ALIGN_TOP_LEFT, 0, 92);
    update_profile_info(0);

    s_progress_label = lv_label_create(screen);
    lv_label_set_text(s_progress_label, "Ready");
    lv_obj_set_style_text_font(s_progress_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_progress_label, lv_color_hex(0x90A4B7), 0);
    lv_obj_align(s_progress_label, LV_ALIGN_TOP_MID, 0, 296);

    s_progress_bar = lv_bar_create(screen);
    lv_obj_set_size(s_progress_bar, BSP_LCD_H_RES - UI_SAFE_MARGIN * 2, 22);
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0x1A3040), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0x3ECF8E), LV_PART_INDICATOR);
    lv_obj_align(s_progress_bar, LV_ALIGN_TOP_MID, 0, 322);

    s_result_label = lv_label_create(screen);
    lv_label_set_text(s_result_label, "");
    lv_obj_set_style_text_font(s_result_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_result_label, lv_color_hex(0xB0C4D8), 0);
    lv_obj_set_style_text_align(s_result_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_result_label, BSP_LCD_H_RES - UI_SAFE_MARGIN * 2);
    lv_obj_align(s_result_label, LV_ALIGN_TOP_MID, 0, 352);

    s_write_btn = lv_button_create(screen);
    lv_obj_set_size(s_write_btn, 220, 48);
    lv_obj_align(s_write_btn, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_radius(s_write_btn, 28, 0);
    lv_obj_set_style_bg_color(s_write_btn, lv_color_hex(0x1E8E5A), 0);
    lv_obj_add_event_cb(s_write_btn, on_write_button_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *write_btn_label = lv_label_create(s_write_btn);
    lv_label_set_text(write_btn_label, "Start Write");
    lv_obj_set_style_text_font(write_btn_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(write_btn_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(write_btn_label);

    ESP_LOGI(TAG, "Program UI ready, %u board profiles", (unsigned)eeprom_program_profile_count());
    return ESP_OK;
}
