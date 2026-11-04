/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mosaico_module_mgr.h"
#include "mosaico_module_tof.h"
#include "tof_ui.h"

static const char *TAG = "tof_distance";

static void scan_i2c_bus(i2c_master_bus_handle_t bus)
{
    unsigned found = 0;
    unsigned errors = 0;
    /* Skip the reserved 7-bit address ranges, including general call. */
    ESP_LOGI(TAG, "Scanning subboard I2C bus: 7-bit addresses 0x08-0x77");
    for (unsigned address = 0x08; address <= 0x77; ++address) {
        esp_err_t ret = i2c_master_probe(bus, address, 20);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", address);
            ++found;
        } else if (ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "I2C probe at 0x%02X failed: %s", address, esp_err_to_name(ret));
            ++errors;
        }
    }
    ESP_LOGI(TAG, "I2C scan complete: %u device(s), %u probe error(s)", found, errors);
}

static esp_err_t assign_slot_address(mosaico_tof_handle_t sensor, mosaico_module_mgr_slot_t slot)
{
    const uint8_t address = slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? 0x30 : 0x31;
    i2c_master_bus_handle_t bus = bsp_subboard_get_i2c_bus();
    /* Do not overwrite a device already responding at the destination address. */
    esp_err_t ret = i2c_master_probe(bus, address, 100);
    ESP_RETURN_ON_FALSE(ret == ESP_ERR_NOT_FOUND,
                        ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret,
                        TAG, "destination address 0x%02X is occupied or probe failed", address);
    ESP_RETURN_ON_ERROR(mosaico_tof_set_address(sensor, address), TAG, "assign slot address failed");
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, address, 100), TAG, "verify slot address failed");
    ESP_LOGI(TAG, "ToF address assigned: slot=%s I2C=0x%02X (7-bit)",
             mosaico_module_mgr_slot_to_name(slot), address);
    return ESP_OK;
}

