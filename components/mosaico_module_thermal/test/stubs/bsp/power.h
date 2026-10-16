/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include "driver/i2c_master.h"
esp_err_t bsp_power_set_codec_3v3(bool enable);
