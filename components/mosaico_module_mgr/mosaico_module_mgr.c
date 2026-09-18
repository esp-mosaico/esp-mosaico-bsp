/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_module_mgr.h"

#include <inttypes.h>
#include <string.h>

#include "bsp/subboard.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "mosaico_module_mgr";

#define EEPROM_DESC_CRC_OFFSET       0x34U
#define EEPROM_MFG_DATA_OFFSET       0x36U
#define EEPROM_MFG_CRC_OFFSET        0x3EU
#define EEPROM_PARAM_DATA_OFFSET     0x40U
#define EEPROM_PARAM_CRC_OFFSET      0x84U
#define EEPROM_I2C_FREQ_HZ           100000U
#define EEPROM_I2C_TIMEOUT_MS        100
#define EEPROM_DETACH_ATTEMPTS       5
#define EEPROM_DETACH_RETRY_MS       20
#define SCAN_TASK_STACK_SIZE         4096U
#define EVENT_TASK_STACK_SIZE        4096U
#define MANAGER_TASK_PRIORITY        5U
#define MODULE_SUBSCRIBER_COUNT      4U
#define CLAIM_WAITER_COUNT           4U
#define MANAGER_STOP_TIMEOUT_MS      2000U
#define ALL_CLAIM_FLAGS              (MOSAICO_MODULE_CLAIM_ALLOW_INVALID_DESCRIPTOR | \
                                      MOSAICO_MODULE_CLAIM_USE_SLOT_CONTROL_GPIO | \
                                      MOSAICO_MODULE_CLAIM_PROBE_WHILE_CLAIMED)

typedef enum {
    MANAGER_STOPPED = 0,
    MANAGER_STARTING,
    MANAGER_RUNNING,
    MANAGER_STOPPING,
} manager_lifecycle_t;

typedef struct {
    mosaico_module_mgr_info_t info;
    i2c_master_dev_handle_t eeprom;
    bool candidate_known;
    bool candidate_present;
    uint8_t candidate_count;
    TickType_t next_descriptor_tick;
    uint32_t scan_token;
    uint32_t lease_id;
    uint32_t claim_flags;
    uint32_t pending_changes;
    esp_err_t probe_error;
    esp_err_t descriptor_error;
    esp_err_t resource_error;
    bool rescan_requested;
} slot_state_t;

typedef struct {
    mosaico_module_mgr_event_callback_t callback;
    void *user_data;
    SemaphoreHandle_t completed;
    uint32_t generation;
    bool enabled;
    bool in_callback;
    bool unsubscribe_waiting;
} module_subscriber_t;

typedef struct {
    SemaphoreHandle_t lock;
    EventGroupHandle_t claim_events;
    SemaphoreHandle_t scan_stopped;
    SemaphoreHandle_t event_stopped;
    SemaphoreHandle_t waiters_idle;
    TaskHandle_t scan_task;
    TaskHandle_t event_task;
    mosaico_module_mgr_config_t config;
    slot_state_t slots[MOSAICO_MODULE_MGR_SLOT_COUNT];
    module_subscriber_t subscribers[MODULE_SUBSCRIBER_COUNT];
    uint32_t next_lease_id;
    uint32_t active_waiter_mask;
    bool scan_stop_confirmed;
    bool event_stop_confirmed;
} manager_context_t;

static manager_context_t s_manager;
static manager_lifecycle_t s_lifecycle;
static bool s_deinit_active;
static portMUX_TYPE s_lifecycle_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_sync_init_lock = portMUX_INITIALIZER_UNLOCKED;
/* Static synchronization objects survive shutdown retries and are never deleted. */
static StaticSemaphore_t s_state_lock_storage;
static StaticEventGroup_t s_claim_events_storage;
static StaticSemaphore_t s_scan_stopped_storage;
static StaticSemaphore_t s_event_stopped_storage;
static StaticSemaphore_t s_waiters_idle_storage;
static StaticSemaphore_t s_subscriber_completed_storage[MODULE_SUBSCRIBER_COUNT];

static manager_lifecycle_t lifecycle_get(void)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    manager_lifecycle_t lifecycle = s_lifecycle;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return lifecycle;
}

static void lifecycle_set(manager_lifecycle_t lifecycle)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_lifecycle = lifecycle;
    portEXIT_CRITICAL(&s_lifecycle_lock);
}

static bool lifecycle_start(void)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    bool start = s_lifecycle == MANAGER_STOPPED;
    if (start) {
        s_lifecycle = MANAGER_STARTING;
    }
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return start;
}

static bool lifecycle_acquire_stop(bool allow_start)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    bool acquired = !s_deinit_active &&
                    (s_lifecycle == MANAGER_STOPPING || (allow_start && s_lifecycle == MANAGER_RUNNING));
    if (acquired) {
        s_lifecycle = MANAGER_STOPPING;
        s_deinit_active = true;
    }
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return acquired;
}

static void lifecycle_release_stop(manager_lifecycle_t lifecycle)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_lifecycle = lifecycle;
    s_deinit_active = false;
    portEXIT_CRITICAL(&s_lifecycle_lock);
}

