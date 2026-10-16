/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"

typedef struct {
    i2c_master_dev_handle_t device;
    int64_t deadline_us;
    esp_err_t error;
} mlx90640_io_t;
