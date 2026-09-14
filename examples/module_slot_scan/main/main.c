/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mosaico_module_mgr.h"
#include "slot_panes.h"

static const char *TAG = "module_slot_scan";

#define SCAN_PERIOD_MS 500U
#define SCAN_DEBOUNCE  3U

typedef struct {
    mosaico_module_mgr_event_t event;
    mosaico_module_mgr_slot_t slot;
    mosaico_board_type_t type;
    char name[32];
} slot_event_t;

static QueueHandle_t s_events;
static slot_pane_t *s_left;
static slot_pane_t *s_right;

static slot_pane_t *pane_for(mosaico_module_mgr_slot_t slot)
{
    return slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? s_left : s_right;
}

static void on_module_event(mosaico_module_mgr_event_t event,
                            const mosaico_module_mgr_info_t *info,
                            void *user_data)
{
    (void)user_data;
    if (info == NULL || s_events == NULL) {
        return;
    }
    if (event != MOSAICO_MODULE_MGR_EVENT_INSERTED &&
            event != MOSAICO_MODULE_MGR_EVENT_REMOVED) {
        return;
    }

    slot_event_t msg = {
        .event = event,
        .slot = info->slot,
        .type = (mosaico_board_type_t)info->eeprom.board_type,
    };
    memcpy(msg.name, info->eeprom.board_name, sizeof(msg.name) - 1);
    ESP_LOGI(TAG, "%s %s type=%s addr=0x%02X",
             event == MOSAICO_MODULE_MGR_EVENT_INSERTED ? "INSERTED" : "REMOVED",
             mosaico_module_mgr_slot_to_name(info->slot),
             mosaico_module_mgr_type_to_name(msg.type),
             info->eeprom_addr);
    (void)xQueueSend(s_events, &msg, 0);
}

static void apply_event(const slot_event_t *msg)
{
    slot_pane_t *pane = pane_for(msg->slot);
    if (msg->event == MOSAICO_MODULE_MGR_EVENT_REMOVED) {
        slot_pane_stop(pane);
        slot_pane_show_empty(pane);
        return;
    }

    if (msg->type == MOSAICO_BOARD_TYPE_CAMERA) {
        const esp_err_t ret = slot_pane_start_camera(pane);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Camera pane failed: %s", esp_err_to_name(ret));
        }
        return;
    }
    if (msg->type == MOSAICO_BOARD_TYPE_INTERACT) {
        const esp_err_t ret = slot_pane_start_interact(pane);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Interact pane failed: %s", esp_err_to_name(ret));
            slot_pane_show_other(pane, "Interaction");
        }
        return;
    }
    slot_pane_stop(pane);
    slot_pane_show_other(pane, mosaico_module_mgr_type_to_name(msg->type));
}

static void ui_task(void *arg)
{
    (void)arg;
    slot_event_t msg;
    while (true) {
        if (xQueueReceive(s_events, &msg, portMAX_DELAY) == pdTRUE) {
            apply_event(&msg);
        }
    }
}

void app_main(void)
{
    s_events = xQueueCreate(8, sizeof(slot_event_t));
    ESP_ERROR_CHECK(s_events ? ESP_OK : ESP_ERR_NO_MEM);

    lv_display_t *display = bsp_display_start();
    ESP_ERROR_CHECK(display ? ESP_OK : ESP_FAIL);
    if (bsp_display_lock(-1)) {
        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x020617), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        bsp_display_unlock();
    }
    slot_panes_create(lv_screen_active(), &s_left, &s_right);

    const mosaico_module_mgr_config_t config = {
        .scan_period_ms = SCAN_PERIOD_MS,
        .debounce_count = SCAN_DEBOUNCE,
    };
    ESP_ERROR_CHECK(mosaico_module_mgr_subscribe(on_module_event, NULL));
    ESP_ERROR_CHECK(mosaico_module_mgr_init(&config));
    ESP_LOGI(TAG, "Split view ready: left=0x50 right=0x51");

    ESP_ERROR_CHECK(xTaskCreate(ui_task, "slot_ui", 8192, NULL, 4, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
