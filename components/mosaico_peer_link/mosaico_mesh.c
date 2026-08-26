/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_mesh.h"
#include "mosaico_topology.h"

#include <limits.h>
#include <string.h>

#define MOSAICO_MESH_WIRE_VERSION 1U

_Static_assert(sizeof(mosaico_mesh_wire_link_t) == 32,
               "unexpected mesh link wire size");

static int edge_index(mosaico_edge_t edge)
{
    return edge >= MOSAICO_EDGE_TOP && edge <= MOSAICO_EDGE_LEFT ?
        (int)edge - (int)MOSAICO_EDGE_TOP : -1;
}

static mosaico_mesh_node_t *find_node(mosaico_mesh_t *mesh, uint64_t device_id)
{
    for (size_t i = 0; i < mesh->node_count; ++i) {
        if (mesh->nodes[i].device_id == device_id) {
            return &mesh->nodes[i];
        }
    }
    return NULL;
}

static const mosaico_mesh_node_t *find_node_const(
    const mosaico_mesh_t *mesh,
    uint64_t device_id)
{
    for (size_t i = 0; i < mesh->node_count; ++i) {
        if (mesh->nodes[i].device_id == device_id) {
            return &mesh->nodes[i];
        }
    }
    return NULL;
}

static mosaico_mesh_node_t *get_or_add_node(
    mosaico_mesh_t *mesh,
    uint64_t device_id)
{
    mosaico_mesh_node_t *node = find_node(mesh, device_id);
    if (node || mesh->node_count >= MOSAICO_MESH_MAX_NODES) {
        return node;
    }
    node = &mesh->nodes[mesh->node_count++];
    memset(node, 0, sizeof(*node));
    node->device_id = device_id;
    return node;
}

static bool sequence_is_newer(uint32_t incoming, uint32_t current)
{
    return (int32_t)(incoming - current) > 0;
}

static bool node_is_recently_referenced(
    const mosaico_mesh_t *mesh,
    uint64_t device_id,
    uint32_t now_ms,
    uint32_t timeout)
{
    for (size_t node = 0; node < mesh->node_count; ++node) {
        for (size_t edge = 0; edge < MOSAICO_MESH_EDGE_COUNT; ++edge) {
            const mosaico_mesh_link_t *link = &mesh->nodes[node].links[edge];
            if (link->known &&
                (mesh->nodes[node].device_id == device_id ||
                 link->neighbor_id == device_id) &&
                (uint32_t)(now_ms - link->updated_ms) < timeout) {
                return true;
            }
        }
    }
    return false;
}

static bool prune_stale_nodes(
    mosaico_mesh_t *mesh,
    uint32_t now_ms,
    uint32_t timeout)
{
    bool pruned = false;
    for (size_t index = 0; index < mesh->node_count;) {
        const mosaico_mesh_node_t *node = &mesh->nodes[index];
        if (node->device_id == mesh->local_id || node->pose_valid ||
            node_is_recently_referenced(
                mesh, node->device_id, now_ms, timeout)) {
            index++;
            continue;
        }
        mesh->nodes[index] = mesh->nodes[mesh->node_count - 1U];
        memset(&mesh->nodes[mesh->node_count - 1U], 0,
               sizeof(mesh->nodes[mesh->node_count - 1U]));
        mesh->node_count--;
        pruned = true;
    }
    return pruned;
}

static const mosaico_mesh_link_t *reciprocal_link(
    const mosaico_mesh_t *mesh,
    const mosaico_mesh_node_t *node,
    size_t edge)
{
    const mosaico_mesh_link_t *link = &node->links[edge];
    if (!link->known || !link->active) {
        return NULL;
    }
    const mosaico_mesh_node_t *peer = find_node_const(mesh, link->neighbor_id);
    const int peer_index = edge_index(link->peer_edge);
    if (!peer || peer_index < 0) {
        return NULL;
    }
    const mosaico_mesh_link_t *reverse = &peer->links[peer_index];
    return reverse->known && reverse->active &&
           reverse->neighbor_id == node->device_id &&
           reverse->peer_edge == (mosaico_edge_t)(edge + MOSAICO_EDGE_TOP) &&
           reverse->relative_rotation ==
               (uint16_t)((360U - link->relative_rotation) % 360U) ?
           link : NULL;
}

