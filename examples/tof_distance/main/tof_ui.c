/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "tof_ui.h"
#include "bsp/display.h"
#include "esp_check.h"

#define ACCENT 0x63E6CB
#define MUTED 0x8298AA
#define AMBER 0xFFC477
#define SCALE_MM 2000

static const char *TAG = "tof_ui";
typedef struct {
    lv_obj_t *distance;
    lv_obj_t *status;
    lv_obj_t *detail;
    lv_obj_t *bar;
} slot_ui_t;
static slot_ui_t s_slots[2];
#define GESTURE_LED_COUNT 12
static lv_obj_t *s_gesture_label;
static lv_obj_t *s_gesture_leds[GESTURE_LED_COUNT];
static lv_timer_t *s_gesture_timer;
static tof_gesture_event_t s_last_gesture;
static uint32_t s_gesture_started;

/* Runs in the LVGL task, independently of blocking ToF measurements. */
static void gesture_animation(lv_timer_t *timer)
{
    unsigned step = lv_tick_elaps(s_gesture_started) / 70;
    for (int i = 0; i < GESTURE_LED_COUNT; ++i) {
        int position = s_last_gesture == TOF_GESTURE_RIGHT ? i : GESTURE_LED_COUNT - 1 - i;
        int trail = (int)step - position;
        uint32_t color = trail == 0 ? 0xD8FFF3 : trail == 1 ? ACCENT :
                         trail == 2 ? 0x329E88 : trail == 3 ? 0x20584F : 0x203344;
        lv_obj_set_style_bg_color(s_gesture_leds[i], lv_color_hex(color), 0);
    }
    if (step >= GESTURE_LED_COUNT + 3) lv_timer_pause(timer);
}