static esp_err_t ensure_sync_objects(void)
{
    portENTER_CRITICAL(&s_sync_init_lock);
    if (!s_manager.lock) {
        s_manager.lock = xSemaphoreCreateMutexStatic(&s_state_lock_storage);
        s_manager.claim_events = xEventGroupCreateStatic(&s_claim_events_storage);
        s_manager.scan_stopped = xSemaphoreCreateBinaryStatic(&s_scan_stopped_storage);
        s_manager.event_stopped = xSemaphoreCreateBinaryStatic(&s_event_stopped_storage);
        s_manager.waiters_idle = xSemaphoreCreateBinaryStatic(&s_waiters_idle_storage);
        for (size_t i = 0; i < MODULE_SUBSCRIBER_COUNT; ++i) {
            s_manager.subscribers[i].completed = xSemaphoreCreateBinaryStatic(&s_subscriber_completed_storage[i]);
        }
    }
    bool ready = s_manager.lock && s_manager.claim_events && s_manager.scan_stopped &&
                 s_manager.event_stopped && s_manager.waiters_idle;
    for (size_t i = 0; i < MODULE_SUBSCRIBER_COUNT; ++i) {
        ready = ready && s_manager.subscribers[i].completed;
    }
    portEXIT_CRITICAL(&s_sync_init_lock);
    if (!ready) {
        ESP_LOGE(TAG, "Create static synchronization objects failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static bool slot_is_valid(mosaico_module_mgr_slot_t slot)
{
    return slot >= MOSAICO_MODULE_MGR_SLOT_LEFT && slot < MOSAICO_MODULE_MGR_SLOT_COUNT;
}

static bool slot_request_is_valid(mosaico_module_mgr_slot_t slot)
{
    return slot == MOSAICO_MODULE_MGR_SLOT_AUTO || slot_is_valid(slot);
}

static bool config_equal(const mosaico_module_mgr_config_t *a, const mosaico_module_mgr_config_t *b)
{
    return a->scan_period_ms == b->scan_period_ms && a->descriptor_retry_ms == b->descriptor_retry_ms &&
           a->debounce_count == b->debounce_count;
}

static bool tick_reached(TickType_t now, TickType_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0xA001U) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

static esp_err_t parse_descriptor(const uint8_t raw[MOSAICO_MODULE_MGR_EEPROM_IMAGE_SIZE],
                                  mosaico_module_mgr_eeprom_v1_t *out)
{
    ESP_RETURN_ON_FALSE(raw && out, ESP_ERR_INVALID_ARG, TAG, "invalid descriptor input");
    if (memcmp(raw, MOSAICO_MODULE_MGR_EEPROM_MAGIC, MOSAICO_MODULE_MGR_EEPROM_MAGIC_LEN) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (read_le16(raw + EEPROM_DESC_CRC_OFFSET) != crc16(raw, EEPROM_DESC_CRC_OFFSET) ||
        read_le16(raw + EEPROM_MFG_CRC_OFFSET) !=
            crc16(raw + EEPROM_MFG_DATA_OFFSET, EEPROM_MFG_CRC_OFFSET - EEPROM_MFG_DATA_OFFSET) ||
        read_le16(raw + EEPROM_PARAM_CRC_OFFSET) !=
            crc16(raw + EEPROM_PARAM_DATA_OFFSET, EEPROM_PARAM_CRC_OFFSET - EEPROM_PARAM_DATA_OFFSET)) {
        return ESP_ERR_INVALID_CRC;
    }
    const uint16_t param_length = read_le16(raw + 0x42U);
    if (param_length > sizeof(out->param_data)) {
        return ESP_ERR_INVALID_SIZE;
    }

    mosaico_module_mgr_eeprom_v1_t parsed = {0};
    memcpy(parsed.magic, raw, sizeof(parsed.magic));
    parsed.board_type = raw[0x03U];
    parsed.board_id = read_le16(raw + 0x04U);
    parsed.hw_version = read_le16(raw + 0x06U);
    parsed.sw_version = read_le16(raw + 0x08U);
    parsed.vendor_id = read_le16(raw + 0x0AU);
    parsed.board_flags = read_le32(raw + 0x0CU);
    parsed.serial_number = read_le32(raw + 0x10U);
    memcpy(parsed.board_name, raw + 0x14U, sizeof(parsed.board_name));
    parsed.desc_crc16 = read_le16(raw + EEPROM_DESC_CRC_OFFSET);
    parsed.manufacture_date = read_le32(raw + EEPROM_MFG_DATA_OFFSET);
    parsed.batch_number = read_le16(raw + 0x3AU);
    parsed.factory_id = read_le16(raw + 0x3CU);
    parsed.mfg_crc16 = read_le16(raw + EEPROM_MFG_CRC_OFFSET);
    parsed.param_version = read_le16(raw + EEPROM_PARAM_DATA_OFFSET);
    parsed.param_length = param_length;
    memcpy(parsed.param_data, raw + 0x44U, sizeof(parsed.param_data));
    parsed.param_crc16 = read_le16(raw + EEPROM_PARAM_CRC_OFFSET);
    *out = parsed;
    return ESP_OK;
}

const char *mosaico_module_mgr_slot_to_name(mosaico_module_mgr_slot_t slot)
{
    switch (slot) {
    case MOSAICO_MODULE_MGR_SLOT_LEFT:
        return "left";
    case MOSAICO_MODULE_MGR_SLOT_RIGHT:
        return "right";
    case MOSAICO_MODULE_MGR_SLOT_AUTO:
        return "auto";
    default:
        return "unknown";
    }
}

const char *mosaico_module_mgr_type_to_name(mosaico_board_type_t type)
{
    switch (type) {
    case MOSAICO_BOARD_TYPE_CAMERA:
        return "Camera";
    case MOSAICO_BOARD_TYPE_SENSOR:
        return "Sensor";
    case MOSAICO_BOARD_TYPE_TOF:
        return "ToF";
    case MOSAICO_BOARD_TYPE_MATRIX_LED:
        return "Matrix LED";
    case MOSAICO_BOARD_TYPE_THERMAL:
        return "Thermal Camera";
    case MOSAICO_BOARD_TYPE_RELAY:
        return "Relay";
    case MOSAICO_BOARD_TYPE_INTERACT:
        return "Interaction";
    case MOSAICO_BOARD_TYPE_CORE:
        return "Core";
    case MOSAICO_BOARD_TYPE_POWER:
        return "Power";
    case MOSAICO_BOARD_TYPE_DOCK:
        return "Dock";
    case MOSAICO_BOARD_TYPE_HANDLE:
        return "Handle";
    case MOSAICO_BOARD_TYPE_BALANCE_CAR:
        return "Balance Car";
    case MOSAICO_BOARD_TYPE_DISPLAY:
        return "Display";
    case MOSAICO_BOARD_TYPE_IO_EXP:
        return "I/O Expansion";
    default:
        return "Unknown";
    }
}

static esp_err_t current_error_locked(const slot_state_t *state)
{
    if (state->resource_error != ESP_OK) {
        return state->resource_error;
    }
    if (state->probe_error != ESP_OK) {
        return state->probe_error;
    }
    return state->descriptor_error;
}

static uint32_t refresh_error_locked(slot_state_t *state)
{
    const esp_err_t error = current_error_locked(state);
    if (state->info.last_error == error) {
        return 0;
    }
    state->info.last_error = error;
    return MOSAICO_MODULE_CHANGE_ERROR;
}

static void mark_changed_locked(slot_state_t *state, uint32_t changes)
{
    if (changes == 0) {
        return;
    }
    state->info.generation++;
    state->pending_changes |= changes;
}

static void notify_manager_task(bool notify_event_task)
{
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    TaskHandle_t task = notify_event_task ? s_manager.event_task : s_manager.scan_task;
    if (task) {
        xTaskNotifyGive(task);
    }
    xSemaphoreGive(s_manager.lock);
}

static void signal_changes(uint32_t waiter_mask, bool event_pending)
{
    if (waiter_mask != 0) {
        xEventGroupSetBits(s_manager.claim_events, waiter_mask);
    }
    if (event_pending) {
        notify_manager_task(true);
    }
}

static bool slot_probe_allowed_locked(const slot_state_t *state)
{
    if (state->info.owner_state == MOSAICO_MODULE_OWNER_RESTORING) {
        return false;
    }
    return state->info.owner_state != MOSAICO_MODULE_OWNER_CLAIMED ||
           !(state->claim_flags & MOSAICO_MODULE_CLAIM_USE_SLOT_CONTROL_GPIO) ||
           (state->claim_flags & MOSAICO_MODULE_CLAIM_PROBE_WHILE_CLAIMED);
}

static esp_err_t attach_eeprom_devices(void)
{
    i2c_master_bus_handle_t bus = bsp_subboard_get_i2c_bus();
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_STATE, TAG, "subboard I2C bus is not initialized");
    for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        bsp_subboard_slot_config_t slot_config = {0};
        ESP_RETURN_ON_ERROR(bsp_subboard_get_slot_config((bsp_subboard_slot_t)i, &slot_config), TAG,
                            "read slot %u configuration failed", (unsigned)i);
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = slot_config.eeprom_addr,
            .scl_speed_hz = EEPROM_I2C_FREQ_HZ,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &s_manager.slots[i].eeprom), TAG,
                            "attach EEPROM 0x%02X failed", slot_config.eeprom_addr);
    }
    return ESP_OK;
}