static void edge_step(mosaico_edge_t edge, int16_t *x, int16_t *y)
{
    switch (edge) {
    case MOSAICO_EDGE_TOP:    (*y)--; break;
    case MOSAICO_EDGE_RIGHT:  (*x)++; break;
    case MOSAICO_EDGE_BOTTOM: (*y)++; break;
    case MOSAICO_EDGE_LEFT:   (*x)--; break;
    default: break;
    }
}

static bool rebuild_topology(mosaico_mesh_t *mesh)
{
    uint64_t previous_root = mesh->root_id;
    size_t previous_count = mesh->connected_count;
    uint8_t previous_conflicts = mesh->conflict_flags;
    uint16_t previous_rotation = 0;
    const mosaico_mesh_node_t *previous_local =
        find_node_const(mesh, mesh->local_id);
    if (previous_local) {
        previous_rotation = previous_local->rotation_degrees;
    }

    for (size_t i = 0; i < mesh->node_count; ++i) {
        mesh->nodes[i].pose_valid = false;
        mesh->nodes[i].depth = 0;
        mesh->nodes[i].x = 0;
        mesh->nodes[i].y = 0;
        mesh->nodes[i].rotation_degrees = 0;
    }

    bool reachable[MOSAICO_MESH_MAX_NODES] = {0};
    size_t queue[MOSAICO_MESH_MAX_NODES] = {0};
    size_t head = 0;
    size_t tail = 0;
    size_t local_index = 0;
    for (; local_index < mesh->node_count; ++local_index) {
        if (mesh->nodes[local_index].device_id == mesh->local_id) {
            break;
        }
    }
    if (local_index >= mesh->node_count) {
        return false;
    }
    reachable[local_index] = true;
    queue[tail++] = local_index;
    uint64_t root_id = mesh->local_id;
    while (head < tail) {
        const size_t index = queue[head++];
        const mosaico_mesh_node_t *node = &mesh->nodes[index];
        if (node->device_id < root_id) {
            root_id = node->device_id;
        }
        for (size_t edge = 0; edge < MOSAICO_MESH_EDGE_COUNT; ++edge) {
            const mosaico_mesh_link_t *link = reciprocal_link(mesh, node, edge);
            if (!link) {
                continue;
            }
            for (size_t peer = 0; peer < mesh->node_count; ++peer) {
                if (mesh->nodes[peer].device_id == link->neighbor_id &&
                    !reachable[peer]) {
                    reachable[peer] = true;
                    queue[tail++] = peer;
                    break;
                }
            }
        }
    }

    size_t root_index = 0;
    for (; root_index < mesh->node_count; ++root_index) {
        if (mesh->nodes[root_index].device_id == root_id) {
            break;
        }
    }
    head = 0;
    tail = 0;
    bool visited[MOSAICO_MESH_MAX_NODES] = {0};
    visited[root_index] = true;
    queue[tail++] = root_index;
    mesh->nodes[root_index].pose_valid = true;
    mesh->conflict_flags = MOSAICO_MESH_CONFLICT_NONE;
    while (head < tail) {
        const size_t index = queue[head++];
        mosaico_mesh_node_t *node = &mesh->nodes[index];
        for (size_t edge = 0; edge < MOSAICO_MESH_EDGE_COUNT; ++edge) {
            const mosaico_mesh_link_t *link = reciprocal_link(mesh, node, edge);
            if (!link) {
                continue;
            }
            size_t peer_index = 0;
            for (; peer_index < mesh->node_count; ++peer_index) {
                if (mesh->nodes[peer_index].device_id == link->neighbor_id) {
                    break;
                }
            }
            if (peer_index >= mesh->node_count || !reachable[peer_index]) {
                continue;
            }
            uint16_t relative_rotation = 0;
            if (mosaico_topology_resolve_display_rotation(
                    (mosaico_edge_t)(edge + MOSAICO_EDGE_TOP),
                    link->peer_edge, &relative_rotation) != ESP_OK) {
                continue;
            }
            uint16_t rotation =
                (uint16_t)((node->rotation_degrees + relative_rotation) % 360U);
            mosaico_edge_t global_edge = MOSAICO_EDGE_NONE;
            if (mosaico_edge_rotate(
                    (mosaico_edge_t)(edge + MOSAICO_EDGE_TOP),
                    (uint16_t)((360U - node->rotation_degrees) % 360U),
                    &global_edge) != ESP_OK) {
                continue;
            }
            int16_t x = node->x;
            int16_t y = node->y;
            edge_step(global_edge, &x, &y);
            mosaico_mesh_node_t *peer = &mesh->nodes[peer_index];
            if (visited[peer_index]) {
                if (peer->x != x || peer->y != y ||
                    peer->rotation_degrees != rotation) {
                    mesh->conflict_flags |= MOSAICO_MESH_CONFLICT_POSE;
                }
                continue;
            }
            visited[peer_index] = true;
            peer->pose_valid = true;
            peer->x = x;
            peer->y = y;
            peer->rotation_degrees = rotation;
            peer->depth = node->depth + 1U;
            queue[tail++] = peer_index;
        }
    }
    for (size_t first = 0; first < mesh->node_count; ++first) {
        if (!mesh->nodes[first].pose_valid) {
            continue;
        }
        for (size_t second = first + 1; second < mesh->node_count; ++second) {
            if (mesh->nodes[second].pose_valid &&
                mesh->nodes[first].x == mesh->nodes[second].x &&
                mesh->nodes[first].y == mesh->nodes[second].y) {
                mesh->conflict_flags |= MOSAICO_MESH_CONFLICT_POSITION;
            }
        }
    }
    mesh->orientation_conflict = mesh->conflict_flags != MOSAICO_MESH_CONFLICT_NONE;
    mesh->root_id = root_id;
    mesh->connected_count = tail;
    const mosaico_mesh_node_t *local = find_node_const(mesh, mesh->local_id);
    const bool changed = previous_root != mesh->root_id ||
        previous_count != mesh->connected_count ||
        previous_conflicts != mesh->conflict_flags ||
        !local || previous_rotation != local->rotation_degrees;
    if (changed) {
        mesh->topology_version++;
    }
    return changed;
}

