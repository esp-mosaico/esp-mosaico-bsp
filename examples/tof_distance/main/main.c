/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_log.h"
#include "esp_check.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mosaico_module_mgr.h"
#include "mosaico_module_tof.h"
#include "tof_ui.h"

static const char *TAG = "tof_distance";

static esp_err_t assign_slot_address(vl53l0x_handle_t sensor, mosaico_module_mgr_slot_t slot)
{
    const uint8_t address = slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? 0x30 : 0x31;
    i2c_master_bus_handle_t bus = bsp_subboard_get_i2c_bus();
    /* Do not overwrite a device already responding at the destination address. */
    esp_err_t ret = i2c_master_probe(bus, address, 100);
    ESP_RETURN_ON_FALSE(ret == ESP_ERR_NOT_FOUND,
                        ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret,
                        TAG, "destination address 0x%02X is occupied or probe failed", address);
    ESP_RETURN_ON_ERROR(vl53l0x_set_address(sensor, bus, address), TAG, "assign slot address failed");
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, address, 100), TAG, "verify slot address failed");
    ESP_LOGI(TAG, "ToF address assigned: slot=%s I2C=0x%02X (7-bit)",
             mosaico_module_mgr_slot_to_name(slot), address);
    return ESP_OK;
}

static esp_err_t apply_distance_offset(vl53l0x_handle_t sensor, mosaico_module_mgr_slot_t slot)
{
    /* Apply once after each hardware reset, preserving the initial device offset. */
    int32_t initial_um, applied_um;
    ESP_RETURN_ON_ERROR(vl53l0x_get_offset_calibration(sensor, &initial_um), TAG, "read offset failed");
    const int adjustment = slot == MOSAICO_MODULE_MGR_SLOT_LEFT ?
        CONFIG_TOF_DISTANCE_OFFSET_ADJUST_MM : CONFIG_TOF_DISTANCE_RIGHT_OFFSET_ADJUST_MM;
    int32_t corrected_um = initial_um + adjustment * 1000;
    ESP_RETURN_ON_FALSE(corrected_um >= -512000 && corrected_um <= 511000,
                        ESP_ERR_INVALID_ARG, TAG, "adjusted offset outside sensor range");
    ESP_RETURN_ON_ERROR(vl53l0x_set_offset_calibration(sensor, corrected_um), TAG, "write offset failed");
    ESP_RETURN_ON_ERROR(vl53l0x_get_offset_calibration(sensor, &applied_um), TAG, "verify offset failed");
    ESP_RETURN_ON_FALSE(applied_um == corrected_um, ESP_FAIL, TAG, "offset readback mismatch");
    ESP_LOGI(TAG, "Distance offset: initial=%ld um adjustment=%d mm applied=%ld um",
             (long)initial_um, adjustment, (long)applied_um);
    return ESP_OK;
}

static bool tof_present(mosaico_module_mgr_slot_t slot)
{
    mosaico_module_mgr_info_t info;
    esp_err_t ret = mosaico_module_mgr_get_info(slot, &info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Read slot state failed: %s", esp_err_to_name(ret));
        return false;
    }
    return info.presence == MOSAICO_MODULE_PRESENCE_PRESENT &&
           info.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID &&
           info.eeprom.board_type == MOSAICO_BOARD_TYPE_TOF;
}

typedef struct {
    mosaico_module_mgr_slot_t slot;
    mosaico_module_lease_t lease;
    vl53l0x_handle_t sensor;
    bool ready;
    bool cleanup;
    TickType_t retry_at;
    vl53l0x_data_t sample;
    uint32_t sample_ms;
    bool fresh;
    uint32_t last_log_ms;
} slot_state_t;

static gpio_num_t xshut_pin(mosaico_module_mgr_slot_t slot)
{
    return bsp_subboard_map_gpio((bsp_subboard_slot_t)slot, GPIO_NUM_48);
}

