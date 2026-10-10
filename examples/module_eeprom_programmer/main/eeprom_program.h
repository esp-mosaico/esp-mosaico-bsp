/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "mosaico_module_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *label;
    mosaico_board_type_t board_type;
    uint16_t board_id;
    const char *board_name;
} eeprom_board_profile_t;

typedef void (*eeprom_program_progress_cb_t)(int percent, const char *status);

typedef struct {
    bool success;
    mosaico_module_mgr_slot_t slot;
    char message[96];
} eeprom_program_result_t;

size_t eeprom_program_profile_count(void);
const eeprom_board_profile_t *eeprom_program_get_profile(size_t index);

/* Initialize once before starting other clients on the shared I2C bus. */
esp_err_t eeprom_program_init(i2c_master_bus_handle_t bus);
esp_err_t eeprom_program_run(size_t profile_index, eeprom_program_progress_cb_t progress_cb,
                            eeprom_program_result_t *result);

#ifdef __cplusplus
}
#endif