static esp_err_t detach_eeprom_devices(void)
{
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        i2c_master_dev_handle_t device = s_manager.slots[i].eeprom;
        if (!device) {
            continue;
        }
        esp_err_t ret = ESP_ERR_INVALID_STATE;
        for (int attempt = 0; attempt < EEPROM_DETACH_ATTEMPTS && ret != ESP_OK; ++attempt) {
            ret = i2c_master_bus_rm_device(device);
            if (ret != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(EEPROM_DETACH_RETRY_MS));
            }
        }
        if (ret == ESP_OK) {
            s_manager.slots[i].eeprom = NULL;
        } else {
            ESP_LOGE(TAG, "Detach %s EEPROM failed: %s",
                     mosaico_module_mgr_slot_to_name((mosaico_module_mgr_slot_t)i), esp_err_to_name(ret));
            result = ret;
        }
    }
    return result;
}

static esp_err_t read_eeprom(i2c_master_dev_handle_t device, uint8_t raw[MOSAICO_MODULE_MGR_EEPROM_IMAGE_SIZE])
{
    const uint8_t address = 0;
    return i2c_master_transmit_receive(device, &address, sizeof(address), raw,
                                       MOSAICO_MODULE_MGR_EEPROM_IMAGE_SIZE, EEPROM_I2C_TIMEOUT_MS);
}

static void log_scan_change(mosaico_module_mgr_slot_t slot, uint32_t changes,
                            const mosaico_module_mgr_info_t *info, esp_err_t previous_error)
{
    if (changes & MOSAICO_MODULE_CHANGE_PRESENCE) {
        if (info->presence == MOSAICO_MODULE_PRESENCE_PRESENT) {
            ESP_LOGI(TAG, "Module present: slot=%s eeprom=0x%02X",
                     mosaico_module_mgr_slot_to_name(slot), info->eeprom_addr);
        } else if (info->presence == MOSAICO_MODULE_PRESENCE_ABSENT) {
            ESP_LOGI(TAG, "Module absent: slot=%s eeprom=0x%02X",
                     mosaico_module_mgr_slot_to_name(slot), info->eeprom_addr);
        }
    }
    if ((changes & MOSAICO_MODULE_CHANGE_DESCRIPTOR) &&
        info->descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID) {
        ESP_LOGI(TAG, "Module identified: slot=%s type=%s(0x%02X) id=0x%04X name=%.32s",
                 mosaico_module_mgr_slot_to_name(slot),
                 mosaico_module_mgr_type_to_name((mosaico_board_type_t)info->eeprom.board_type),
                 info->eeprom.board_type, info->eeprom.board_id, info->eeprom.board_name);
    } else if ((changes & MOSAICO_MODULE_CHANGE_DESCRIPTOR) &&
               info->descriptor_state == MOSAICO_MODULE_DESCRIPTOR_INVALID) {
        ESP_LOGW(TAG, "Invalid module descriptor: slot=%s error=%s",
                 mosaico_module_mgr_slot_to_name(slot), esp_err_to_name(info->last_error));
    }
    if ((changes & MOSAICO_MODULE_CHANGE_ERROR) && info->last_error != ESP_OK &&
        info->descriptor_state != MOSAICO_MODULE_DESCRIPTOR_INVALID) {
        ESP_LOGW(TAG, "Module scan error: slot=%s error=%s",
                 mosaico_module_mgr_slot_to_name(slot), esp_err_to_name(info->last_error));
    } else if ((changes & MOSAICO_MODULE_CHANGE_ERROR) && info->last_error == ESP_OK &&
               previous_error != ESP_OK) {
        ESP_LOGI(TAG, "Module scan recovered: slot=%s", mosaico_module_mgr_slot_to_name(slot));
    }
}

