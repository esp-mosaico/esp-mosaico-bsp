/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "mosaico_game.h"
#include "mosaico_interaction.h"
#include "mosaico_mag_classifier.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    mosaico_interaction_state_t state;
    mosaico_edge_mask_t detected_mask;
    mosaico_edge_mask_t committed_mask;
    mosaico_edge_t simulated_edge;
    mosaico_edge_t attached_edge;
    uint16_t simulated_rotation;
    uint16_t attached_rotation;
    char last_event[32];
    uint32_t topology_version;
    uint16_t mesh_node_count;
    uint64_t mesh_root_id;
    bool mesh_orientation_conflict;
    uint32_t orbit_cw_count;
    uint32_t orbit_ccw_count;
    bool pair_match;
    uint32_t idiom_tokens[4];
    size_t idiom_count;
    bool idiom_complete;
    bool radio_ready;
    bool peer_connected;
    bool hardware_source;
    bool sensors_valid;
    bool saturated;
    uint8_t filtered_samples;
    mosaico_mag_calibration_state_t calibration_state;
    int32_t baseline_right_y;
    uint64_t device_id;
    uint64_t last_peer_id;
    uint32_t energy_session_id;
    uint32_t energy_event_id;
    uint32_t energy_hop;
    mosaico_edge_t energy_edge;
    mosaico_energy_phase_t energy_phase;
    uint16_t energy_progress_permille;
    uint16_t display_rotation;
    uint16_t display_rotation_delta;
    bool display_rotation_pending;
} interaction_controller_snapshot_t;

esp_err_t interaction_controller_start(void);
void interaction_controller_set_mock_edge(mosaico_edge_t edge);
void interaction_controller_rotate_mock(void);
void interaction_controller_reset_game(void);
void interaction_controller_recalibrate(void);
void interaction_controller_get_snapshot(interaction_controller_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
