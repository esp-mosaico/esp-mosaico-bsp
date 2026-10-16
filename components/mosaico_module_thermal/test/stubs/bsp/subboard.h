/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "driver/i2c_master.h"
typedef enum {
    BSP_SUBBOARD_SLOT_LEFT,
    BSP_SUBBOARD_SLOT_RIGHT,
    BSP_SUBBOARD_SLOT_COUNT,
} bsp_subboard_slot_t;
i2c_master_bus_handle_t bsp_subboard_get_i2c_bus(void);
