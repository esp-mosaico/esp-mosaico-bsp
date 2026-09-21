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

/** Probe and initialize the ESP32-A1 audio subboard on the V1.2 expansion I2C bus. */
esp_err_t a1_audio_init(void);

/** Configure the A1 DAC and its TDM link for 16-bit stereo PCM. */
esp_err_t a1_audio_open(uint32_t sample_rate, uint8_t volume);
esp_err_t a1_audio_write(const void *data, size_t size);
esp_err_t a1_audio_set_volume(uint8_t volume);
esp_err_t a1_audio_set_mute(bool mute);

#ifdef __cplusplus
}
#endif
