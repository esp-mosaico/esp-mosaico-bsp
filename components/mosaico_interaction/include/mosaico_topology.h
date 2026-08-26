/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "mosaico_interaction.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t peer_id;
    mosaico_edge_t peer_edge;
    uint16_t relative_rotation;
    bool committed;
} mosaico_neighbor_t;

typedef struct {
    mosaico_neighbor_t neighbors[4];
    uint32_t version;
} mosaico_topology_t;

void mosaico_topology_init(mosaico_topology_t *topology);

esp_err_t mosaico_topology_attach(
    mosaico_topology_t *topology,
    mosaico_edge_t local_edge,
    uint64_t peer_id,
    mosaico_edge_t peer_edge,
    uint16_t relative_rotation);

esp_err_t mosaico_topology_detach(
    mosaico_topology_t *topology,
    mosaico_edge_t local_edge,
    uint64_t peer_id);

const mosaico_neighbor_t *mosaico_topology_get_neighbor(
    const mosaico_topology_t *topology,
    mosaico_edge_t local_edge);

bool mosaico_edges_are_complementary(mosaico_edge_t local_edge, mosaico_edge_t peer_edge);

/** Return whether two physical edges belong to the same horizontal or vertical axis. */
bool mosaico_edges_can_connect(mosaico_edge_t local_edge, mosaico_edge_t peer_edge);

/** Resolve 0/180-degree display compensation for a physically valid edge pair. */
esp_err_t mosaico_topology_resolve_display_rotation(
    mosaico_edge_t anchor_edge,
    mosaico_edge_t rotating_edge,
    uint16_t *rotation_degrees);

/** Rotate a cardinal edge clockwise in 90-degree steps. */
esp_err_t mosaico_edge_rotate(
    mosaico_edge_t edge,
    uint16_t rotation_degrees,
    mosaico_edge_t *rotated_edge);

#ifdef __cplusplus
}
#endif