/* Keep unused slots LOW, including while EEPROM discovery debounces insertion. */
static esp_err_t hold_shutdown(mosaico_module_mgr_slot_t slot)
{
    gpio_num_t pin = xshut_pin(slot);
    ESP_RETURN_ON_ERROR(gpio_hold_dis(pin), TAG, "release GPIO hold failed");
    ESP_RETURN_ON_ERROR(gpio_sleep_sel_dis(pin), TAG, "disable GPIO sleep selection failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, 0), TAG, "assert XSHUT failed");
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

/* Retry cleanup without blocking the healthy slot or surrendering its peer's lease. */
static esp_err_t close_slot(slot_state_t *state)
{
    ESP_RETURN_ON_ERROR(hold_shutdown(state->slot), TAG, "shutdown failed");
    if (state->sensor) {
        ESP_RETURN_ON_ERROR(vl53l0x_destroy(state->sensor), TAG, "destroy failed");
        state->sensor = NULL;
    }
    if (state->lease.id) {
        ESP_RETURN_ON_ERROR(mosaico_module_mgr_release(&state->lease), TAG, "release failed");
        state->lease = (mosaico_module_lease_t){0};
    }
    state->cleanup = false;
    return ESP_OK;
}

static void retire_slot(slot_state_t *state, const char *message)
{
    state->ready = false;
    state->cleanup = true;
    tof_ui_status(state->slot, message);
    esp_err_t ret = close_slot(state);
    if (ret != ESP_OK) {
        tof_ui_status(state->slot, "CLEANUP RETRY");
    }
    state->retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
}

static void service_slot(slot_state_t *state)
{
    if (state->ready && !tof_present(state->slot)) {
        retire_slot(state, "WAITING FOR MODULE");
        return;
    }
    if (!state->ready) {
        if ((int32_t)(xTaskGetTickCount() - state->retry_at) < 0) {
            return;
        }
        if (state->cleanup) {
            if (close_slot(state) != ESP_OK) {
                state->retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
                return;
            }
        }
        const mosaico_module_mgr_claim_config_t config = {
            .expected_type = MOSAICO_BOARD_TYPE_TOF,
            .slot = state->slot,
            .timeout_ms = 0,
        };
        esp_err_t ret = mosaico_module_mgr_claim(&config, &state->lease);
        if (ret != ESP_OK) {
            state->retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
            return;
        }
        tof_ui_status(state->slot, "INITIALIZING");
        /* Calls are serialized: the peer is either held LOW or already readdressed. */
        ret = mosaico_tof_new((bsp_subboard_slot_t)state->slot, &state->sensor);
        if (ret == ESP_OK) {
            ret = assign_slot_address(state->sensor, state->slot);
        }
        if (ret == ESP_OK) {
            ret = apply_distance_offset(state->sensor, state->slot);
        }
        if (ret == ESP_OK) {
            /* 20 ms per sensor; retain the driver's signal/sigma validity checks. */
            ret = vl53l0x_set_profile(state->sensor, VL53L0X_PROFILE_HIGH_SPEED);
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Initialize %s failed: %s",
                     mosaico_module_mgr_slot_to_name(state->slot), esp_err_to_name(ret));
            retire_slot(state, "RETRYING SENSOR");
            return;
        }
        state->ready = true;
    }
    vl53l0x_data_t data = {0};
    uint32_t measure_started_ms = (uint32_t)(esp_timer_get_time() / 1000);
    esp_err_t ret = vl53l0x_single_measure(state->sensor, &data);
    uint32_t measure_finished_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (ret != ESP_OK || !tof_present(state->slot)) {
        retire_slot(state, "RECONNECTING");
        return;
    }
    state->sample = data;
    /* Approximate the measurement's effective time by its acquisition midpoint. */
    state->sample_ms = measure_started_ms + (measure_finished_ms - measure_started_ms) / 2;
    state->fresh = true;
    if (data.valid && state->sample_ms - state->last_log_ms >= 1000) {
        state->last_log_ms = state->sample_ms;
        ESP_LOGI(TAG, "slot=%s distance=%u mm signal=%.2f MCPS ambient=%.2f MCPS",
                 mosaico_module_mgr_slot_to_name(state->slot),
                 (unsigned)data.distance_mm, data.signal_rate_mcps, data.ambient_rate_mcps);
    }
}

void app_main(void)
{
    /* Dedicated dual-ToF demo: reserve both XSHUT lines before starting discovery. */
    ESP_ERROR_CHECK(hold_shutdown(MOSAICO_MODULE_MGR_SLOT_LEFT));
    ESP_ERROR_CHECK(hold_shutdown(MOSAICO_MODULE_MGR_SLOT_RIGHT));
    ESP_ERROR_CHECK(tof_ui_init());
    ESP_ERROR_CHECK(mosaico_module_mgr_init(NULL));
    slot_state_t slots[] = {
        {.slot = MOSAICO_MODULE_MGR_SLOT_LEFT},
        {.slot = MOSAICO_MODULE_MGR_SLOT_RIGHT},
    };
    tof_gesture_t gesture;
    tof_gesture_init(&gesture, CONFIG_TOF_GESTURE_THRESHOLD_MM, CONFIG_TOF_GESTURE_WINDOW_MS);
    uint32_t last_ui_ms = 0;
    uint32_t rate_started_ms = (uint32_t)(esp_timer_get_time() / 1000);
    unsigned paired_frames = 0;
    unsigned valid_pairs_left = 0, valid_pairs_right = 0;
    bool right_first = false;
    while (true) {
        slots[0].fresh = slots[1].fresh = false;
        /* Balance acquisition phase instead of permanently favoring one side. */
        service_slot(&slots[right_first ? 1 : 0]);
        service_slot(&slots[right_first ? 0 : 1]);
        right_first = !right_first;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool left_valid = slots[0].ready && slots[0].fresh && slots[0].sample.valid &&
                          now_ms - slots[0].sample_ms <= 500;
        bool right_valid = slots[1].ready && slots[1].fresh && slots[1].sample.valid &&
                           now_ms - slots[1].sample_ms <= 500;
        tof_gesture_event_t event = tof_gesture_update_timed(&gesture, now_ms,
            left_valid, slots[0].sample.distance_mm, slots[0].sample_ms,
            right_valid, slots[1].sample.distance_mm, slots[1].sample_ms);
        /* Gesture processing consumes every pair; the dashboard only needs 10 Hz. */
        if (event != TOF_GESTURE_NONE || now_ms - last_ui_ms >= 100) {
            last_ui_ms = now_ms;
            for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
                if (slots[i].ready && slots[i].fresh) {
                    tof_ui_update(slots[i].slot, &slots[i].sample);
                }
            }
            tof_ui_gesture(event, left_valid && right_valid, gesture.ready);
        }
        if (event != TOF_GESTURE_NONE) {
            ESP_LOGI(TAG, "Gesture: SWIPE %s", event == TOF_GESTURE_RIGHT ? "RIGHT" : "LEFT");
        }
        if (slots[0].fresh && slots[1].fresh) {
            ++paired_frames;
            if (left_valid) ++valid_pairs_left;
            if (right_valid) ++valid_pairs_right;
        }
        uint32_t rate_elapsed_ms = now_ms - rate_started_ms;
        if (rate_elapsed_ms >= 5000) {
            ESP_LOGI(TAG, "Dual sampling: %.1f pairs/s (includes invalid results), budget=20 ms/sensor",
                     paired_frames * 1000.0 / rate_elapsed_ms);
            ESP_LOGI(TAG, "Sample validity: left=%u/%u right=%u/%u",
                     valid_pairs_left, paired_frames, valid_pairs_right, paired_frames);
            paired_frames = 0;
            valid_pairs_left = valid_pairs_right = 0;
            rate_started_ms = now_ms;
        }
        /* Yield one tick while sampling (10 ms at the current 100 Hz tick rate).
         * Idle discovery can sleep longer without affecting active sensors. */
        vTaskDelay(slots[0].ready || slots[1].ready ? 1 : pdMS_TO_TICKS(100));
    }
}
