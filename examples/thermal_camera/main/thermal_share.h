/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define THERMAL_W           32
#define THERMAL_H           24
#define THERMAL_N           (THERMAL_W * THERMAL_H)

typedef struct {
    float pixels[THERMAL_N];
    float ta;
    float vdd;
    float tmin;
    float tmax;
    float tavg;
    float tcenter;
    uint32_t frame_id;
    bool valid;
    bool rotate_180;
} thermal_frame_t;

esp_err_t thermal_share_init(void);
void thermal_share_invalidate(void);
void thermal_share_update(const float *pixels, float ta, float vdd, bool rotate_180);
bool thermal_share_get(thermal_frame_t *out);
