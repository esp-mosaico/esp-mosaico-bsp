/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "thermal_share.h"

static thermal_frame_t s_frame;
static SemaphoreHandle_t s_lock;
static uint32_t s_frame_id;

esp_err_t thermal_share_init(void)
{
    if (s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_frame, 0, sizeof(s_frame));
    return ESP_OK;
}

void thermal_share_update(const float *pixels, float ta, float vdd, bool rotate_180)
{
    if (!pixels || !s_lock) {
        return;
    }

    float tmin = pixels[0];
    float tmax = pixels[0];
    float sum = 0.0f;
    const int center = (THERMAL_H / 2) * THERMAL_W + (THERMAL_W / 2);

    for (int i = 0; i < THERMAL_N; i++) {
        float t = pixels[i];
        if (t < tmin) {
            tmin = t;
        }
        if (t > tmax) {
            tmax = t;
        }
        sum += t;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_frame.pixels, pixels, sizeof(s_frame.pixels));
    s_frame.ta = ta;
    s_frame.vdd = vdd;
    s_frame.tmin = tmin;
    s_frame.tmax = tmax;
    s_frame.tavg = sum / THERMAL_N;
    s_frame.tcenter = pixels[center];
    s_frame.frame_id = ++s_frame_id;
    s_frame.valid = true;
    s_frame.rotate_180 = rotate_180;
    xSemaphoreGive(s_lock);
}

bool thermal_share_get(thermal_frame_t *out)
{
    if (!out || !s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_frame;
    bool ok = s_frame.valid;
    xSemaphoreGive(s_lock);
    return ok;
}

void thermal_share_invalidate(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_frame.valid = false;
        xSemaphoreGive(s_lock);
    }
}