static void fill_wire(
    mosaico_mesh_t *mesh,
    mosaico_edge_t local_edge,
    const mosaico_mesh_link_t *link,
    mosaico_mesh_wire_link_t *wire)
{
    *wire = (mosaico_mesh_wire_link_t) {
        .version = MOSAICO_MESH_WIRE_VERSION,
        .flags = link->active ? MOSAICO_MESH_LINK_ACTIVE : 0,
        .ttl = MOSAICO_MESH_DEFAULT_RECORD_TTL,
        .local_edge = (uint8_t)local_edge,
        .peer_edge = (uint8_t)link->peer_edge,
        .relative_rotation = link->relative_rotation,
        .sequence = link->sequence,
        .boot_id = mesh->local_boot_id,
        .origin_id = mesh->local_id,
        .neighbor_id = link->neighbor_id,
    };
}

esp_err_t mosaico_mesh_init(mosaico_mesh_t *mesh, uint64_t local_id)
{
    return mosaico_mesh_init_with_boot_id(mesh, local_id, 1);
}

esp_err_t mosaico_mesh_init_with_boot_id(
    mosaico_mesh_t *mesh,
    uint64_t local_id,
    uint32_t boot_id)
{
    if (!mesh || local_id == 0 || boot_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(mesh, 0, sizeof(*mesh));
    mesh->local_id = local_id;
    mesh->local_boot_id = boot_id;
    mesh->root_id = local_id;
    mesh->topology_version = 1;
    mosaico_mesh_node_t *local = get_or_add_node(mesh, local_id);
    local->boot_id = boot_id;
    local->pose_valid = true;
    mesh->connected_count = 1;
    return ESP_OK;
}

esp_err_t mosaico_mesh_attach(
    mosaico_mesh_t *mesh,
    mosaico_edge_t local_edge,
    uint64_t peer_id,
    mosaico_edge_t peer_edge,
    uint32_t now_ms,
    mosaico_mesh_wire_link_t *wire)
{
    const int index = edge_index(local_edge);
    if (!mesh || mesh->local_id == 0 || index < 0 || peer_id == 0 ||
        peer_id == mesh->local_id || edge_index(peer_edge) < 0 || !wire) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t relative_rotation = 0;
    esp_err_t ret = mosaico_topology_resolve_display_rotation(
        local_edge, peer_edge, &relative_rotation);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!get_or_add_node(mesh, peer_id)) {
        return ESP_ERR_NO_MEM;
    }
    mosaico_mesh_node_t *local = find_node(mesh, mesh->local_id);
    mosaico_mesh_link_t *link = &local->links[index];
    if (link->known && link->active && link->neighbor_id != peer_id) {
        return ESP_ERR_INVALID_STATE;
    }
    if (link->known && link->active && link->neighbor_id == peer_id &&
        link->peer_edge == peer_edge &&
        link->relative_rotation == relative_rotation) {
        link->sequence = ++mesh->local_sequence;
        link->updated_ms = now_ms;
        fill_wire(mesh, local_edge, link, wire);
        return ESP_OK;
    }
    *link = (mosaico_mesh_link_t) {
        .neighbor_id = peer_id,
        .sequence = ++mesh->local_sequence,
        .updated_ms = now_ms,
        .relative_rotation = relative_rotation,
        .peer_edge = peer_edge,
        .active = true,
        .known = true,
    };
    fill_wire(mesh, local_edge, link, wire);
    mesh->topology_version++;
    rebuild_topology(mesh);
    return ESP_OK;
}

