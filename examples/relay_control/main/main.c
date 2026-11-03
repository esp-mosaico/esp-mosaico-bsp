/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mosaico_module_mgr.h"
#include "mosaico_relay.h"

static const char *TAG = "relay_control";
static mosaico_relay_handle_t s_relay;

/* All UI objects and relay state are accessed under the BSP display lock. */
typedef struct {
    lv_obj_t *card;
    lv_obj_t *state;
    lv_obj_t *hint;
    lv_obj_t *track;
    lv_obj_t *thumb;
    lv_obj_t *meter;
} channel_ui_t;

static channel_ui_t s_channels[MOSAICO_RELAY_CHANNEL_COUNT];
static lv_obj_t *s_status;
static lv_obj_t *s_all;
static lv_obj_t *s_all_action;
static lv_obj_t *s_connection;
static lv_obj_t *s_connection_dot;
static lv_obj_t *s_connection_label;
static lv_obj_t *s_count;
static lv_obj_t *s_slot;
static lv_obj_t *s_all_hint;
static bool s_on[MOSAICO_RELAY_CHANNEL_COUNT];
static lv_style_transition_dsc_t s_transition;

enum {
    COLOR_BG = 0x0B1016,
    COLOR_SURFACE = 0x151D27,
    COLOR_BORDER = 0x293442,
    COLOR_TEXT = 0xEFF5FA,
    COLOR_MUTED = 0x8E9EAF,
    COLOR_ACCENT = 0x68E4BC,
    COLOR_AMBER = 0xE9B86D,
};

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y,
                       const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

