/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "bsp/subboard.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOSAICO_RELAY_CHANNEL_COUNT 4

typedef struct mosaico_relay *mosaico_relay_handle_t;

/** Claim the slot and initialize the module manager before calling. All outputs start low. */
esp_err_t mosaico_relay_new(bsp_subboard_slot_t slot, mosaico_relay_handle_t *out_handle);

/** Set a zero-based channel (0..3). Calls on the same handle must be serialized. */
esp_err_t mosaico_relay_set(mosaico_relay_handle_t handle, unsigned channel, bool on);

/** Best-effort switch-off of every channel; returns the first error. */
esp_err_t mosaico_relay_all_off(mosaico_relay_handle_t handle);

/** Drive all outputs low and free the handle. On failure, retain the handle for retry. */
esp_err_t mosaico_relay_del(mosaico_relay_handle_t handle);

#ifdef __cplusplus
}
#endif
