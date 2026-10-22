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
static lv_obj_t *s_cards[MOSAICO_RELAY_CHANNEL_COUNT];
static lv_obj_t *s_labels[MOSAICO_RELAY_CHANNEL_COUNT];
static lv_obj_t *s_status;
static lv_obj_t *s_off;
static bool s_on[MOSAICO_RELAY_CHANNEL_COUNT];

static void refresh_ui(void)
{
    for (unsigned i = 0; i < MOSAICO_RELAY_CHANNEL_COUNT; ++i) {
        lv_label_set_text_fmt(s_labels[i], "CHANNEL %02u\n\n%s", i + 1, s_on[i] ? "ON" : "OFF");
        lv_obj_set_style_bg_color(s_cards[i], lv_color_hex(s_on[i] ? 0x12685D : 0x192A36), 0);
        lv_obj_set_state(s_cards[i], LV_STATE_DISABLED, s_relay == NULL);
    }
    lv_obj_set_state(s_off, LV_STATE_DISABLED, s_relay == NULL);
}

static void clicked(lv_event_t *event)
{
    if (!s_relay) {
        return;
    }
    lv_obj_t *target = lv_event_get_target_obj(event);
    esp_err_t ret = ESP_OK;
    if (target == s_off) {
        /* Track each successful write even if another channel fails. */
        for (unsigned i = 0; i < MOSAICO_RELAY_CHANNEL_COUNT; ++i) {
            esp_err_t result = mosaico_relay_set(s_relay, i, false);
            if (result == ESP_OK) {
                s_on[i] = false;
            } else {
                ret = result;
            }
        }
    } else {
        for (unsigned i = 0; i < MOSAICO_RELAY_CHANNEL_COUNT; ++i) {
            if (target == s_cards[i]) {
                ret = mosaico_relay_set(s_relay, i, !s_on[i]);
                if (ret == ESP_OK) {
                    s_on[i] = !s_on[i];
                }
                break;
            }
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Relay command failed: %s", esp_err_to_name(ret));
    }
    lv_label_set_text(s_status, ret == ESP_OK ? "Tap a channel to switch" : "Write failed - try ALL OFF");
    refresh_ui();
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, const lv_font_t *font)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xE6F3F1), 0);
    return obj;
}

static void create_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0C1821), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    label(screen, "RELAY / 04", 28, 24, &lv_font_montserrat_28);
    s_status = label(screen, "Insert a relay board", 28, 64, &lv_font_montserrat_16);
    for (unsigned i = 0; i < MOSAICO_RELAY_CHANNEL_COUNT; ++i) {
        lv_obj_t *card = lv_button_create(screen);
        s_cards[i] = card;
        lv_obj_set_pos(card, 28 + (i % 2) * 218, 106 + (i / 2) * 132);
        lv_obj_set_size(card, 206, 120);
        lv_obj_set_style_radius(card, 20, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_add_event_cb(card, clicked, LV_EVENT_CLICKED, NULL);
        s_labels[i] = label(card, "", 8, 4, &lv_font_montserrat_16);
    }
    s_off = lv_button_create(screen);
    lv_obj_set_pos(s_off, 28, 382);
    lv_obj_set_size(s_off, 424, 66);
    lv_obj_set_style_radius(s_off, 20, 0);
    lv_obj_set_style_bg_color(s_off, lv_color_hex(0x29434A), 0);
    lv_obj_set_style_shadow_width(s_off, 0, 0);
    lv_obj_add_event_cb(s_off, clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(s_off, "ALL OFF", 0, 0, &lv_font_montserrat_16));
    refresh_ui();
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
            lv_label_set_text(s_status, "Module init failed / Retrying");
            bsp_display_unlock();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        /* The display lock serializes callbacks, handle lifetime, and UI state. */
        ESP_ERROR_CHECK(bsp_display_lock(-1) ? ESP_OK : ESP_ERR_TIMEOUT);
        s_relay = relay;
        lv_label_set_text(s_status, lease.slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? "Left slot / Tap to switch" : "Right slot / Tap to switch");
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
        lv_label_set_text(s_status, "Insert a relay board");
        refresh_ui();
        bsp_display_unlock();
        ESP_ERROR_CHECK(mosaico_module_mgr_release(&lease));
        ESP_LOGI(TAG, "Module removed; outputs OFF");
    }
}
