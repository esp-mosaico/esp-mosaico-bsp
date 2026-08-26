/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mosaico_mag_classifier.h"
#include "mosaico_peer_link.h"
#include "pong_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PONG_DOCKING_SAMPLE_PERIOD_MS 50U

typedef struct {
    bool started;
    bool hardware_available;
    bool sensors_valid;
    pong_dock_state_t state;
    pong_role_t inferred_role;
    mosaico_mag_calibration_state_t calibration_state;
    mosaico_edge_mask_t detected_edges;
    mosaico_edge_t local_edge;
    mosaico_edge_t peer_edge;
    uint64_t peer_id;
    uint32_t session_id;
} pong_docking_status_t;

/**
 * Initialize magnetic layout detection.
 *
 * Magnetometer or calibration failures are reported through status as
 * PONG_DOCK_WIRELESS and do not fail the already-running network.
 */
esp_err_t pong_docking_start(void);
void pong_docking_stop(void);

/**
 * Sample the magnetometers and advance peer contact sessions.
 */
void pong_docking_tick(uint32_t now_ms);

/**
 * Feed contact-session frames drained by pong_network_tick().
 */
void pong_docking_receive_peer_message(const mosaico_peer_message_t *message,
                                       uint32_t now_ms);

esp_err_t pong_docking_get_status(pong_docking_status_t *status);

#ifdef __cplusplus
}
#endif