static void scan_slot(mosaico_module_mgr_slot_t slot)
{
    i2c_master_dev_handle_t device = NULL;
    uint8_t eeprom_addr = 0;
    uint32_t token = 0;
    bool forced = false;

    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    slot_state_t *state = &s_manager.slots[slot];
    if (lifecycle_get() != MANAGER_RUNNING || !slot_probe_allowed_locked(state)) {
        xSemaphoreGive(s_manager.lock);
        return;
    }
    device = state->eeprom;
    eeprom_addr = state->info.eeprom_addr;
    token = state->scan_token;
    forced = state->rescan_requested;
    state->rescan_requested = false;
    xSemaphoreGive(s_manager.lock);
    if (!device) {
        return;
    }

    const esp_err_t probe_ret = i2c_master_probe(bsp_subboard_get_i2c_bus(), eeprom_addr, EEPROM_I2C_TIMEOUT_MS);
    const bool probe_ok = probe_ret == ESP_OK || probe_ret == ESP_ERR_NOT_FOUND;
    const bool present = probe_ret == ESP_OK;
    const TickType_t now = xTaskGetTickCount();
    uint32_t changes = 0;
    uint32_t waiter_mask = 0;
    bool read_descriptor_now = false;
    bool silent_update = false;
    esp_err_t previous_error = ESP_OK;
    mosaico_module_mgr_info_t snapshot = {0};

    /* Reject I2C results if ownership or an explicit rescan changed the scan token. */
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    state = &s_manager.slots[slot];
    if (lifecycle_get() != MANAGER_RUNNING || token != state->scan_token || !slot_probe_allowed_locked(state)) {
        xSemaphoreGive(s_manager.lock);
        return;
    }
    previous_error = state->info.last_error;
    if (!probe_ok) {
        state->probe_error = probe_ret;
        changes |= refresh_error_locked(state);
    } else {
        state->probe_error = ESP_OK;
        if (!state->candidate_known || state->candidate_present != present) {
            state->candidate_known = true;
            state->candidate_present = present;
            state->candidate_count = 1;
        } else if (state->candidate_count < s_manager.config.debounce_count) {
            state->candidate_count++;
        }

        if (state->candidate_count >= s_manager.config.debounce_count) {
            const mosaico_module_presence_t stable =
                present ? MOSAICO_MODULE_PRESENCE_PRESENT : MOSAICO_MODULE_PRESENCE_ABSENT;
            if (state->info.presence != stable) {
                const bool initial_absent = state->info.presence == MOSAICO_MODULE_PRESENCE_UNKNOWN &&
                                            stable == MOSAICO_MODULE_PRESENCE_ABSENT;
                state->info.presence = stable;
                state->info.descriptor_state = MOSAICO_MODULE_DESCRIPTOR_UNKNOWN;
                if (stable == MOSAICO_MODULE_PRESENCE_PRESENT) {
                    memset(&state->info.eeprom, 0, sizeof(state->info.eeprom));
                }
                state->descriptor_error = ESP_OK;
                state->next_descriptor_tick = now;
                if (!initial_absent) {
                    changes |= MOSAICO_MODULE_CHANGE_PRESENCE | MOSAICO_MODULE_CHANGE_DESCRIPTOR;
                } else {
                    silent_update = true;
                }
            }
        }
        read_descriptor_now = state->info.presence == MOSAICO_MODULE_PRESENCE_PRESENT &&
                              (forced || ((state->descriptor_error != ESP_OK ||
                                           state->info.descriptor_state != MOSAICO_MODULE_DESCRIPTOR_VALID) &&
                                          tick_reached(now, state->next_descriptor_tick)));
        changes |= refresh_error_locked(state);
    }
    if (changes != 0) {
        mark_changed_locked(state, changes);
    } else if (silent_update) {
        state->info.generation++;
    }
    if (changes != 0) {
        snapshot = state->info;
        waiter_mask = s_manager.active_waiter_mask;
    }
    xSemaphoreGive(s_manager.lock);
    if (changes != 0) {
        log_scan_change(slot, changes, &snapshot, previous_error);
        signal_changes(waiter_mask, true);
    }
    if (!probe_ok || !read_descriptor_now) {
        return;
    }

    uint8_t raw[MOSAICO_MODULE_MGR_EEPROM_IMAGE_SIZE] = {0};
    mosaico_module_mgr_eeprom_v1_t descriptor = {0};
    const esp_err_t read_ret = read_eeprom(device, raw);
    const esp_err_t descriptor_ret = read_ret == ESP_OK ? parse_descriptor(raw, &descriptor) : read_ret;
    changes = 0;
    waiter_mask = 0;
    previous_error = ESP_OK;

    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    state = &s_manager.slots[slot];
    if (lifecycle_get() != MANAGER_RUNNING || token != state->scan_token ||
        state->info.presence != MOSAICO_MODULE_PRESENCE_PRESENT || !slot_probe_allowed_locked(state)) {
        xSemaphoreGive(s_manager.lock);
        return;
    }
    previous_error = state->info.last_error;
    state->next_descriptor_tick = now + pdMS_TO_TICKS(s_manager.config.descriptor_retry_ms);
    if (descriptor_ret == ESP_OK) {
        if (state->info.descriptor_state != MOSAICO_MODULE_DESCRIPTOR_VALID ||
            memcmp(&state->info.eeprom, &descriptor, sizeof(descriptor)) != 0) {
            state->info.eeprom = descriptor;
            state->info.descriptor_state = MOSAICO_MODULE_DESCRIPTOR_VALID;
            changes |= MOSAICO_MODULE_CHANGE_DESCRIPTOR;
        }
        state->descriptor_error = ESP_OK;
    } else {
        if (read_ret == ESP_OK && state->info.descriptor_state != MOSAICO_MODULE_DESCRIPTOR_INVALID) {
            state->info.descriptor_state = MOSAICO_MODULE_DESCRIPTOR_INVALID;
            memset(&state->info.eeprom, 0, sizeof(state->info.eeprom));
            changes |= MOSAICO_MODULE_CHANGE_DESCRIPTOR;
        }
        state->descriptor_error = descriptor_ret;
    }
    changes |= refresh_error_locked(state);
    mark_changed_locked(state, changes);
    if (changes != 0) {
        snapshot = state->info;
        waiter_mask = s_manager.active_waiter_mask;
    }
    xSemaphoreGive(s_manager.lock);
    if (changes != 0) {
        log_scan_change(slot, changes, &snapshot, previous_error);
        signal_changes(waiter_mask, true);
    }
}

static bool take_pending_event(mosaico_module_mgr_event_t *out_event)
{
    /* Per-slot dirty bits coalesce bursts into the latest consistent snapshot. */
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        slot_state_t *state = &s_manager.slots[i];
        if (state->pending_changes != 0) {
            *out_event = (mosaico_module_mgr_event_t) {
                .changes = state->pending_changes,
                .info = state->info,
            };
            state->pending_changes = 0;
            xSemaphoreGive(s_manager.lock);
            return true;
        }
    }
    xSemaphoreGive(s_manager.lock);
    return false;
}

static void dispatch_event(const mosaico_module_mgr_event_t *event)
{
    mosaico_module_subscription_t targets[MODULE_SUBSCRIBER_COUNT];
    size_t target_count = 0;
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    for (size_t i = 0; i < MODULE_SUBSCRIBER_COUNT; ++i) {
        const module_subscriber_t *subscriber = &s_manager.subscribers[i];
        if (subscriber->enabled) {
            targets[target_count++] = (mosaico_module_subscription_t) {
                .index = i,
                .generation = subscriber->generation,
            };
        }
    }
    xSemaphoreGive(s_manager.lock);

    for (size_t i = 0; i < target_count; ++i) {
        const mosaico_module_subscription_t target = targets[i];
        mosaico_module_mgr_event_callback_t callback = NULL;
        void *user_data = NULL;
        xSemaphoreTake(s_manager.lock, portMAX_DELAY);
        module_subscriber_t *subscriber = &s_manager.subscribers[target.index];
        if (subscriber->enabled && subscriber->generation == target.generation) {
            subscriber->in_callback = true;
            callback = subscriber->callback;
            user_data = subscriber->user_data;
        }
        xSemaphoreGive(s_manager.lock);
        if (!callback) {
            continue;
        }

        /* Callbacks run unlocked and may call manager APIs, including self-unsubscribe. */
        callback(event, user_data);

        bool notify_unsubscribe = false;
        xSemaphoreTake(s_manager.lock, portMAX_DELAY);
        subscriber = &s_manager.subscribers[target.index];
        if (subscriber->generation == target.generation) {
            subscriber->in_callback = false;
            notify_unsubscribe = subscriber->unsubscribe_waiting;
            subscriber->unsubscribe_waiting = false;
            if (!subscriber->enabled) {
                subscriber->callback = NULL;
                subscriber->user_data = NULL;
            }
        }
        xSemaphoreGive(s_manager.lock);
        if (notify_unsubscribe) {
            xSemaphoreGive(subscriber->completed);
        }
    }
}

