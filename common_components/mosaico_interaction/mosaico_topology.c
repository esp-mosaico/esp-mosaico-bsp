/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_topology.h"

#include <string.h>

static int edge_index(mosaico_edge_t edge)
{
    switch (edge) {
    case MOSAICO_EDGE_TOP:    return 0;
    case MOSAICO_EDGE_RIGHT:  return 1;
    case MOSAICO_EDGE_BOTTOM: return 2;
    case MOSAICO_EDGE_LEFT:   return 3;
    default:                  return -1;
    }
}

void mosaico_topology_init(mosaico_topology_t *topology)
{
    if (topology) {
        memset(topology, 0, sizeof(*topology));
    }
}

esp_err_t mosaico_topology_attach(
    mosaico_topology_t *topology,
    mosaico_edge_t local_edge,
    uint64_t peer_id,
    mosaico_edge_t peer_edge,
    uint16_t relative_rotation)
{
    if (!topology || peer_id == 0 || relative_rotation > 270 ||
        relative_rotation % 90 != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const int index = edge_index(local_edge);
    if (index < 0 || !mosaico_edges_can_connect(local_edge, peer_edge)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t expected_rotation = 0;
    if (mosaico_topology_resolve_display_rotation(
            local_edge, peer_edge, &expected_rotation) != ESP_OK ||
        relative_rotation != expected_rotation) {
        return ESP_ERR_INVALID_ARG;
    }
    mosaico_neighbor_t *neighbor = &topology->neighbors[index];
    if (neighbor->committed && neighbor->peer_id != peer_id) {
        return ESP_ERR_INVALID_STATE;
    }
    *neighbor = (mosaico_neighbor_t) {
        .peer_id = peer_id,
        .peer_edge = peer_edge,
        .relative_rotation = relative_rotation,
        .committed = true,
    };
    topology->version++;
    return ESP_OK;
}

esp_err_t mosaico_topology_detach(
    mosaico_topology_t *topology,
    mosaico_edge_t local_edge,
    uint64_t peer_id)
{
    if (!topology || peer_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const int index = edge_index(local_edge);
    if (index < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    mosaico_neighbor_t *neighbor = &topology->neighbors[index];
    if (!neighbor->committed || neighbor->peer_id != peer_id) {
        return ESP_ERR_NOT_FOUND;
    }
    memset(neighbor, 0, sizeof(*neighbor));
    topology->version++;
    return ESP_OK;
}

const mosaico_neighbor_t *mosaico_topology_get_neighbor(
    const mosaico_topology_t *topology,
    mosaico_edge_t local_edge)
{
    const int index = edge_index(local_edge);
    return topology && index >= 0 && topology->neighbors[index].committed ?
           &topology->neighbors[index] : NULL;
}

bool mosaico_edges_are_complementary(mosaico_edge_t local_edge, mosaico_edge_t peer_edge)
{
    return (local_edge == MOSAICO_EDGE_TOP && peer_edge == MOSAICO_EDGE_BOTTOM) ||
           (local_edge == MOSAICO_EDGE_RIGHT && peer_edge == MOSAICO_EDGE_LEFT) ||
           (local_edge == MOSAICO_EDGE_BOTTOM && peer_edge == MOSAICO_EDGE_TOP) ||
           (local_edge == MOSAICO_EDGE_LEFT && peer_edge == MOSAICO_EDGE_RIGHT);
}

bool mosaico_edges_can_connect(mosaico_edge_t local_edge, mosaico_edge_t peer_edge)
{
    const int local_index = edge_index(local_edge);
    const int peer_index = edge_index(peer_edge);
    return local_index >= 0 && peer_index >= 0 &&
           (local_index % 2) == (peer_index % 2);
}

esp_err_t mosaico_topology_resolve_display_rotation(
    mosaico_edge_t anchor_edge,
    mosaico_edge_t rotating_edge,
    uint16_t *rotation_degrees)
{
    const int anchor_index = edge_index(anchor_edge);
    const int rotating_index = edge_index(rotating_edge);
    if (anchor_index < 0 || rotating_index < 0 || !rotation_degrees ||
        !mosaico_edges_can_connect(anchor_edge, rotating_edge)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The device orientation maps rotating_edge opposite anchor_edge. The display
     * compensation is the inverse of that physical device orientation. */
    const int rotation_steps = (rotating_index - anchor_index + 6) % 4;
    *rotation_degrees = (uint16_t)(rotation_steps * 90);
    return ESP_OK;
}

esp_err_t mosaico_edge_rotate(
    mosaico_edge_t edge,
    uint16_t rotation_degrees,
    mosaico_edge_t *rotated_edge)
{
    static const mosaico_edge_t edges[] = {
        MOSAICO_EDGE_TOP,
        MOSAICO_EDGE_RIGHT,
        MOSAICO_EDGE_BOTTOM,
        MOSAICO_EDGE_LEFT,
    };
    const int index = edge_index(edge);
    if (index < 0 || !rotated_edge || rotation_degrees > 270 ||
        rotation_degrees % 90 != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *rotated_edge = edges[(index + rotation_degrees / 90) % 4];
    return ESP_OK;
}