esp_err_t mosaico_mesh_detach(
    mosaico_mesh_t *mesh,
    mosaico_edge_t local_edge,
    uint64_t peer_id,
    uint32_t now_ms,
    mosaico_mesh_wire_link_t *wire)
{
    const int index = edge_index(local_edge);
    if (!mesh || mesh->local_id == 0 || index < 0 || peer_id == 0 || !wire) {
        return ESP_ERR_INVALID_ARG;
    }
    mosaico_mesh_node_t *local = find_node(mesh, mesh->local_id);
    mosaico_mesh_link_t *link = &local->links[index];
    if (!link->known || !link->active || link->neighbor_id != peer_id) {
        return ESP_ERR_NOT_FOUND;
    }
    link->active = false;
    link->sequence = ++mesh->local_sequence;
    link->updated_ms = now_ms;
    fill_wire(mesh, local_edge, link, wire);
    mesh->topology_version++;
    rebuild_topology(mesh);
    return ESP_OK;
}

esp_err_t mosaico_mesh_refresh(
    mosaico_mesh_t *mesh,
    mosaico_edge_t local_edge,
    uint32_t now_ms,
    mosaico_mesh_wire_link_t *wire)
{
    const int index = edge_index(local_edge);
    if (!mesh || mesh->local_id == 0 || index < 0 || !wire) {
        return ESP_ERR_INVALID_ARG;
    }
    mosaico_mesh_link_t *link =
        &find_node(mesh, mesh->local_id)->links[index];
    if (!link->known || !link->active) {
        return ESP_ERR_NOT_FOUND;
    }
    link->sequence = ++mesh->local_sequence;
    link->updated_ms = now_ms;
    fill_wire(mesh, local_edge, link, wire);
    return ESP_OK;
}