static void event_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (true) {
        mosaico_module_mgr_event_t event = {0};
        while (take_pending_event(&event)) {
            dispatch_event(&event);
        }
        if (lifecycle_get() == MANAGER_STOPPING) {
            break;
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    s_manager.event_task = NULL;
    xSemaphoreGive(s_manager.lock);
    xSemaphoreGive(s_manager.event_stopped);
    vTaskDelete(NULL);
}

static void scan_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    TickType_t next_period = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(s_manager.config.scan_period_ms);

    while (lifecycle_get() == MANAGER_RUNNING) {
        uint32_t scan_mask = 0;
        TickType_t now = xTaskGetTickCount();
        if (tick_reached(now, next_period)) {
            scan_mask = (1U << MOSAICO_MODULE_MGR_SLOT_COUNT) - 1U;
            do {
                next_period += period;
            } while (tick_reached(now, next_period));
        }

        xSemaphoreTake(s_manager.lock, portMAX_DELAY);
        for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
            if (s_manager.slots[i].rescan_requested && slot_probe_allowed_locked(&s_manager.slots[i])) {
                scan_mask |= 1U << i;
            }
        }
        xSemaphoreGive(s_manager.lock);

        if (scan_mask != 0) {
            for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
                if (scan_mask & (1U << i)) {
                    scan_slot((mosaico_module_mgr_slot_t)i);
                }
            }
            continue;
        }

        now = xTaskGetTickCount();
        const TickType_t wait = tick_reached(now, next_period) ? 0 : next_period - now;
        ulTaskNotifyTake(pdTRUE, wait);
    }

    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    s_manager.scan_task = NULL;
    xSemaphoreGive(s_manager.lock);
    xSemaphoreGive(s_manager.scan_stopped);
    vTaskDelete(NULL);
}

static void reset_runtime_locked(void)
{
    xEventGroupClearBits(s_manager.claim_events, (1U << CLAIM_WAITER_COUNT) - 1U);
    while (xSemaphoreTake(s_manager.scan_stopped, 0) == pdTRUE) {
    }
    while (xSemaphoreTake(s_manager.event_stopped, 0) == pdTRUE) {
    }
    while (xSemaphoreTake(s_manager.waiters_idle, 0) == pdTRUE) {
    }
    s_manager.active_waiter_mask = 0;
    s_manager.scan_task = NULL;
    s_manager.event_task = NULL;
    s_manager.scan_stop_confirmed = false;
    s_manager.event_stop_confirmed = false;
    for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        i2c_master_dev_handle_t eeprom = s_manager.slots[i].eeprom;
        s_manager.slots[i] = (slot_state_t) {
            .eeprom = eeprom,
            .info = {
                .slot = (mosaico_module_mgr_slot_t)i,
                .presence = MOSAICO_MODULE_PRESENCE_UNKNOWN,
                .descriptor_state = MOSAICO_MODULE_DESCRIPTOR_UNKNOWN,
                .owner_state = MOSAICO_MODULE_OWNER_FREE,
            },
            .rescan_requested = true,
        };
        (void)bsp_subboard_eeprom_addr_from_slot((bsp_subboard_slot_t)i,
                                                 &s_manager.slots[i].info.eeprom_addr);
    }
    for (size_t i = 0; i < MODULE_SUBSCRIBER_COUNT; ++i) {
        module_subscriber_t *subscriber = &s_manager.subscribers[i];
        subscriber->callback = NULL;
        subscriber->user_data = NULL;
        subscriber->enabled = false;
        subscriber->in_callback = false;
        subscriber->unsubscribe_waiting = false;
        subscriber->generation++;
        if (subscriber->generation == 0) {
            subscriber->generation = 1;
        }
        while (xSemaphoreTake(subscriber->completed, 0) == pdTRUE) {
        }
    }
}

