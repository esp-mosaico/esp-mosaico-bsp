/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include "bsp/esp_mosaico.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mosaico_module_interact.h"
#include <stdatomic.h>

static const char *TAG = "interaction_demo";

static lv_obj_t *s_subtitle;
static lv_obj_t *s_light_value;
static lv_obj_t *s_light_raw;
static lv_obj_t *s_light_bar;
static lv_obj_t *s_left_card;
static lv_obj_t *s_motion_card;
static lv_obj_t *s_right_card;
static lv_obj_t *s_ir_button;
static lv_obj_t *s_ir_label;
static atomic_bool s_ir_requested;

static void ir_button_clicked(lv_event_t *event)
{
    (void)event;
    atomic_store_explicit(&s_ir_requested, true, memory_order_relaxed);
}

static lv_obj_t *create_status_card(lv_obj_t *parent, const char *title, const char *hint, int32_t x)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 132, 112);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, x, -24);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x172033), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *indicator = lv_obj_create(card);
    lv_obj_set_size(indicator, 12, 12);
    lv_obj_align(indicator, LV_ALIGN_TOP_RIGHT, 0, 2);
    lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(indicator, lv_color_hex(0x475569), 0);
    lv_obj_set_style_border_width(indicator, 0, 0);

    lv_obj_t *title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xF8FAFC), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint_label = lv_label_create(card);
    lv_label_set_text(hint_label, hint);
    lv_obj_set_style_text_color(hint_label, lv_color_hex(0x64748B), 0);
    lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
    lv_obj_align(hint_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return card;
}