esp_err_t mosaico_mesh_receive(
    mosaico_mesh_t *mesh,
    const mosaico_mesh_wire_link_t *wire,
    uint32_t now_ms,
    mosaico_mesh_wire_link_t *forward,
    bool *topology_changed)
{
    uint16_t expected_rotation = 0;
    if (!mesh || mesh->local_id == 0 || !wire ||
        wire->version != MOSAICO_MESH_WIRE_VERSION || wire->origin_id == 0 ||
        wire->neighbor_id == 0 || wire->origin_id == wire->neighbor_id ||
        wire->sequence == 0 || wire->boot_id == 0 ||
        wire->ttl > MOSAICO_MESH_DEFAULT_RECORD_TTL ||
        (wire->flags & ~MOSAICO_MESH_LINK_ACTIVE) != 0 ||
        edge_index((mosaico_edge_t)wire->local_edge) < 0 ||
        edge_index((mosaico_edge_t)wire->peer_edge) < 0 ||
        mosaico_topology_resolve_display_rotation(
            (mosaico_edge_t)wire->local_edge,
            (mosaico_edge_t)wire->peer_edge, &expected_rotation) != ESP_OK ||
        wire->relative_rotation != expected_rotation) {
        return ESP_ERR_INVALID_ARG;
    }
    if (topology_changed) {
        *topology_changed = false;
    }
    if (wire->origin_id == mesh->local_id) {
        return ESP_ERR_NOT_FOUND;
    }
    mosaico_mesh_node_t *origin = get_or_add_node(mesh, wire->origin_id);
    if (!origin || !get_or_add_node(mesh, wire->neighbor_id)) {
        return ESP_ERR_NO_MEM;
    }
    bool origin_restarted = false;
    if (origin->boot_id == 0) {
        origin->boot_id = wire->boot_id;
    } else if (origin->boot_id != wire->boot_id) {
        if (origin->previous_boot_id == wire->boot_id) {
            return ESP_ERR_NOT_FOUND;
        }
        origin->previous_boot_id = origin->boot_id;
        origin->boot_id = wire->boot_id;
        memset(origin->links, 0, sizeof(origin->links));
        origin_restarted = true;
    }
    mosaico_mesh_link_t *link =
        &origin->links[edge_index((mosaico_edge_t)wire->local_edge)];
    if (!origin_restarted && link->known &&
        !sequence_is_newer(wire->sequence, link->sequence)) {
        return ESP_ERR_NOT_FOUND;
    }
    const bool content_changed = origin_restarted || !link->known ||
        link->neighbor_id != wire->neighbor_id ||
        link->relative_rotation != wire->relative_rotation ||
        link->peer_edge != (mosaico_edge_t)wire->peer_edge ||
        link->active != ((wire->flags & MOSAICO_MESH_LINK_ACTIVE) != 0);
    *link = (mosaico_mesh_link_t) {
        .neighbor_id = wire->neighbor_id,
        .sequence = wire->sequence,
        .updated_ms = now_ms,
        .relative_rotation = wire->relative_rotation,
        .peer_edge = (mosaico_edge_t)wire->peer_edge,
        .active = (wire->flags & MOSAICO_MESH_LINK_ACTIVE) != 0,
        .known = true,
    };
    if (content_changed) {
        mesh->topology_version++;
        rebuild_topology(mesh);
    }
    if (topology_changed) {
        *topology_changed = content_changed;
    }
    if (forward) {
        *forward = *wire;
        if (forward->ttl > 0) {
            forward->ttl--;
        }
    }
    return ESP_OK;
}

bool mosaico_mesh_expire(
    mosaico_mesh_t *mesh,
    uint32_t now_ms,
    uint32_t stale_ms)
{
    if (!mesh || mesh->local_id == 0) {
        return false;
    }
    const uint32_t timeout = stale_ms ? stale_ms : MOSAICO_MESH_RECORD_STALE_MS;
    bool expired = false;
    for (size_t node = 0; node < mesh->node_count; ++node) {
        if (mesh->nodes[node].device_id == mesh->local_id) {
            continue;
        }
        for (size_t edge = 0; edge < MOSAICO_MESH_EDGE_COUNT; ++edge) {
            mosaico_mesh_link_t *link = &mesh->nodes[node].links[edge];
            if (link->known && link->active &&
                (uint32_t)(now_ms - link->updated_ms) >= timeout) {
                link->active = false;
                expired = true;
            }
        }
    }
    if (expired) {
        mesh->topology_version++;
        rebuild_topology(mesh);
    }
    const bool pruned = prune_stale_nodes(mesh, now_ms, timeout);
    if (pruned) {
        mesh->topology_version++;
    }
    return expired || pruned;
}

const mosaico_mesh_node_t *mosaico_mesh_get_node(
    const mosaico_mesh_t *mesh,
    uint64_t device_id)
{
    return mesh ? find_node_const(mesh, device_id) : NULL;
}

