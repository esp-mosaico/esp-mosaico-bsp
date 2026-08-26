/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t diagnostic_stream_start(void);
void diagnostic_stream_publish(const char *record);
void diagnostic_stream_receive(
    uint64_t source_id,
    int8_t rssi,
    const void *payload,
    size_t payload_len);
