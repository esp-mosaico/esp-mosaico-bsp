/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOSAICO_MODULE_MGR_EEPROM_MAGIC          "ESP"
#define MOSAICO_MODULE_MGR_EEPROM_MAGIC_LEN      3U
#define MOSAICO_MODULE_MGR_EEPROM_IMAGE_SIZE     0x86U
#define MOSAICO_MODULE_MGR_SLOT_AUTO             MOSAICO_MODULE_MGR_SLOT_COUNT

#define MOSAICO_MODULE_MGR_DEFAULT_CONFIG() { \
    .scan_period_ms = 200,                  \
    .debounce_count = 3,                    \
    .event_callback = NULL,                 \
    .event_user_data = NULL,                \
}

typedef enum {
    MOSAICO_MODULE_MGR_SLOT_LEFT = 0,
    MOSAICO_MODULE_MGR_SLOT_RIGHT,
    MOSAICO_MODULE_MGR_SLOT_COUNT,
} mosaico_module_mgr_slot_t;

typedef enum {
    MOSAICO_BOARD_TYPE_CORE        = 0x01,
    MOSAICO_BOARD_TYPE_POWER       = 0x02,
    MOSAICO_BOARD_TYPE_DOCK        = 0x03,
    MOSAICO_BOARD_TYPE_HANDLE      = 0x04,
    MOSAICO_BOARD_TYPE_BALANCE_CAR = 0x05,
    MOSAICO_BOARD_TYPE_DISPLAY     = 0x06,
    MOSAICO_BOARD_TYPE_CAMERA      = 0x07,
    MOSAICO_BOARD_TYPE_SENSOR      = 0x08,
    MOSAICO_BOARD_TYPE_IO_EXP      = 0x09,
    MOSAICO_BOARD_TYPE_TOF         = 0x10,
    MOSAICO_BOARD_TYPE_MATRIX_LED  = 0x11,
    MOSAICO_BOARD_TYPE_THERMAL     = 0x12,
    MOSAICO_BOARD_TYPE_RELAY       = 0x13,
    MOSAICO_BOARD_TYPE_BUTTON_LED  = 0x14,
} mosaico_board_type_t;

typedef enum {
    MOSAICO_MODULE_MGR_STATE_EMPTY = 0,
    MOSAICO_MODULE_MGR_STATE_INVALID,
    MOSAICO_MODULE_MGR_STATE_READY,
    MOSAICO_MODULE_MGR_STATE_CLAIMED,
} mosaico_module_mgr_state_t;

typedef enum {
    MOSAICO_MODULE_MGR_EVENT_INSERTED = 0,
    MOSAICO_MODULE_MGR_EVENT_REMOVED,
    MOSAICO_MODULE_MGR_EVENT_INVALID,
    MOSAICO_MODULE_MGR_EVENT_CLAIMED,
    MOSAICO_MODULE_MGR_EVENT_RELEASED,
} mosaico_module_mgr_event_t;

typedef struct __attribute__((packed)) {
    char magic[3];
    uint8_t board_type;
    uint16_t board_id;
    uint16_t hw_version;
    uint16_t sw_version;
    uint16_t vendor_id;
    uint32_t board_flags;
    uint32_t serial_number;
    char board_name[32];
    uint16_t desc_crc16;
    uint32_t manufacture_date;
    uint16_t batch_number;
    uint16_t factory_id;
    uint16_t mfg_crc16;
    uint16_t param_version;
    uint16_t param_length;
    uint8_t param_data[64];
    uint16_t param_crc16;
} mosaico_module_mgr_eeprom_v1_t;

typedef struct {
    mosaico_module_mgr_slot_t slot;
    mosaico_module_mgr_state_t state;
    uint32_t generation;
    /** AT24C02 7-bit address that identified this slot (0x50 left / 0x51 right). */
    uint8_t eeprom_addr;
    mosaico_module_mgr_eeprom_v1_t eeprom;
} mosaico_module_mgr_info_t;

typedef void (*mosaico_module_mgr_event_callback_t)(mosaico_module_mgr_event_t event,
                                                     const mosaico_module_mgr_info_t *info,
                                                     void *user_data);

typedef struct {
    uint32_t scan_period_ms;
    uint8_t debounce_count;
    mosaico_module_mgr_event_callback_t event_callback;
    void *event_user_data;
} mosaico_module_mgr_config_t;

/**
 * @brief Start the singleton Mosaico module manager
 *
 * Passing NULL selects automatic defaults. Repeated calls are idempotent.
 */
esp_err_t mosaico_module_mgr_init(const mosaico_module_mgr_config_t *config);

/**
 * @brief Stop the manager
 *
 * Returns ESP_ERR_INVALID_STATE while any slot is claimed.
 */
esp_err_t mosaico_module_mgr_deinit(void);

esp_err_t mosaico_module_mgr_get_info(mosaico_module_mgr_slot_t slot,
                                      mosaico_module_mgr_info_t *out_info);

esp_err_t mosaico_module_mgr_find(mosaico_board_type_t type,
                                  mosaico_module_mgr_slot_t preferred_slot,
                                  mosaico_module_mgr_info_t *out_info);

esp_err_t mosaico_module_mgr_wait_for(mosaico_board_type_t type,
                                      mosaico_module_mgr_slot_t preferred_slot,
                                      uint32_t timeout_ms,
                                      mosaico_module_mgr_info_t *out_info);

esp_err_t mosaico_module_mgr_claim(mosaico_module_mgr_slot_t slot,
                                   mosaico_board_type_t expected_type);

/**
 * @brief Claim a slot whose EEPROM never identified a board
 *
 * Stops scanning the slot like a normal claim does, so the driver may repurpose
 * the connector pins. The caller takes responsibility for what is plugged in.
 */
esp_err_t mosaico_module_mgr_claim_unidentified(mosaico_module_mgr_slot_t slot);

esp_err_t mosaico_module_mgr_release(mosaico_module_mgr_slot_t slot);

void mosaico_module_mgr_request_rescan(void);

bool mosaico_module_mgr_eeprom_valid(const mosaico_module_mgr_eeprom_v1_t *image);
uint16_t mosaico_module_mgr_crc16(const uint8_t *data, size_t len);
const char *mosaico_module_mgr_slot_to_name(mosaico_module_mgr_slot_t slot);
const char *mosaico_module_mgr_type_to_name(mosaico_board_type_t type);

/**
 * @brief Resolve slot from AT24C02 address (0x50 left / 0x51 right)
 */
esp_err_t mosaico_module_mgr_slot_from_eeprom_addr(uint8_t eeprom_addr,
                                                   mosaico_module_mgr_slot_t *out_slot);

#ifdef __cplusplus
}
#endif
