/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* SPDX-License-Identifier: Apache-2.0 */
#include "tof_ui.h"
#include "bsp/display.h"
#include "esp_check.h"

#define ACCENT 0x63E6CB
#define MUTED  0x8298AA
#define AMBER  0xFFC477
#define SCALE_MM 2000

static const char *TAG = "tof_ui";
static lv_obj_t *s_slot;
static lv_obj_t *s_distance, *s_state, *s_detail, *s_arc, *s_marker, *s_signal, *s_ambient;

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color, int x, int y, int width)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, width);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    return obj;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

esp_err_t tof_ui_init(void)
{
    bsp_display_config_t config = BSP_DISPLAY_DEFAULT_CONFIG();
    config.enable_touch = false;
    ESP_RETURN_ON_FALSE(bsp_display_start_with_config(&config), ESP_FAIL, TAG, "display start failed");
    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(80), TAG, "brightness failed");
    ESP_RETURN_ON_FALSE(bsp_display_lock(-1), ESP_ERR_TIMEOUT, TAG, "display lock failed");

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x08131E), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    label(screen, "MOSAICO  /  DISTANCE", &lv_font_montserrat_16, 0xDCEAF3, 24, 21, 432);
    s_slot = label(screen, "TIME OF FLIGHT", &lv_font_montserrat_12, MUTED, 24, 46, 432);

    /* A fixed 0..2000 mm scale avoids suggesting precision beyond the sensor range. */
    s_arc = lv_arc_create(screen);
    lv_obj_set_size(s_arc, 250, 250);
    lv_obj_set_pos(s_arc, 115, 70);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, 0, SCALE_MM);
    lv_arc_set_value(s_arc, 0);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x203344), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_INDICATOR);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    label(screen, "TARGET DISTANCE", &lv_font_montserrat_12, MUTED, 130, 127, 220);
    s_distance = label(screen, "--", &lv_font_montserrat_48, 0xF3FAFF, 120, 153, 240);
    label(screen, "millimeters", &lv_font_montserrat_16, MUTED, 130, 212, 220);
    s_state = label(screen, "CONNECTING", &lv_font_montserrat_16, ACCENT, 92, 264, 296);

    box(screen, 40, 320, 400, 3, 0x203344, 1);
    for (int i = 0; i < 5; ++i) {
        box(screen, 40 + i * 100, 317, 1, 9, MUTED, 0);
    }
    s_marker = box(screen, 34, 315, 12, 12, ACCENT, LV_RADIUS_CIRCLE);
    lv_obj_add_flag(s_marker, LV_OBJ_FLAG_HIDDEN);
    label(screen, "0", &lv_font_montserrat_12, MUTED, 24, 331, 32);
    label(screen, "1 m", &lv_font_montserrat_12, MUTED, 220, 331, 40);
    label(screen, "2 m", &lv_font_montserrat_12, MUTED, 416, 331, 48);

    lv_obj_t *card = box(screen, 24, 361, 210, 76, 0x132433, 14);
    label(card, "RETURN SIGNAL / MCPS", &lv_font_montserrat_12, MUTED, 0, 12, 210);
    s_signal = label(card, "--", &lv_font_montserrat_28, ACCENT, 0, 33, 210);
    card = box(screen, 246, 361, 210, 76, 0x132433, 14);
    label(card, "AMBIENT / MCPS", &lv_font_montserrat_12, MUTED, 0, 12, 210);
    s_ambient = label(card, "--", &lv_font_montserrat_28, 0xC4D7E5, 0, 33, 210);
    s_detail = label(screen, "Looking for a ToF module...", &lv_font_montserrat_12, MUTED, 16, 451, 448);
    bsp_display_unlock();
    return ESP_OK;
}

void tof_ui_status(const char *message, const char *detail)
{
    if (!bsp_display_lock(-1)) {
        return;
    }
    lv_label_set_text(s_slot, "TIME OF FLIGHT");
    lv_label_set_text(s_distance, "--");
    lv_label_set_text(s_signal, "--");
    lv_label_set_text(s_ambient, "--");
    lv_label_set_text(s_state, message);
    lv_obj_set_style_text_color(s_state, lv_color_hex(AMBER), 0);
    lv_label_set_text(s_detail, detail);
    lv_arc_set_value(s_arc, 0);
    lv_obj_add_flag(s_marker, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

void tof_ui_update(const mosaico_tof_data_t *data)
{
    if (!bsp_display_lock(-1)) {
        return;
    }
    lv_label_set_text_fmt(s_signal, "%.2f", data->signal_rate_mcps);
    lv_label_set_text_fmt(s_ambient, "%.2f", data->ambient_rate_mcps);
    lv_obj_set_style_text_color(s_state, lv_color_hex(data->valid ? ACCENT : AMBER), 0);
    if (data->valid) {
        unsigned distance = data->distance_mm;
        unsigned position = distance > SCALE_MM ? SCALE_MM : distance;
        lv_label_set_text_fmt(s_distance, "%u", distance);
        lv_label_set_text(s_state, "TARGET LOCKED");
        lv_label_set_text(s_detail, distance > SCALE_MM ? "Beyond visual scale  /  > 2 m" : "LIVE  /  Move an object in front of the sensor");
        lv_arc_set_value(s_arc, position);
        lv_obj_set_x(s_marker, 34 + position * 400 / SCALE_MM);
        lv_obj_remove_flag(s_marker, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* Never retain an old distance when the current sample is invalid. */
        lv_label_set_text(s_distance, "--");
        lv_label_set_text(s_state, "NO VALID TARGET");
        lv_label_set_text_fmt(s_detail, "Range status %u  /  Reposition the target", (unsigned)data->range_status);
        lv_arc_set_value(s_arc, 0);
        lv_obj_add_flag(s_marker, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_display_unlock();
}

void tof_ui_set_slot(mosaico_module_mgr_slot_t slot, const char *model)
{
    if (!bsp_display_lock(-1)) {
        return;
    }
    const char *slot_name = slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? "LEFT SLOT" : "RIGHT SLOT";
    lv_label_set_text_fmt(s_slot, "%s  /  %s", model, slot_name);
    bsp_display_unlock();
}
