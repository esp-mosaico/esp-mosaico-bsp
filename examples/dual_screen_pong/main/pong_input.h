/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "mosaico_joystick.h"
#include "pong_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PONG_BUTTON_MASK(button) (1U << (button))
#define PONG_BUTTON_CONFIRM \
    PONG_BUTTON_MASK(MOSAICO_JOYSTICK_BUTTON_1)
#define PONG_BUTTON_PAUSE \
    PONG_BUTTON_MASK(MOSAICO_JOYSTICK_BUTTON_2)
#define PONG_BUTTON_RESTART \
    PONG_BUTTON_MASK(MOSAICO_JOYSTICK_BUTTON_3)
#define PONG_BUTTON_EMOTE \
    PONG_BUTTON_MASK(MOSAICO_JOYSTICK_BUTTON_4)

typedef struct pong_input_context_t *pong_input_handle_t;

esp_err_t pong_input_create(pong_input_handle_t *out_handle);
void pong_input_destroy(pong_input_handle_t handle);

/**
 * @brief Poll the joystick and advance its calibration/hot-plug state.
 *
 * A disconnected board is rediscovered automatically. The output is always
 * initialized; axes and buttons are neutral while unavailable or calibrating.
 */
esp_err_t pong_input_poll(pong_input_handle_t handle, uint32_t now_ms,
                          pong_input_t *out_input, bool *out_ready);

uint16_t pong_input_pressed_edges(pong_input_handle_t handle);

#ifdef __cplusplus
}
#endif
