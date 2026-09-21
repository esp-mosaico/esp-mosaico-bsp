/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef void (*touch_key_callback_t)(uint8_t channel, void *user_data);

esp_err_t touch_keys_start(touch_key_callback_t callback, void *user_data);