static esp_err_t apply_distance_offset(mosaico_tof_handle_t sensor, mosaico_module_mgr_slot_t slot)
{
    /* Apply once after each hardware reset, preserving the initial device offset. */
    int32_t initial_um, applied_um;
    ESP_RETURN_ON_ERROR(mosaico_tof_get_offset_calibration(sensor, &initial_um), TAG, "read offset failed");
    const bool l4cd = strcmp(mosaico_tof_model_name(sensor), "VL53L4CD") == 0;
    const int adjustment = l4cd ?
        (slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? CONFIG_TOF_L4CD_LEFT_OFFSET_ADJUST_MM : CONFIG_TOF_L4CD_RIGHT_OFFSET_ADJUST_MM) :
        (slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? CONFIG_TOF_DISTANCE_OFFSET_ADJUST_MM : CONFIG_TOF_DISTANCE_RIGHT_OFFSET_ADJUST_MM);
    if (adjustment == 0) {
        return ESP_OK;
    }
    int32_t corrected_um = initial_um + adjustment * 1000;
    ESP_RETURN_ON_FALSE(corrected_um >= -512000 && corrected_um <= 511000,
                        ESP_ERR_INVALID_ARG, TAG, "adjusted offset outside sensor range");
    ESP_RETURN_ON_ERROR(mosaico_tof_set_offset_calibration(sensor, corrected_um), TAG, "write offset failed");
    ESP_RETURN_ON_ERROR(mosaico_tof_get_offset_calibration(sensor, &applied_um), TAG, "verify offset failed");
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
    mosaico_tof_handle_t sensor;
    bool ready;
    bool is_l0x;
    bool cleanup;
    TickType_t retry_at;
    mosaico_tof_data_t sample;
    uint32_t sample_ms;
    bool fresh;
    bool have_sample;
    uint32_t last_result_ms;
    uint32_t last_log_ms;
    unsigned reads, valid_reads;
    uint32_t read_total_ms, read_max_ms;
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
    if (state->sensor) {
        ESP_RETURN_ON_ERROR(mosaico_tof_close(state->sensor), TAG, "destroy failed");
        state->sensor = NULL;
    }
    ESP_RETURN_ON_ERROR(hold_shutdown(state->slot), TAG, "shutdown failed");
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
    state->fresh = state->have_sample = false;
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
        ret = mosaico_tof_open((bsp_subboard_slot_t)state->slot, &state->sensor);
        if (ret == ESP_OK) {
            ret = assign_slot_address(state->sensor, state->slot);
        }
        if (ret == ESP_OK) {
            ret = apply_distance_offset(state->sensor, state->slot);
        }
        if (ret == ESP_OK) {
            /* 20 ms per sensor; retain the driver's signal/sigma validity checks. */
            ret = mosaico_tof_set_timing_budget(state->sensor, 20000);
        }
        if (ret == ESP_OK) {
            ret = mosaico_tof_start_continuous(state->sensor);
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Initialize %s failed: %s",
                     mosaico_module_mgr_slot_to_name(state->slot), esp_err_to_name(ret));
            retire_slot(state, "RETRYING SENSOR");
            return;
        }
        state->ready = true;
        state->is_l0x = strcmp(mosaico_tof_model_name(state->sensor), "VL53L0X") == 0;
        state->last_result_ms = (uint32_t)(esp_timer_get_time() / 1000);
        tof_ui_set_sensor(state->slot, mosaico_tof_model_name(state->sensor));
    }
    mosaico_tof_data_t data = {0};
    uint32_t measure_started_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool sample_ready = false;
    esp_err_t ret = mosaico_tof_read_ready(state->sensor, &data, &sample_ready);
    uint32_t measure_finished_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (ret != ESP_OK || !tof_present(state->slot)) {
        retire_slot(state, "RECONNECTING");
        return;
    }
    if (!sample_ready) {
        if (measure_finished_ms - state->last_result_ms >= 500) {
            ESP_LOGW(TAG, "No new sample: slot=%s", mosaico_module_mgr_slot_to_name(state->slot));
            retire_slot(state, "RECONNECTING");
        }
        return;
    }
    state->last_result_ms = measure_finished_ms;
    state->sample = data;
    ++state->reads;
    if (data.valid) ++state->valid_reads;
    uint32_t read_ms = measure_finished_ms - measure_started_ms;
    state->read_total_ms += read_ms;
    if (read_ms > state->read_max_ms) state->read_max_ms = read_ms;
    /* Each sensor acquires independently; use its own data-ready observation. */
    state->sample_ms = data.timestamp_ms;
    state->have_sample = true;
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
    /* Scan on every startup, independently of module descriptors or presence. */
    scan_i2c_bus(bsp_subboard_get_i2c_bus());
    slot_state_t slots[] = {
        {.slot = MOSAICO_MODULE_MGR_SLOT_LEFT},
        {.slot = MOSAICO_MODULE_MGR_SLOT_RIGHT},
    };
    tof_gesture_t gesture;
    tof_gesture_init(&gesture, CONFIG_TOF_GESTURE_THRESHOLD_MM, CONFIG_TOF_GESTURE_WINDOW_MS);
    tof_hand_gesture_t hand;
    tof_hand_gesture_init(&hand, CONFIG_TOF_GESTURE_WINDOW_MS);
    bool have_gesture = false;
    uint32_t last_gesture_ms = 0;
    uint32_t last_ui_ms = 0;
    uint32_t rate_started_ms = (uint32_t)(esp_timer_get_time() / 1000);
    unsigned paired_frames = 0;
    unsigned valid_pairs_left = 0, valid_pairs_right = 0;
    bool right_first = false;
    uint32_t loop_max_ms = 0, ui_max_ms = 0;
    while (true) {
        uint32_t loop_started_ms = (uint32_t)(esp_timer_get_time() / 1000);
        /* Poll without waiting: both sensors are already acquiring concurrently. */
        service_slot(&slots[right_first ? 1 : 0]);
        service_slot(&slots[right_first ? 0 : 1]);
        right_first = !right_first;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool left_valid = slots[0].ready && slots[0].have_sample && slots[0].sample.valid &&
                          now_ms - slots[0].sample_ms <= 500;
        bool right_valid = slots[1].ready && slots[1].have_sample && slots[1].sample.valid &&
                           now_ms - slots[1].sample_ms <= 500;
        bool online = slots[0].ready && slots[1].ready;
        bool pair_fresh = online && slots[0].fresh && slots[1].fresh;
        tof_hand_input_t input[2];
        for (unsigned i = 0; i < 2; ++i) {
            input[i] = slots[i].ready && slots[i].have_sample && now_ms - slots[i].sample_ms <= 500 ?
                tof_hand_classify_model(slots[i].is_l0x, slots[i].sample.valid, slots[i].sample.distance_mm,
                                        slots[i].sample.range_status, CONFIG_TOF_GESTURE_NEAR_MM) : TOF_HAND_UNKNOWN;
        }
        bool available = online && input[0] != TOF_HAND_UNKNOWN && input[1] != TOF_HAND_UNKNOWN;
        bool cooling = have_gesture && now_ms - last_gesture_ms < 800U;
        tof_gesture_event_t event = TOF_GESTURE_NONE;
        if (!online || cooling) {
            tof_gesture_init(&gesture, CONFIG_TOF_GESTURE_THRESHOLD_MM, CONFIG_TOF_GESTURE_WINDOW_MS);
            tof_hand_gesture_init(&hand, CONFIG_TOF_GESTURE_WINDOW_MS);
        } else if (pair_fresh) {
            event = tof_hand_gesture_update(&hand, now_ms, input[0], slots[0].sample_ms,
                                           input[1], slots[1].sample_ms);
            /* Once the empty-field detector is armed, it owns the full gesture.
             * Do not learn the passing hand itself as a new background. */
            if (hand.ready || event != TOF_GESTURE_NONE) {
                tof_gesture_init(&gesture, CONFIG_TOF_GESTURE_THRESHOLD_MM, CONFIG_TOF_GESTURE_WINDOW_MS);
            } else {
                event = tof_gesture_update_timed(&gesture, now_ms,
                    left_valid, slots[0].sample.distance_mm, slots[0].sample_ms,
                    right_valid, slots[1].sample.distance_mm, slots[1].sample_ms);
            }
        }
        if (event != TOF_GESTURE_NONE) {
            have_gesture = true;
            last_gesture_ms = now_ms;
            tof_gesture_init(&gesture, CONFIG_TOF_GESTURE_THRESHOLD_MM, CONFIG_TOF_GESTURE_WINDOW_MS);
            tof_hand_gesture_init(&hand, CONFIG_TOF_GESTURE_WINDOW_MS);
        }
        /* Gesture processing consumes every pair; the dashboard only needs 10 Hz. */
        if (event != TOF_GESTURE_NONE || now_ms - last_ui_ms >= 100) {
            uint32_t ui_started_ms = (uint32_t)(esp_timer_get_time() / 1000);
            last_ui_ms = now_ms;
            for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
                if (slots[i].ready && slots[i].have_sample) {
                    tof_ui_update(slots[i].slot, &slots[i].sample);
                }
            }
            tof_ui_gesture(event, available, hand.ready || gesture.ready);
            uint32_t ui_ms = (uint32_t)(esp_timer_get_time() / 1000) - ui_started_ms;
            if (ui_ms > ui_max_ms) ui_max_ms = ui_ms;
        }
        if (event != TOF_GESTURE_NONE) {
            ESP_LOGI(TAG, "Gesture: SWIPE %s", event == TOF_GESTURE_RIGHT ? "RIGHT" : "LEFT");
        }
        if (pair_fresh) {
            ++paired_frames;
            if (left_valid) ++valid_pairs_left;
            if (right_valid) ++valid_pairs_right;
            /* A not-ready poll must never replay an old sample into detection. */
            slots[0].fresh = slots[1].fresh = false;
        }
        uint32_t rate_elapsed_ms = now_ms - rate_started_ms;
        uint32_t loop_ms = (uint32_t)(esp_timer_get_time() / 1000) - loop_started_ms;
        if (loop_ms > loop_max_ms) loop_max_ms = loop_ms;
        if (rate_elapsed_ms >= 5000) {
#if CONFIG_TOF_GESTURE_DIAGNOSTICS
            ESP_LOGI(TAG, "Sampling: %.1f pairs/s loop_max=%lu ms ui_max=%lu ms ready=%d mode=%s",
                     paired_frames * 1000.0 / rate_elapsed_ms,
                     (unsigned long)loop_max_ms, (unsigned long)ui_max_ms, hand.ready || gesture.ready,
                     hand.ready ? "near" : gesture.ready ? "baseline" : "learning");
            for (unsigned i = 0; i < 2; ++i) {
                ESP_LOGI(TAG, "Sampling %s: valid=%u/%u last=%u mm status=%u read_avg=%lu ms max=%lu ms",
                         mosaico_module_mgr_slot_to_name(slots[i].slot), slots[i].valid_reads, slots[i].reads,
                         (unsigned)slots[i].sample.distance_mm, (unsigned)slots[i].sample.range_status,
                         (unsigned long)(slots[i].reads ? slots[i].read_total_ms / slots[i].reads : 0),
                         (unsigned long)slots[i].read_max_ms);
            }
#endif
            ESP_LOGD(TAG, "Dual sampling: %.1f pairs/s (includes invalid results), budget=20 ms/sensor",
                     paired_frames * 1000.0 / rate_elapsed_ms);
            ESP_LOGD(TAG, "Sample validity: left=%u/%u right=%u/%u",
                     valid_pairs_left, paired_frames, valid_pairs_right, paired_frames);
            paired_frames = 0;
            valid_pairs_left = valid_pairs_right = 0;
            rate_started_ms = now_ms;
            loop_max_ms = ui_max_ms = 0;
            for (unsigned i = 0; i < 2; ++i) {
                slots[i].reads = slots[i].valid_reads = 0;
                slots[i].read_total_ms = slots[i].read_max_ms = 0;
            }
        }
        /* Yield at least 2 ms, independent of the sensors' integration budgets.
         * Idle discovery can sleep longer without affecting active sensors. */
        vTaskDelay(pdMS_TO_TICKS(slots[0].ready || slots[1].ready ? 2 : 100) + 1);
    }
}