static slot_ui_t *slot_ui(mosaico_module_mgr_slot_t slot)
{
    if (slot == MOSAICO_MODULE_MGR_SLOT_LEFT) return &s_slots[0];
    if (slot == MOSAICO_MODULE_MGR_SLOT_RIGHT) return &s_slots[1];
    return NULL;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       uint32_t color, int x, int y, int width)
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
    label(screen, "ESP-MOSAICO TOF MODULE", &lv_font_montserrat_16, 0xDCEAF3, 16, 15, 448);

    for (int i = 0; i < 2; ++i) {
        slot_ui_t *ui = &s_slots[i];
        lv_obj_t *card = lv_obj_create(screen);
        lv_obj_remove_style_all(card);
        lv_obj_set_pos(card, 16 + i * 232, 45);
        lv_obj_set_size(card, 216, 308);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x132433), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        label(card, i == 0 ? "LEFT" : "RIGHT",
              &lv_font_montserrat_28, ACCENT, 12, 8, 192);
        label(card, i == 0 ? "I2C 0x30" : "I2C 0x31",
              &lv_font_montserrat_16, MUTED, 12, 42, 192);
        ui->distance = label(card, "--", &lv_font_montserrat_48, 0xF3FAFF, 12, 78, 192);
        lv_obj_set_style_transform_pivot_x(ui->distance, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(ui->distance, LV_PCT(50), 0);
        lv_obj_set_style_transform_scale_x(ui->distance, 384, 0);
        lv_obj_set_style_transform_scale_y(ui->distance, 384, 0);
        label(card, "mm", &lv_font_montserrat_28, ACCENT, 12, 148, 192);
        ui->status = label(card, "WAITING FOR MODULE", &lv_font_montserrat_16,
                           AMBER, 12, 185, 192);
        lv_obj_set_height(ui->status, 48);
        ui->bar = lv_bar_create(card);
        lv_obj_set_pos(ui->bar, 16, 232);
        lv_obj_set_size(ui->bar, 184, 6);
        lv_bar_set_range(ui->bar, 0, SCALE_MM);
        lv_bar_set_value(ui->bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(ui->bar, lv_color_hex(0x203344), LV_PART_MAIN);
        lv_obj_set_style_bg_color(ui->bar, lv_color_hex(ACCENT), LV_PART_INDICATOR);
        label(card, "0 mm", &lv_font_montserrat_12, MUTED, 8, 242, 60);
        label(card, "2000 mm", &lv_font_montserrat_12, MUTED, 120, 242, 88);
        ui->detail = label(card, "SIGNAL -- MCPS\nAMBIENT -- MCPS", &lv_font_montserrat_12,
                           MUTED, 12, 270, 192);
        lv_obj_set_style_text_line_space(ui->detail, 4, 0);
    }
    lv_obj_t *gesture_card = lv_obj_create(screen);
    lv_obj_remove_style_all(gesture_card);
    lv_obj_set_pos(gesture_card, 16, 365);
    lv_obj_set_size(gesture_card, 448, 103);
    lv_obj_set_style_bg_color(gesture_card, lv_color_hex(0x132433), 0);
    lv_obj_set_style_bg_opa(gesture_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(gesture_card, 14, 0);
    lv_obj_remove_flag(gesture_card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    label(gesture_card, "GESTURE RECOGNITION", &lv_font_montserrat_12, MUTED, 12, 8, 424);
    s_gesture_label = label(gesture_card, "WAIT FOR TWO VALID TARGETS",
                            &lv_font_montserrat_16, ACCENT, 12, 32, 424);
    for (int i = 0; i < GESTURE_LED_COUNT; ++i) {
        s_gesture_leds[i] = lv_obj_create(gesture_card);
        lv_obj_remove_style_all(s_gesture_leds[i]);
        lv_obj_set_pos(s_gesture_leds[i], 16 + i * 35, 72);
        lv_obj_set_size(s_gesture_leds[i], 31, 14);
        lv_obj_set_style_radius(s_gesture_leds[i], 5, 0);
        lv_obj_set_style_bg_color(s_gesture_leds[i], lv_color_hex(0x203344), 0);
        lv_obj_set_style_bg_opa(s_gesture_leds[i], LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_gesture_leds[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    s_gesture_timer = lv_timer_create(gesture_animation, 35, NULL);
    if (!s_gesture_timer) {
        bsp_display_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_timer_pause(s_gesture_timer);
    bsp_display_unlock();
    return ESP_OK;
}

void tof_ui_gesture(tof_gesture_event_t event, bool available, bool ready)
{
    if (!bsp_display_lock(-1)) return;
    if (event != TOF_GESTURE_NONE) {
        s_last_gesture = event;
        s_gesture_started = lv_tick_get();
        lv_label_set_text(s_gesture_label, event == TOF_GESTURE_RIGHT ?
                          "SWIPE RIGHT  >>>" : "<<<  SWIPE LEFT");
        lv_timer_reset(s_gesture_timer);
        lv_timer_resume(s_gesture_timer);
        gesture_animation(s_gesture_timer);
    } else if (s_last_gesture == TOF_GESTURE_NONE || lv_tick_elaps(s_gesture_started) > 2500) {
        lv_label_set_text(s_gesture_label, !available ? "WAIT FOR TWO VALID TARGETS" :
                          ready ? "READY - WAVE LEFT OR RIGHT" : "HOLD STILL - LEARNING BASELINE");
    }
    bsp_display_unlock();
}

void tof_ui_status(mosaico_module_mgr_slot_t slot, const char *message)
{
    slot_ui_t *ui = slot_ui(slot);
    if (!ui || !bsp_display_lock(-1)) return;
    lv_label_set_text(ui->distance, "--");
    lv_label_set_text(ui->status, message);
    lv_obj_set_style_text_color(ui->status, lv_color_hex(AMBER), 0);
    lv_label_set_text(ui->detail, "SIGNAL -- MCPS\nAMBIENT -- MCPS");
    lv_bar_set_value(ui->bar, 0, LV_ANIM_OFF);
    bsp_display_unlock();
}

void tof_ui_update(mosaico_module_mgr_slot_t slot, const vl53l0x_data_t *data)
{
    slot_ui_t *ui = slot_ui(slot);
    if (!ui || !bsp_display_lock(-1)) return;
    lv_label_set_text_fmt(ui->detail, "SIGNAL %.2f MCPS\nAMBIENT %.2f MCPS",
                          data->signal_rate_mcps, data->ambient_rate_mcps);
    lv_obj_set_style_text_color(ui->status, lv_color_hex(data->valid ? ACCENT : AMBER), 0);
    if (data->valid) {
        /* Keep unusually large five-digit readings inside the half-screen card. */
        int scale = data->distance_mm >= 10000 ? 256 : 384;
        lv_obj_set_style_transform_scale_x(ui->distance, scale, 0);
        lv_obj_set_style_transform_scale_y(ui->distance, scale, 0);
        lv_label_set_text_fmt(ui->distance, "%u", (unsigned)data->distance_mm);
        lv_label_set_text(ui->status, data->distance_mm > SCALE_MM ?
                          "BEYOND SCALE (> 2000 mm)" : "TARGET LOCKED");
        lv_bar_set_value(ui->bar, data->distance_mm > SCALE_MM ? SCALE_MM : data->distance_mm,
                         LV_ANIM_OFF);
    } else {
        lv_label_set_text(ui->distance, "--");
        lv_label_set_text_fmt(ui->status, "INVALID TARGET  /  STATUS %u", (unsigned)data->range_status);
        lv_bar_set_value(ui->bar, 0, LV_ANIM_OFF);
    }
    bsp_display_unlock();
}
