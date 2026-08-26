/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mosaico_interaction.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOSAICO_MESH_MAX_NODES 16
#define MOSAICO_MESH_EDGE_COUNT 4
#define MOSAICO_MESH_DEFAULT_RECORD_TTL 15
#define MOSAICO_MESH_RECORD_STALE_MS 5000U

#define MOSAICO_MESH_LINK_ACTIVE (1U << 0)

typedef enum {
    MOSAICO_MESH_CONFLICT_NONE = 0,
    MOSAICO_MESH_CONFLICT_POSE = (1U << 0),
    MOSAICO_MESH_CONFLICT_POSITION = (1U << 1),
} mosaico_mesh_conflict_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t flags;
    uint8_t ttl;
    uint8_t local_edge;
    uint8_t peer_edge;
    uint8_t reserved;
    uint16_t relative_rotation;
    uint32_t sequence;
    uint32_t boot_id;
    uint64_t origin_id;
    uint64_t neighbor_id;
} mosaico_mesh_wire_link_t;

typedef struct {
    uint64_t neighbor_id;
    uint32_t sequence;
    uint32_t updated_ms;
    uint16_t relative_rotation;
    mosaico_edge_t peer_edge;
    bool active;
    bool known;
} mosaico_mesh_link_t;

typedef struct {
    uint64_t device_id;
    uint32_t boot_id;
    uint32_t previous_boot_id;
    int16_t x;
    int16_t y;
    uint16_t rotation_degrees;
    uint8_t depth;
    bool pose_valid;
    mosaico_mesh_link_t links[MOSAICO_MESH_EDGE_COUNT];
} mosaico_mesh_node_t;

typedef struct {
    uint64_t local_id;
    uint32_t local_boot_id;
    uint64_t root_id;
    uint32_t topology_version;
    uint32_t local_sequence;
    size_t node_count;
    size_t connected_count;
    bool orientation_conflict;
    uint8_t conflict_flags;
    mosaico_mesh_node_t nodes[MOSAICO_MESH_MAX_NODES];
} mosaico_mesh_t;

esp_err_t mosaico_mesh_init(mosaico_mesh_t *mesh, uint64_t local_id);

esp_err_t mosaico_mesh_init_with_boot_id(
    mosaico_mesh_t *mesh,
    uint64_t local_id,
    uint32_t boot_id);

esp_err_t mosaico_mesh_attach(
    mosaico_mesh_t *mesh,
    mosaico_edge_t local_edge,
    uint64_t peer_id,
    mosaico_edge_t peer_edge,
    uint32_t now_ms,
    mosaico_mesh_wire_link_t *wire);

esp_err_t mosaico_mesh_detach(
    mosaico_mesh_t *mesh,
    mosaico_edge_t local_edge,
    uint64_t peer_id,
    uint32_t now_ms,
    mosaico_mesh_wire_link_t *wire);

esp_err_t mosaico_mesh_refresh(
    mosaico_mesh_t *mesh,
    mosaico_edge_t local_edge,
    uint32_t now_ms,
    mosaico_mesh_wire_link_t *wire);

esp_err_t mosaico_mesh_receive(
    mosaico_mesh_t *mesh,
    const mosaico_mesh_wire_link_t *wire,
    uint32_t now_ms,
    mosaico_mesh_wire_link_t *forward,
    bool *topology_changed);

bool mosaico_mesh_expire(
    mosaico_mesh_t *mesh,
    uint32_t now_ms,
    uint32_t stale_ms);

const mosaico_mesh_node_t *mosaico_mesh_get_node(
    const mosaico_mesh_t *mesh,
    uint64_t device_id);

esp_err_t mosaico_mesh_get_path(
    const mosaico_mesh_t *mesh,
    uint64_t source_id,
    uint64_t destination_id,
    uint64_t *path,
    size_t path_capacity,
    size_t *path_length);

esp_err_t mosaico_mesh_get_next_hop(
    const mosaico_mesh_t *mesh,
    uint64_t destination_id,
    uint64_t *next_hop_id,
    mosaico_edge_t *local_edge);

size_t mosaico_mesh_get_connected_ids(
    const mosaico_mesh_t *mesh,
    uint64_t *device_ids,
    size_t capacity);

uint32_t mosaico_mesh_get_topology_id(const mosaico_mesh_t *mesh);

uint8_t mosaico_mesh_get_conflicts(const mosaico_mesh_t *mesh);

size_t mosaico_mesh_get_traversal_order(
    const mosaico_mesh_t *mesh,
    uint64_t *device_ids,
    size_t capacity);

#ifdef __cplusplus
}
#endif