esp_err_t mosaico_module_mgr_init(const mosaico_module_mgr_config_t *config)
{
    ESP_RETURN_ON_ERROR(ensure_sync_objects(), TAG, "initialize synchronization failed");
    const mosaico_module_mgr_config_t active =
        config ? *config : (mosaico_module_mgr_config_t)MOSAICO_MODULE_MGR_DEFAULT_CONFIG();
    ESP_RETURN_ON_FALSE(active.scan_period_ms > 0 && active.descriptor_retry_ms > 0 && active.debounce_count > 0 &&
                            pdMS_TO_TICKS(active.scan_period_ms) > 0 && pdMS_TO_TICKS(active.descriptor_retry_ms) > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid manager configuration");

    if (lifecycle_get() == MANAGER_RUNNING) {
        xSemaphoreTake(s_manager.lock, portMAX_DELAY);
        const bool running = lifecycle_get() == MANAGER_RUNNING;
        const bool same = !config || config_equal(&active, &s_manager.config);
        xSemaphoreGive(s_manager.lock);
        return running && same ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(lifecycle_start(), ESP_ERR_INVALID_STATE, TAG, "module manager is busy");

    esp_err_t ret = detach_eeprom_devices();
    if (ret != ESP_OK) {
        lifecycle_set(MANAGER_STOPPED);
        return ret;
    }
    ret = bsp_subboard_init();
    if (ret != ESP_OK) {
        lifecycle_set(MANAGER_STOPPED);
        ESP_LOGE(TAG, "Initialize BSP subboard failed: %s", esp_err_to_name(ret));
        return ret;
    }

    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    s_manager.config = active;
    reset_runtime_locked();
    xSemaphoreGive(s_manager.lock);

    ret = attach_eeprom_devices();
    if (ret != ESP_OK) {
        (void)detach_eeprom_devices();
        lifecycle_set(MANAGER_STOPPED);
        return ret;
    }
    if (xTaskCreate(event_task, "module_event", EVENT_TASK_STACK_SIZE, NULL, MANAGER_TASK_PRIORITY,
                    &s_manager.event_task) != pdPASS) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    if (xTaskCreate(scan_task, "module_scan", SCAN_TASK_STACK_SIZE, NULL, MANAGER_TASK_PRIORITY,
                    &s_manager.scan_task) != pdPASS) {
        xSemaphoreTake(s_manager.lock, portMAX_DELAY);
        s_manager.scan_stop_confirmed = true;
        xSemaphoreGive(s_manager.lock);
        lifecycle_set(MANAGER_STOPPING);
        xTaskNotifyGive(s_manager.event_task);
        if (xSemaphoreTake(s_manager.event_stopped, pdMS_TO_TICKS(MANAGER_STOP_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "Event task rollback timed out");
            return ESP_ERR_TIMEOUT;
        }
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    lifecycle_set(MANAGER_RUNNING);
    xTaskNotifyGive(s_manager.event_task);
    xTaskNotifyGive(s_manager.scan_task);
    ESP_LOGI(TAG, "Module manager started: period=%" PRIu32 " ms debounce=%u",
             active.scan_period_ms, active.debounce_count);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Create module manager tasks failed: %s", esp_err_to_name(ret));
    (void)detach_eeprom_devices();
    lifecycle_set(MANAGER_STOPPED);
    return ret;
}

esp_err_t mosaico_module_mgr_deinit(void)
{
    if (lifecycle_get() == MANAGER_STOPPED) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(xTaskGetCurrentTaskHandle() != s_manager.event_task, ESP_ERR_INVALID_STATE, TAG,
                        "deinit is not allowed from module callback");

    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    const manager_lifecycle_t lifecycle = lifecycle_get();
    if (lifecycle != MANAGER_RUNNING && lifecycle != MANAGER_STOPPING) {
        xSemaphoreGive(s_manager.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (lifecycle == MANAGER_RUNNING) {
        for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
            if (s_manager.slots[i].info.owner_state != MOSAICO_MODULE_OWNER_FREE) {
                xSemaphoreGive(s_manager.lock);
                ESP_LOGE(TAG, "Cannot stop manager while slot %s is claimed",
                         mosaico_module_mgr_slot_to_name((mosaico_module_mgr_slot_t)i));
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    if (!lifecycle_acquire_stop(lifecycle == MANAGER_RUNNING)) {
        xSemaphoreGive(s_manager.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (lifecycle == MANAGER_RUNNING) {
        s_manager.scan_stop_confirmed = false;
        s_manager.event_stop_confirmed = false;
    }
    while (xSemaphoreTake(s_manager.waiters_idle, 0) == pdTRUE) {
    }
    uint32_t waiter_mask = s_manager.active_waiter_mask;
    for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        s_manager.slots[i].scan_token++;
    }
    xSemaphoreGive(s_manager.lock);

    if (waiter_mask != 0) {
        xEventGroupSetBits(s_manager.claim_events, waiter_mask);
        if (xSemaphoreTake(s_manager.waiters_idle, pdMS_TO_TICKS(MANAGER_STOP_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "Claim waiters stop timed out");
            lifecycle_release_stop(MANAGER_STOPPING);
            return ESP_ERR_TIMEOUT;
        }
    }
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    TaskHandle_t scan = s_manager.scan_task;
    const bool scan_stop_confirmed = s_manager.scan_stop_confirmed;
    if (!scan_stop_confirmed && scan) {
        xTaskNotifyGive(scan);
    }
    xSemaphoreGive(s_manager.lock);
    if (!scan_stop_confirmed) {
        if (xSemaphoreTake(s_manager.scan_stopped, pdMS_TO_TICKS(MANAGER_STOP_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "Scan task stop timed out");
            lifecycle_release_stop(MANAGER_STOPPING);
            return ESP_ERR_TIMEOUT;
        }
        xSemaphoreTake(s_manager.lock, portMAX_DELAY);
        s_manager.scan_stop_confirmed = true;
        xSemaphoreGive(s_manager.lock);
    }

    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    TaskHandle_t events = s_manager.event_task;
    const bool event_stop_confirmed = s_manager.event_stop_confirmed;
    if (!event_stop_confirmed && events) {
        xTaskNotifyGive(events);
    }
    xSemaphoreGive(s_manager.lock);
    if (!event_stop_confirmed) {
        if (xSemaphoreTake(s_manager.event_stopped, pdMS_TO_TICKS(MANAGER_STOP_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "Event task stop timed out");
            lifecycle_release_stop(MANAGER_STOPPING);
            return ESP_ERR_TIMEOUT;
        }
        xSemaphoreTake(s_manager.lock, portMAX_DELAY);
        s_manager.event_stop_confirmed = true;
        xSemaphoreGive(s_manager.lock);
    }

    esp_err_t ret = detach_eeprom_devices();
    if (ret != ESP_OK) {
        lifecycle_release_stop(MANAGER_STOPPED);
        return ret;
    }
    lifecycle_release_stop(MANAGER_STOPPED);
    ESP_LOGI(TAG, "Module manager stopped");
    return ESP_OK;
}

esp_err_t mosaico_module_mgr_get_info(mosaico_module_mgr_slot_t slot, mosaico_module_mgr_info_t *out_info)
{
    ESP_RETURN_ON_FALSE(out_info && slot_is_valid(slot), ESP_ERR_INVALID_ARG, TAG, "invalid slot info request");
    ESP_RETURN_ON_FALSE(lifecycle_get() == MANAGER_RUNNING, ESP_ERR_INVALID_STATE, TAG,
                        "module manager is not running");
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    if (lifecycle_get() != MANAGER_RUNNING) {
        xSemaphoreGive(s_manager.lock);
        return ESP_ERR_INVALID_STATE;
    }
    *out_info = s_manager.slots[slot].info;
    xSemaphoreGive(s_manager.lock);
    return ESP_OK;
}

static int allocate_waiter_locked(void)
{
    for (int i = 0; i < CLAIM_WAITER_COUNT; ++i) {
        const uint32_t bit = 1U << i;
        if (!(s_manager.active_waiter_mask & bit)) {
            s_manager.active_waiter_mask |= bit;
            xEventGroupClearBits(s_manager.claim_events, bit);
            return i;
        }
    }
    return -1;
}

static void release_waiter_locked(uint32_t bit, bool *notify_idle)
{
    s_manager.active_waiter_mask &= ~bit;
    xEventGroupClearBits(s_manager.claim_events, bit);
    *notify_idle = lifecycle_get() == MANAGER_STOPPING && s_manager.active_waiter_mask == 0;
}

static bool claim_matches_locked(const slot_state_t *state, const mosaico_module_mgr_claim_config_t *config)
{
    if (state->info.owner_state != MOSAICO_MODULE_OWNER_FREE ||
        state->info.presence != MOSAICO_MODULE_PRESENCE_PRESENT) {
        return false;
    }
    if (state->info.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID) {
        return state->info.eeprom.board_type == (uint8_t)config->expected_type;
    }
    return state->info.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_INVALID &&
           (config->flags & MOSAICO_MODULE_CLAIM_ALLOW_INVALID_DESCRIPTOR);
}

static esp_err_t try_claim_locked(const mosaico_module_mgr_claim_config_t *config,
                                  mosaico_module_lease_t *out_lease, uint32_t *out_changes)
{
    for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        const mosaico_module_mgr_slot_t slot =
            config->slot == MOSAICO_MODULE_MGR_SLOT_AUTO ? (mosaico_module_mgr_slot_t)i : config->slot;
        slot_state_t *state = &s_manager.slots[slot];
        if (claim_matches_locked(state, config)) {
            uint32_t lease_id = ++s_manager.next_lease_id;
            if (lease_id == 0) {
                lease_id = ++s_manager.next_lease_id;
            }
            state->lease_id = lease_id;
            state->claim_flags = config->flags;
            state->info.owner_state = MOSAICO_MODULE_OWNER_CLAIMED;
            state->scan_token++;
            uint32_t changes = MOSAICO_MODULE_CHANGE_OWNER;
            if ((config->flags & MOSAICO_MODULE_CLAIM_USE_SLOT_CONTROL_GPIO) &&
                !(config->flags & MOSAICO_MODULE_CLAIM_PROBE_WHILE_CLAIMED)) {
                state->info.presence = MOSAICO_MODULE_PRESENCE_UNKNOWN;
                state->info.descriptor_state = MOSAICO_MODULE_DESCRIPTOR_UNKNOWN;
                memset(&state->info.eeprom, 0, sizeof(state->info.eeprom));
                state->candidate_known = false;
                state->candidate_count = 0;
                state->descriptor_error = ESP_OK;
                changes |= MOSAICO_MODULE_CHANGE_PRESENCE | MOSAICO_MODULE_CHANGE_DESCRIPTOR;
                changes |= refresh_error_locked(state);
            }
            mark_changed_locked(state, changes);
            *out_lease = (mosaico_module_lease_t) {
                .slot = slot,
                .id = lease_id,
            };
            *out_changes = changes;
            return ESP_OK;
        }
        if (config->slot != MOSAICO_MODULE_MGR_SLOT_AUTO) {
            break;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t mosaico_module_mgr_claim(const mosaico_module_mgr_claim_config_t *config,
                                   mosaico_module_lease_t *out_lease)
{
    ESP_RETURN_ON_FALSE(config && out_lease && slot_request_is_valid(config->slot),
                        ESP_ERR_INVALID_ARG, TAG, "invalid claim request");
    *out_lease = (mosaico_module_lease_t) {0};
    ESP_RETURN_ON_FALSE((config->flags & ~ALL_CLAIM_FLAGS) == 0 &&
                            (!(config->flags & MOSAICO_MODULE_CLAIM_PROBE_WHILE_CLAIMED) ||
                             (config->flags & MOSAICO_MODULE_CLAIM_USE_SLOT_CONTROL_GPIO)) &&
                            (!(config->flags & MOSAICO_MODULE_CLAIM_ALLOW_INVALID_DESCRIPTOR) ||
                             config->slot != MOSAICO_MODULE_MGR_SLOT_AUTO),
                        ESP_ERR_INVALID_ARG, TAG, "invalid claim flags");
    ESP_RETURN_ON_FALSE(lifecycle_get() == MANAGER_RUNNING, ESP_ERR_INVALID_STATE, TAG,
                        "module manager is not running");

    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(config->timeout_ms);
    int waiter = -1;
    while (true) {
        xSemaphoreTake(s_manager.lock, portMAX_DELAY);
        if (lifecycle_get() != MANAGER_RUNNING) {
            bool notify_idle = false;
            if (waiter >= 0) {
                release_waiter_locked(1U << waiter, &notify_idle);
            }
            xSemaphoreGive(s_manager.lock);
            if (notify_idle) {
                xSemaphoreGive(s_manager.waiters_idle);
            }
            return ESP_ERR_INVALID_STATE;
        }

        uint32_t changes = 0;
        esp_err_t ret = try_claim_locked(config, out_lease, &changes);
        if (ret == ESP_OK) {
            bool notify_idle = false;
            if (waiter >= 0) {
                release_waiter_locked(1U << waiter, &notify_idle);
            }
            const uint32_t other_waiters = s_manager.active_waiter_mask;
            xSemaphoreGive(s_manager.lock);
            if (notify_idle) {
                xSemaphoreGive(s_manager.waiters_idle);
            }
            ESP_LOGI(TAG, "Module claimed: slot=%s lease=%" PRIu32 " type=%s",
                     mosaico_module_mgr_slot_to_name(out_lease->slot), out_lease->id,
                     mosaico_module_mgr_type_to_name(config->expected_type));
            signal_changes(other_waiters, changes != 0);
            return ESP_OK;
        }
        if (config->timeout_ms == 0) {
            xSemaphoreGive(s_manager.lock);
            return ret;
        }
        if (waiter < 0) {
            waiter = allocate_waiter_locked();
            if (waiter < 0) {
                xSemaphoreGive(s_manager.lock);
                ESP_LOGE(TAG, "Claim waiter table is full");
                return ESP_ERR_NO_MEM;
            }
        } else {
            xEventGroupClearBits(s_manager.claim_events, 1U << waiter);
        }
        xSemaphoreGive(s_manager.lock);

        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout_ticks ||
            xEventGroupWaitBits(s_manager.claim_events, 1U << waiter, pdTRUE, pdTRUE,
                                timeout_ticks - elapsed) == 0) {
            xSemaphoreTake(s_manager.lock, portMAX_DELAY);
            bool notify_idle = false;
            release_waiter_locked(1U << waiter, &notify_idle);
            xSemaphoreGive(s_manager.lock);
            if (notify_idle) {
                xSemaphoreGive(s_manager.waiters_idle);
            }
            ESP_LOGD(TAG, "Claim timed out: slot=%s type=%s",
                     mosaico_module_mgr_slot_to_name(config->slot),
                     mosaico_module_mgr_type_to_name(config->expected_type));
            return ESP_ERR_TIMEOUT;
        }
    }
}

esp_err_t mosaico_module_mgr_release(const mosaico_module_lease_t *lease)
{
    ESP_RETURN_ON_FALSE(lease && slot_is_valid(lease->slot) && lease->id != 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid lease");
    ESP_RETURN_ON_FALSE(lifecycle_get() == MANAGER_RUNNING, ESP_ERR_INVALID_STATE, TAG,
                        "module manager is not running");

    uint32_t claim_flags = 0;
    uint32_t waiter_mask = 0;
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    slot_state_t *state = &s_manager.slots[lease->slot];
    if (lifecycle_get() != MANAGER_RUNNING || state->info.owner_state != MOSAICO_MODULE_OWNER_CLAIMED ||
        state->lease_id != lease->id) {
        xSemaphoreGive(s_manager.lock);
        ESP_LOGE(TAG, "Release rejected: slot=%s lease=%" PRIu32,
                 mosaico_module_mgr_slot_to_name(lease->slot), lease->id);
        return ESP_ERR_INVALID_STATE;
    }
    claim_flags = state->claim_flags;
    /* Keep the lease reserved while GPIO restoration runs without the state lock. */
    state->info.owner_state = MOSAICO_MODULE_OWNER_RESTORING;
    state->scan_token++;
    mark_changed_locked(state, MOSAICO_MODULE_CHANGE_OWNER);
    waiter_mask = s_manager.active_waiter_mask;
    xSemaphoreGive(s_manager.lock);
    signal_changes(waiter_mask, true);

    esp_err_t restore_ret = ESP_OK;
    if (claim_flags & MOSAICO_MODULE_CLAIM_USE_SLOT_CONTROL_GPIO) {
        restore_ret = bsp_subboard_apply_address_select((bsp_subboard_slot_t)lease->slot);
    }

    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    state = &s_manager.slots[lease->slot];
    if (state->lease_id != lease->id || state->info.owner_state != MOSAICO_MODULE_OWNER_RESTORING) {
        xSemaphoreGive(s_manager.lock);
        ESP_LOGE(TAG, "Lease changed during release: slot=%s lease=%" PRIu32,
                 mosaico_module_mgr_slot_to_name(lease->slot), lease->id);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t changes = MOSAICO_MODULE_CHANGE_OWNER;
    state->scan_token++;
    if (restore_ret == ESP_OK) {
        state->info.owner_state = MOSAICO_MODULE_OWNER_FREE;
        state->lease_id = 0;
        state->claim_flags = 0;
        state->resource_error = ESP_OK;
        state->rescan_requested = true;
    } else {
        state->info.owner_state = MOSAICO_MODULE_OWNER_CLAIMED;
        state->resource_error = restore_ret;
    }
    changes |= refresh_error_locked(state);
    mark_changed_locked(state, changes);
    waiter_mask = s_manager.active_waiter_mask;
    xSemaphoreGive(s_manager.lock);
    signal_changes(waiter_mask, true);
    if (restore_ret != ESP_OK) {
        ESP_LOGE(TAG, "Restore slot %s GPIO failed: %s",
                 mosaico_module_mgr_slot_to_name(lease->slot), esp_err_to_name(restore_ret));
        return restore_ret;
    }
    notify_manager_task(false);
    ESP_LOGI(TAG, "Module released: slot=%s lease=%" PRIu32,
             mosaico_module_mgr_slot_to_name(lease->slot), lease->id);
    return ESP_OK;
}

esp_err_t mosaico_module_mgr_request_rescan(mosaico_module_mgr_slot_t slot)
{
    ESP_RETURN_ON_FALSE(slot_request_is_valid(slot), ESP_ERR_INVALID_ARG, TAG, "invalid rescan slot");
    ESP_RETURN_ON_FALSE(lifecycle_get() == MANAGER_RUNNING, ESP_ERR_INVALID_STATE, TAG,
                        "module manager is not running");

    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    if (lifecycle_get() != MANAGER_RUNNING) {
        xSemaphoreGive(s_manager.lock);
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        if (slot == MOSAICO_MODULE_MGR_SLOT_AUTO || slot == (mosaico_module_mgr_slot_t)i) {
            s_manager.slots[i].rescan_requested = true;
            s_manager.slots[i].next_descriptor_tick = 0;
            s_manager.slots[i].scan_token++;
        }
    }
    xSemaphoreGive(s_manager.lock);
    notify_manager_task(false);
    return ESP_OK;
}

esp_err_t mosaico_module_mgr_subscribe(mosaico_module_mgr_event_callback_t callback, void *user_data,
                                       mosaico_module_subscription_t *out_subscription)
{
    ESP_RETURN_ON_FALSE(callback && out_subscription, ESP_ERR_INVALID_ARG, TAG, "invalid subscription");
    *out_subscription = (mosaico_module_subscription_t) {0};
    ESP_RETURN_ON_FALSE(lifecycle_get() == MANAGER_RUNNING, ESP_ERR_INVALID_STATE, TAG,
                        "module manager is not running");

    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    if (lifecycle_get() != MANAGER_RUNNING) {
        xSemaphoreGive(s_manager.lock);
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < MODULE_SUBSCRIBER_COUNT; ++i) {
        module_subscriber_t *subscriber = &s_manager.subscribers[i];
        if (!subscriber->callback && !subscriber->in_callback) {
            while (xSemaphoreTake(subscriber->completed, 0) == pdTRUE) {
            }
            subscriber->generation++;
            if (subscriber->generation == 0) {
                subscriber->generation = 1;
            }
            subscriber->callback = callback;
            subscriber->user_data = user_data;
            subscriber->enabled = true;
            *out_subscription = (mosaico_module_subscription_t) {
                .index = i,
                .generation = subscriber->generation,
            };
            xSemaphoreGive(s_manager.lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_manager.lock);
    ESP_LOGE(TAG, "Subscriber table is full");
    return ESP_ERR_NO_MEM;
}

esp_err_t mosaico_module_mgr_unsubscribe(const mosaico_module_subscription_t *subscription)
{
    ESP_RETURN_ON_FALSE(subscription && subscription->index < MODULE_SUBSCRIBER_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid subscription handle");
    ESP_RETURN_ON_FALSE(lifecycle_get() == MANAGER_RUNNING, ESP_ERR_INVALID_STATE, TAG,
                        "module manager is not running");

    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    module_subscriber_t *subscriber = &s_manager.subscribers[subscription->index];
    if (lifecycle_get() != MANAGER_RUNNING || !subscriber->callback ||
        subscriber->generation != subscription->generation) {
        xSemaphoreGive(s_manager.lock);
        return ESP_ERR_INVALID_STATE;
    }
    subscriber->enabled = false;
    if (!subscriber->in_callback) {
        subscriber->callback = NULL;
        subscriber->user_data = NULL;
        xSemaphoreGive(s_manager.lock);
        return ESP_OK;
    }
    if (xTaskGetCurrentTaskHandle() == s_manager.event_task) {
        xSemaphoreGive(s_manager.lock);
        return ESP_OK;
    }
    if (subscriber->unsubscribe_waiting) {
        xSemaphoreGive(s_manager.lock);
        return ESP_ERR_INVALID_STATE;
    }
    while (xSemaphoreTake(subscriber->completed, 0) == pdTRUE) {
    }
    subscriber->unsubscribe_waiting = true;
    SemaphoreHandle_t completed = subscriber->completed;
    xSemaphoreGive(s_manager.lock);
    xSemaphoreTake(completed, portMAX_DELAY);
    return ESP_OK;
}