esp_err_t mosaico_mesh_get_path(
    const mosaico_mesh_t *mesh,
    uint64_t source_id,
    uint64_t destination_id,
    uint64_t *path,
    size_t path_capacity,
    size_t *path_length)
{
    if (!mesh || !path || !path_length || path_capacity == 0 ||
        source_id == 0 || destination_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t source = SIZE_MAX;
    size_t destination = SIZE_MAX;
    for (size_t i = 0; i < mesh->node_count; ++i) {
        if (mesh->nodes[i].device_id == source_id && mesh->nodes[i].pose_valid) {
            source = i;
        }
        if (mesh->nodes[i].device_id == destination_id && mesh->nodes[i].pose_valid) {
            destination = i;
        }
    }
    if (source == SIZE_MAX || destination == SIZE_MAX) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t queue[MOSAICO_MESH_MAX_NODES] = {0};
    size_t parent[MOSAICO_MESH_MAX_NODES];
    bool visited[MOSAICO_MESH_MAX_NODES] = {0};
    for (size_t i = 0; i < MOSAICO_MESH_MAX_NODES; ++i) {
        parent[i] = SIZE_MAX;
    }
    size_t head = 0;
    size_t tail = 0;
    queue[tail++] = source;
    visited[source] = true;
    while (head < tail && !visited[destination]) {
        const size_t current = queue[head++];
        for (size_t edge = 0; edge < MOSAICO_MESH_EDGE_COUNT; ++edge) {
            const mosaico_mesh_link_t *link = reciprocal_link(
                mesh, &mesh->nodes[current], edge);
            if (!link) {
                continue;
            }
            for (size_t next = 0; next < mesh->node_count; ++next) {
                if (!visited[next] &&
                    mesh->nodes[next].device_id == link->neighbor_id) {
                    visited[next] = true;
                    parent[next] = current;
                    queue[tail++] = next;
                    break;
                }
            }
        }
    }
    if (!visited[destination]) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t reverse[MOSAICO_MESH_MAX_NODES] = {0};
    size_t length = 0;
    for (size_t current = destination; current != SIZE_MAX;
         current = parent[current]) {
        reverse[length++] = current;
        if (current == source) {
            break;
        }
    }
    if (length > path_capacity) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < length; ++i) {
        path[i] = mesh->nodes[reverse[length - i - 1]].device_id;
    }
    *path_length = length;
    return ESP_OK;
}