/* Decorative objects never intercept a tap on the enclosing channel card. */
static lv_obj_t *surface(lv_obj_t *parent, int x, int y, int w, int h,
                         uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

static void thumb_position(void *obj, int32_t x)
{
    lv_obj_set_x(obj, x);
}

static void set_status(const char *message, bool error)
{
    lv_label_set_text(s_status, message);
    lv_obj_set_style_text_color(s_status, lv_color_hex(error ? COLOR_AMBER : COLOR_MUTED), 0);
}

static void refresh_ui(void)
{
    const bool connected = s_relay != NULL;
    unsigned active = 0;
    for (unsigned i = 0; i < MOSAICO_RELAY_CHANNEL_COUNT; ++i) {
        channel_ui_t *ui = &s_channels[i];
        const bool on = connected && s_on[i];
        const bool changed = lv_obj_has_state(ui->card, LV_STATE_CHECKED) != on;
        active += on;
        lv_obj_set_state(ui->card, LV_STATE_CHECKED, on);
        lv_obj_set_state(ui->card, LV_STATE_DISABLED, !connected);
        lv_obj_set_state(ui->track, LV_STATE_CHECKED, on);
        lv_label_set_text(ui->state, connected ? (on ? "ON" : "OFF") : "--");
        lv_obj_set_style_text_color(ui->state, lv_color_hex(on ? COLOR_ACCENT : COLOR_TEXT), 0);
        lv_label_set_text(ui->hint, connected ? (on ? "Tap to switch off" : "Tap to switch on") : "Unavailable");
        lv_obj_set_style_bg_color(ui->thumb, lv_color_hex(on ? COLOR_BG : COLOR_MUTED), 0);
        lv_obj_set_style_bg_color(ui->meter, lv_color_hex(on ? COLOR_ACCENT : COLOR_BORDER), 0);
        if (changed) {
            /* Restart from the current position, including during a quick second tap. */
            lv_obj_update_layout(ui->thumb);
            lv_anim_t animation;
            lv_anim_init(&animation);
            lv_anim_set_var(&animation, ui->thumb);
            lv_anim_set_exec_cb(&animation, thumb_position);
            lv_anim_set_values(&animation, lv_obj_get_x(ui->thumb), on ? 24 : 3);
            lv_anim_set_duration(&animation, 180);
            lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
            lv_anim_start(&animation);
        }
    }
    lv_label_set_text_fmt(s_count, "%02u / 04 ACTIVE", active);
    lv_obj_set_style_text_color(s_count, lv_color_hex(active ? COLOR_ACCENT : COLOR_TEXT), 0);
    lv_obj_set_style_bg_color(s_connection, lv_color_hex(connected ? 0x18342D : 0x322A20), 0);
    lv_obj_set_style_bg_color(s_connection_dot, lv_color_hex(connected ? COLOR_ACCENT : COLOR_AMBER), 0);
    lv_label_set_text(s_connection_label, connected ? "ONLINE" : "OFFLINE");
    lv_obj_set_style_text_color(s_connection_label, lv_color_hex(connected ? COLOR_ACCENT : COLOR_AMBER), 0);
    const bool all_on = active == MOSAICO_RELAY_CHANNEL_COUNT;
    lv_obj_set_state(s_all, LV_STATE_DISABLED, !connected);
    lv_obj_set_state(s_all, LV_STATE_CHECKED, all_on);
    lv_label_set_text(s_all_action, all_on ? "ALL OFF" : "ALL ON");
    lv_label_set_text(s_all_hint, connected ? "ALL CHANNELS" : "NO MODULE");
}

static void clicked(lv_event_t *event)
{
    if (!s_relay) {
        return;
    }
    lv_obj_t *target = lv_event_get_target_obj(event);
    esp_err_t ret = ESP_OK;
    bool enable_all = false;
    if (target == s_all) {
        /* A mixed state becomes all ON; only all ON becomes all OFF. */
        for (unsigned i = 0; i < MOSAICO_RELAY_CHANNEL_COUNT; ++i) {
            enable_all |= !s_on[i];
        }
        /* Track each successful write even if another channel fails. */
        for (unsigned i = 0; i < MOSAICO_RELAY_CHANNEL_COUNT; ++i) {
            esp_err_t result = mosaico_relay_set(s_relay, i, enable_all);
            if (result == ESP_OK) {
                s_on[i] = enable_all;
            } else {
                ret = result;
            }
        }
    } else {
        for (unsigned i = 0; i < MOSAICO_RELAY_CHANNEL_COUNT; ++i) {
            if (target == s_channels[i].card) {
                ret = mosaico_relay_set(s_relay, i, !s_on[i]);
                if (ret == ESP_OK) {
                    s_on[i] = !s_on[i];
                    ESP_LOGI(TAG, "Channel %02u -> %s", i + 1, s_on[i] ? "ON" : "OFF");
                    lv_label_set_text_fmt(s_status, "Channel %02u switched %s", i + 1, s_on[i] ? "on" : "off");
                    lv_obj_set_style_text_color(s_status, lv_color_hex(COLOR_MUTED), 0);
                }
                break;
            }
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Relay command failed: %s", esp_err_to_name(ret));
        set_status("Command failed. Check individual channels.", true);
    } else if (target == s_all) {
        set_status(enable_all ? "All channels switched on" : "All channels switched off", false);
        ESP_LOGI(TAG, "All channels -> %s", enable_all ? "ON" : "OFF");
    }
    refresh_ui();
}

static lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 18, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(COLOR_SURFACE), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_transition(obj, &s_transition, 0);
    lv_obj_set_style_transform_pivot_x(obj, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(obj, h / 2, 0);
    lv_obj_set_style_transform_scale(obj, 250, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(obj, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_opa(obj, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_add_event_cb(obj, clicked, LV_EVENT_CLICKED, NULL);
    return obj;
}

static void create_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(screen, 0, 0);
    static const lv_style_prop_t properties[] = {
        LV_STYLE_BG_COLOR, LV_STYLE_BORDER_COLOR,
        LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y, LV_STYLE_PROP_INV,
    };
    lv_style_transition_dsc_init(&s_transition, properties, lv_anim_path_ease_out, 140, 0, NULL);

    surface(screen, 24, 20, 4, 12, COLOR_ACCENT, 2);
    lv_obj_t *brand = label(screen, "ESP-MOSAICO", 36, 18, &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_style_text_letter_space(brand, 2, 0);
    label(screen, "Module Relay", 24, 40, &lv_font_montserrat_28, COLOR_TEXT);
    s_connection = surface(screen, 348, 42, 108, 28, 0x322A20, LV_RADIUS_CIRCLE);
    s_connection_dot = surface(s_connection, 12, 10, 8, 8, COLOR_AMBER, LV_RADIUS_CIRCLE);
    s_connection_label = label(s_connection, "OFFLINE", 28, 6, &lv_font_montserrat_14, COLOR_AMBER);
    s_status = label(screen, "Connect a relay module to get started", 24, 78, &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_width(s_status, 432);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_CLIP);

    lv_obj_t *summary = surface(screen, 24, 106, 432, 36, COLOR_SURFACE, 10);
    s_count = label(summary, "00 / 04 ACTIVE", 12, 10, &lv_font_montserrat_14, COLOR_TEXT);
    s_slot = label(summary, "NO MODULE", 190, 10, &lv_font_montserrat_14, COLOR_MUTED);
    for (unsigned i = 0; i < MOSAICO_RELAY_CHANNEL_COUNT; ++i) {
        channel_ui_t *ui = &s_channels[i];
        ui->meter = surface(summary, 324 + i * 24, 14, 18, 8, COLOR_BORDER, 3);
        ui->card = button(screen, 24 + (i % 2) * 222, 156 + (i / 2) * 126, 210, 114);
        lv_obj_set_style_bg_color(ui->card, lv_color_hex(0x17352E), LV_STATE_CHECKED);
        lv_obj_set_style_border_color(ui->card, lv_color_hex(0x3D8D74), LV_STATE_CHECKED);
        lv_obj_t *name = label(ui->card, "", 16, 14, &lv_font_montserrat_14, COLOR_MUTED);
        lv_label_set_text_fmt(name, "CHANNEL %02u", i + 1);
        lv_obj_set_style_text_letter_space(name, 1, 0);
        ui->state = label(ui->card, "OFF", 16, 40, &lv_font_montserrat_28, COLOR_TEXT);
        ui->hint = label(ui->card, "", 16, 86, &lv_font_montserrat_14, COLOR_MUTED);
        ui->track = surface(ui->card, 144, 46, 46, 26, 0x354250, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(ui->track, lv_color_hex(COLOR_ACCENT), LV_STATE_CHECKED);
        lv_obj_set_style_transition(ui->track, &s_transition, 0);
        ui->thumb = surface(ui->track, 3, 3, 20, 20, COLOR_MUTED, LV_RADIUS_CIRCLE);
    }
    s_all = button(screen, 24, 412, 432, 48);
    lv_obj_set_style_radius(s_all, 14, 0);
    lv_obj_set_style_bg_color(s_all, lv_color_hex(0x202B37), 0);
    lv_obj_set_style_bg_color(s_all, lv_color_hex(0x17352E), LV_STATE_CHECKED);
    lv_obj_set_style_border_color(s_all, lv_color_hex(0x3D8D74), LV_STATE_CHECKED);
    label(s_all, LV_SYMBOL_POWER, 16, 15, &lv_font_montserrat_16, COLOR_ACCENT);
    s_all_action = label(s_all, "ALL ON", 44, 15, &lv_font_montserrat_16, COLOR_TEXT);
    s_all_hint = label(s_all, "NO MODULE", 0, 0, &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_align(s_all_hint, LV_ALIGN_RIGHT_MID, -16, 0);
    refresh_ui();
    ESP_LOGI(TAG, "Relay console UI ready (480x480)");
}

void app_main(void)
{
    ESP_ERROR_CHECK(mosaico_module_mgr_init(NULL));
    if (!bsp_display_start()) {
        ESP_LOGE(TAG, "Display initialization failed");
        return;
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(80));
    if (!bsp_display_lock(-1)) {
        ESP_LOGE(TAG, "Display lock failed");
        return;
    }
    create_ui();
    bsp_display_unlock();
    const mosaico_module_mgr_claim_config_t claim = {
        .expected_type = MOSAICO_BOARD_TYPE_RELAY,
        .slot = MOSAICO_MODULE_MGR_SLOT_AUTO,
        .timeout_ms = 1000,
    };
    while (true) {
        mosaico_module_lease_t lease;
        esp_err_t ret = mosaico_module_mgr_claim(&claim, &lease);
        if (ret == ESP_ERR_TIMEOUT) {
            continue;
        }
        ESP_ERROR_CHECK(ret);
        mosaico_relay_handle_t relay = NULL;
        ret = mosaico_relay_new((bsp_subboard_slot_t)lease.slot, &relay);
        if (ret != ESP_OK) {
            ESP_ERROR_CHECK(mosaico_module_mgr_release(&lease));
            ESP_ERROR_CHECK(bsp_display_lock(-1) ? ESP_OK : ESP_ERR_TIMEOUT);
            set_status("Module initialization failed. Retrying...", true);
            bsp_display_unlock();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        /* The display lock serializes callbacks, handle lifetime, and UI state. */
        ESP_ERROR_CHECK(bsp_display_lock(-1) ? ESP_OK : ESP_ERR_TIMEOUT);
        s_relay = relay;
        lv_label_set_text(s_slot, lease.slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? "LEFT SLOT" : "RIGHT SLOT");
        set_status("Module ready. Tap a channel to switch.", false);
        refresh_ui();
        bsp_display_unlock();
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(100));
            mosaico_module_mgr_info_t info;
            ret = mosaico_module_mgr_get_info(lease.slot, &info);
            if (ret != ESP_OK || info.presence != MOSAICO_MODULE_PRESENCE_PRESENT ||
                info.descriptor_state != MOSAICO_MODULE_DESCRIPTOR_VALID || info.eeprom.board_type != MOSAICO_BOARD_TYPE_RELAY) {
                break;
            }
        }
        ESP_ERROR_CHECK(bsp_display_lock(-1) ? ESP_OK : ESP_ERR_TIMEOUT);
        ESP_ERROR_CHECK(mosaico_relay_del(s_relay));
        s_relay = NULL;
        for (unsigned i = 0; i < MOSAICO_RELAY_CHANNEL_COUNT; ++i) {
            s_on[i] = false;
        }
        lv_label_set_text(s_slot, "NO MODULE");
        set_status("Module disconnected. All channels off.", false);
        refresh_ui();
        bsp_display_unlock();
        ESP_ERROR_CHECK(mosaico_module_mgr_release(&lease));
        ESP_LOGI(TAG, "Module removed; outputs OFF");
    }
}