static void set_card_active(lv_obj_t *card, bool active, uint32_t color)
{
    lv_obj_t *indicator = lv_obj_get_child(card, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(active ? color : 0x172033), 0);
    lv_obj_set_style_bg_color(indicator, lv_color_hex(active ? 0xFFFFFF : 0x475569), 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(card, active ? 18 : 0, 0);
    lv_obj_set_style_shadow_opa(card, active ? LV_OPA_30 : LV_OPA_TRANSP, 0);
}

static void create_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x080D18), 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "INTERACTION");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF8FAFC), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 24);

    s_subtitle = lv_label_create(screen);
    lv_label_set_text(s_subtitle, "SEARCHING FOR BOARD");
    lv_obj_set_style_text_color(s_subtitle, lv_color_hex(0x64748B), 0);
    lv_obj_set_style_text_font(s_subtitle, &lv_font_montserrat_14, 0);
    lv_obj_align(s_subtitle, LV_ALIGN_TOP_LEFT, 28, 57);

    s_ir_button = lv_button_create(screen);
    lv_obj_set_size(s_ir_button, 116, 52);
    lv_obj_align(s_ir_button, LV_ALIGN_TOP_RIGHT, -28, 22);
    lv_obj_set_style_radius(s_ir_button, 18, 0);
    lv_obj_set_style_bg_color(s_ir_button, lv_color_hex(0x0891B2), 0);
    lv_obj_set_style_bg_color(s_ir_button, lv_color_hex(0x263247), LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_ir_button, ir_button_clicked, LV_EVENT_CLICKED, NULL);

    s_ir_label = lv_label_create(s_ir_button);
    lv_label_set_text(s_ir_label, "IR SEND");
    lv_obj_set_style_text_color(s_ir_label, lv_color_hex(0xF8FAFC), 0);
    lv_obj_set_style_text_font(s_ir_label, &lv_font_montserrat_14, 0);
    lv_obj_center(s_ir_label);

    lv_obj_t *light_card = lv_obj_create(screen);
    lv_obj_set_size(light_card, 424, 206);
    lv_obj_align(light_card, LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_set_style_radius(light_card, 28, 0);
    lv_obj_set_style_bg_color(light_card, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_color(light_card, lv_color_hex(0x263247), 0);
    lv_obj_set_style_border_width(light_card, 1, 0);
    lv_obj_set_style_pad_all(light_card, 24, 0);
    lv_obj_remove_flag(light_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *light_title = lv_label_create(light_card);
    lv_label_set_text(light_title, "AMBIENT LIGHT");
    lv_obj_set_style_text_color(light_title, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(light_title, &lv_font_montserrat_14, 0);
    lv_obj_align(light_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_light_value = lv_label_create(light_card);
    lv_label_set_text(s_light_value, "--%");
    lv_obj_set_style_text_color(s_light_value, lv_color_hex(0x67E8F9), 0);
    lv_obj_set_style_text_font(s_light_value, &lv_font_montserrat_28, 0);
    lv_obj_align(s_light_value, LV_ALIGN_LEFT_MID, 0, -12);

    s_light_raw = lv_label_create(light_card);
    lv_label_set_text(s_light_raw, "RAW  ----");
    lv_obj_set_style_text_color(s_light_raw, lv_color_hex(0x64748B), 0);
    lv_obj_set_style_text_font(s_light_raw, &lv_font_montserrat_14, 0);
    lv_obj_align(s_light_raw, LV_ALIGN_RIGHT_MID, 0, -8);

    s_light_bar = lv_bar_create(light_card);
    lv_obj_set_size(s_light_bar, 376, 10);
    lv_obj_align(s_light_bar, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_bar_set_range(s_light_bar, 0, 100);
    lv_bar_set_value(s_light_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_light_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_light_bar, lv_color_hex(0x263247), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_light_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_light_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_light_bar, lv_color_hex(0x22D3EE), LV_PART_INDICATOR);

    s_left_card = create_status_card(screen, "LEFT", "BUTTON", -146);
    s_motion_card = create_status_card(screen, "PIR", "MOTION", 0);
    s_right_card = create_status_card(screen, "RIGHT", "BUTTON", 146);
}

static void update_ui(const mosaico_interact_inputs_t *inputs)
{
    lv_label_set_text_fmt(s_light_value, "%u%%", inputs->light_level);
    lv_label_set_text_fmt(s_light_raw, "RAW  %d", inputs->light_raw);
    lv_bar_set_value(s_light_bar, inputs->light_level, LV_ANIM_ON);
    set_card_active(s_left_card, inputs->left_pressed, 0xC2410C);
    set_card_active(s_motion_card, inputs->motion_detected, 0x047857);
    set_card_active(s_right_card, inputs->right_pressed, 0x1D4ED8);
}

static void show_disconnected(void)
{
    atomic_store_explicit(&s_ir_requested, false, memory_order_relaxed);
    lv_label_set_text(s_subtitle, "WAITING FOR BOARD");
    lv_obj_set_style_text_color(s_subtitle, lv_color_hex(0x64748B), 0);
    lv_label_set_text(s_light_value, "--%");
    lv_label_set_text(s_light_raw, "RAW  ----");
    lv_bar_set_value(s_light_bar, 0, LV_ANIM_OFF);
    set_card_active(s_left_card, false, 0xC2410C);
    set_card_active(s_motion_card, false, 0x047857);
    set_card_active(s_right_card, false, 0x1D4ED8);
    lv_label_set_text(s_ir_label, "IR SEND");
    lv_obj_add_state(s_ir_button, LV_STATE_DISABLED);
}

static void show_connected(const mosaico_interact_info_t *info)
{
    lv_label_set_text_fmt(s_subtitle, "%s SLOT  /  %s INPUT", mosaico_module_mgr_slot_to_name(info->slot),
                          info->button_mode == MOSAICO_INTERACT_BUTTON_MODE_TOUCH ? "TOUCH" : "GPIO");
    lv_obj_set_style_text_color(s_subtitle, lv_color_hex(0x34D399), 0);
    lv_label_set_text(s_ir_label, "IR SEND");
    lv_obj_remove_state(s_ir_button, LV_STATE_DISABLED);
}

void app_main(void)
{
    bsp_display_config_t display_config = BSP_DISPLAY_DEFAULT_CONFIG();
    display_config.enable_touch = true;
    lv_display_t *display = bsp_display_start_with_config(&display_config);
    if (!display) {
        ESP_LOGE(TAG, "Display initialization failed");
        return;
    }

    ESP_ERROR_CHECK(bsp_display_lock(-1) ? ESP_OK : ESP_FAIL);
    create_ui();
    show_disconnected();
    bsp_display_unlock();

    mosaico_interact_handle_t interact = NULL;
    mosaico_interact_config_t config = MOSAICO_INTERACT_DEFAULT_CONFIG();
    mosaico_interact_info_t info = {0};
    mosaico_interact_inputs_t previous = {0};
    bool first_sample = true;
    bool ir_sent_visible = false;
    TickType_t ir_sent_at = 0;
    while (true) {
        if (!interact) {
            esp_err_t ret = mosaico_interact_open(&config, &interact);
            if (ret != ESP_OK) {
                if (interact) {
                    ESP_LOGE(TAG, "Interaction board cleanup is incomplete: %s", esp_err_to_name(ret));
                    ESP_ERROR_CHECK(mosaico_interact_close(interact));
                    interact = NULL;
                }
                if (ret != ESP_ERR_TIMEOUT && ret != ESP_ERR_NOT_FOUND) {
                    ESP_LOGE(TAG, "Interaction board initialization failed: %s", esp_err_to_name(ret));
                }
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }

            ret = mosaico_interact_get_info(interact, &info);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Read interaction board info failed: %s", esp_err_to_name(ret));
                ESP_ERROR_CHECK(mosaico_interact_close(interact));
                interact = NULL;
                continue;
            }
            if (bsp_display_lock(-1)) {
                show_connected(&info);
                bsp_display_unlock();
            }
            first_sample = true;
            ir_sent_visible = false;
        }

        mosaico_module_mgr_info_t module;
        esp_err_t ret = mosaico_module_mgr_get_info(info.slot, &module);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Read module state failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (module.presence == MOSAICO_MODULE_PRESENCE_ABSENT) {
            ret = mosaico_interact_close(interact);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Close removed interaction board failed: %s", esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            interact = NULL;
            if (bsp_display_lock(-1)) {
                show_disconnected();
                bsp_display_unlock();
            }
            ir_sent_visible = false;
            continue;
        }

        if (atomic_exchange_explicit(&s_ir_requested, false, memory_order_relaxed)) {
            ret = mosaico_interact_ir_send_nec(interact, 0x00, 0x10);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "IR transmission failed: %s", esp_err_to_name(ret));
            } else if (bsp_display_lock(-1)) {
                lv_label_set_text(s_ir_label, "IR SENT");
                bsp_display_unlock();
                ir_sent_at = xTaskGetTickCount();
                ir_sent_visible = true;
            }
        }
        if (ir_sent_visible && xTaskGetTickCount() - ir_sent_at >= pdMS_TO_TICKS(1000)) {
            if (bsp_display_lock(-1)) {
                lv_label_set_text(s_ir_label, "IR SEND");
                bsp_display_unlock();
                ir_sent_visible = false;
            }
        }

        mosaico_interact_inputs_t inputs;
        ret = mosaico_interact_read_inputs(interact, &inputs);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Input read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const bool digital_changed = first_sample || inputs.left_pressed != previous.left_pressed ||
                                     inputs.right_pressed != previous.right_pressed ||
                                     inputs.motion_detected != previous.motion_detected;
        if (digital_changed) {
            // Reflect the three digital inputs on the subboard LEDs.
            const mosaico_interact_rgb_t color = {
                .r = inputs.left_pressed ? 255 : 0,
                .g = inputs.motion_detected ? 255 : 0,
                .b = inputs.right_pressed ? 255 : 0,
            };
            ret = mosaico_interact_led_fill(interact, color);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "LED update failed: %s", esp_err_to_name(ret));
            }
        }

        if ((digital_changed || inputs.light_raw != previous.light_raw || inputs.light_level != previous.light_level) &&
            bsp_display_lock(-1)) {
            update_ui(&inputs);
            bsp_display_unlock();
        }
        previous = inputs;
        first_sample = false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