esp_err_t mosaico_mesh_get_next_hop(
    const mosaico_mesh_t *mesh,
    uint64_t destination_id,
    uint64_t *next_hop_id,
    mosaico_edge_t *local_edge)
{
    if (!mesh || !next_hop_id || !local_edge) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t path[MOSAICO_MESH_MAX_NODES] = {0};
    size_t path_length = 0;
    esp_err_t ret = mosaico_mesh_get_path(
        mesh, mesh->local_id, destination_id, path,
        MOSAICO_MESH_MAX_NODES, &path_length);
    if (ret != ESP_OK) {
        return ret;
    }
    if (path_length < 2) {
        return ESP_ERR_INVALID_STATE;
    }
    const mosaico_mesh_node_t *local = find_node_const(mesh, mesh->local_id);
    for (size_t edge = 0; edge < MOSAICO_MESH_EDGE_COUNT; ++edge) {
        if (reciprocal_link(mesh, local, edge) &&
            local->links[edge].neighbor_id == path[1]) {
            *next_hop_id = path[1];
            *local_edge = (mosaico_edge_t)(edge + MOSAICO_EDGE_TOP);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

size_t mosaico_mesh_get_connected_ids(
    const mosaico_mesh_t *mesh,
    uint64_t *device_ids,
    size_t capacity)
{
    if (!mesh || !device_ids || capacity == 0) {
        return 0;
    }
    size_t count = 0;
    for (size_t i = 0; i < mesh->node_count && count < capacity; ++i) {
        if (mesh->nodes[i].pose_valid) {
            size_t insert = count;
            while (insert > 0 && device_ids[insert - 1] > mesh->nodes[i].device_id) {
                device_ids[insert] = device_ids[insert - 1];
                insert--;
            }
            device_ids[insert] = mesh->nodes[i].device_id;
            count++;
        }
    }
    return count;
}

static uint32_t topology_hash_byte(uint32_t hash, uint8_t value)
{
    return (hash ^ value) * 16777619U;
}

static uint32_t topology_hash_value(
    uint32_t hash,
    uint64_t value,
    size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i) {
        hash = topology_hash_byte(hash, (uint8_t)(value >> (i * 8U)));
    }
    return hash;
}

uint32_t mosaico_mesh_get_topology_id(const mosaico_mesh_t *mesh)
{
    if (!mesh) {
        return 0;
    }
    uint64_t connected[MOSAICO_MESH_MAX_NODES] = {0};
    const size_t count = mosaico_mesh_get_connected_ids(
        mesh, connected, MOSAICO_MESH_MAX_NODES);
    if (count == 0) {
        return 0;
    }
    uint32_t hash = topology_hash_value(2166136261U, count, sizeof(uint8_t));
    for (size_t i = 0; i < count; ++i) {
        hash = topology_hash_value(hash, connected[i], sizeof(connected[i]));
    }
    for (size_t i = 0; i < count; ++i) {
        const mosaico_mesh_node_t *node = find_node_const(mesh, connected[i]);
        for (size_t edge = 0; node && edge < MOSAICO_MESH_EDGE_COUNT; ++edge) {
            const mosaico_mesh_link_t *link = reciprocal_link(mesh, node, edge);
            if (!link || node->device_id >= link->neighbor_id) {
                continue;
            }
            hash = topology_hash_value(hash, node->device_id, sizeof(node->device_id));
            hash = topology_hash_value(hash, edge, sizeof(uint8_t));
            hash = topology_hash_value(hash, link->neighbor_id, sizeof(link->neighbor_id));
            hash = topology_hash_value(hash, link->peer_edge, sizeof(uint8_t));
            hash = topology_hash_value(
                hash, link->relative_rotation, sizeof(link->relative_rotation));
        }
    }
    return hash ? hash : 1U;
}

uint8_t mosaico_mesh_get_conflicts(const mosaico_mesh_t *mesh)
{
    return mesh ? mesh->conflict_flags : MOSAICO_MESH_CONFLICT_NONE;
}

size_t mosaico_mesh_get_traversal_order(
    const mosaico_mesh_t *mesh,
    uint64_t *device_ids,
    size_t capacity)
{
    if (!mesh || !device_ids || capacity == 0) {
        return 0;
    }
    uint64_t connected[MOSAICO_MESH_MAX_NODES] = {0};
    const size_t count = mosaico_mesh_get_connected_ids(
        mesh, connected, MOSAICO_MESH_MAX_NODES);
    if (count == 0 || capacity < count) {
        return 0;
    }
    if (count == 1) {
        device_ids[0] = connected[0];
        return 1;
    }

    uint64_t start_id = 0;
    uint64_t end_id = 0;
    size_t diameter_length = 0;
    uint64_t path[MOSAICO_MESH_MAX_NODES] = {0};
    for (size_t first = 0; first + 1 < count; ++first) {
        for (size_t second = first + 1; second < count; ++second) {
            size_t path_length = 0;
            if (mosaico_mesh_get_path(
                    mesh, connected[first], connected[second], path,
                    MOSAICO_MESH_MAX_NODES, &path_length) == ESP_OK &&
                path_length > diameter_length) {
                diameter_length = path_length;
                start_id = connected[first];
                end_id = connected[second];
            }
        }
    }
    if (!start_id || !end_id) {
        return 0;
    }

    bool visited[MOSAICO_MESH_MAX_NODES] = {0};
    size_t start_index = 0;
    size_t end_index = 0;
    for (size_t i = 0; i < count; ++i) {
        if (connected[i] == start_id) {
            start_index = i;
        }
        if (connected[i] == end_id) {
            end_index = i;
        }
    }
    device_ids[0] = start_id;
    visited[start_index] = true;
    uint64_t current_id = start_id;
    for (size_t output = 1; output < count; ++output) {
        if (output == count - 1) {
            device_ids[output] = end_id;
            visited[end_index] = true;
            break;
        }
        size_t selected = SIZE_MAX;
        size_t selected_distance = SIZE_MAX;
        for (size_t candidate = 0; candidate < count; ++candidate) {
            if (visited[candidate] || candidate == end_index) {
                continue;
            }
            size_t path_length = 0;
            if (mosaico_mesh_get_path(
                    mesh, current_id, connected[candidate], path,
                    MOSAICO_MESH_MAX_NODES, &path_length) != ESP_OK) {
                continue;
            }
            if (path_length < selected_distance) {
                selected = candidate;
                selected_distance = path_length;
            }
        }
        if (selected == SIZE_MAX) {
            return 0;
        }
        visited[selected] = true;
        current_id = connected[selected];
        device_ids[output] = current_id;
    }
    return count;
}
