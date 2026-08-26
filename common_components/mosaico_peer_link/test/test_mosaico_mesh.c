/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_mesh.h"
#include "mosaico_topology.h"
#include "unity.h"

#include <string.h>

#define GRID_NODE_COUNT 9
#define FULL_GRID_SIDE 4
#define FULL_GRID_NODE_COUNT (FULL_GRID_SIDE * FULL_GRID_SIDE)

typedef struct {
    int16_t x;
    int16_t y;
    uint16_t rotation;
} test_pose_t;

static void distribute_link(
    mosaico_mesh_t *meshes,
    size_t mesh_count,
    size_t origin,
    mosaico_edge_t origin_edge,
    size_t peer,
    mosaico_edge_t peer_edge,
    uint32_t now_ms)
{
    mosaico_mesh_wire_link_t wire = {0};
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_attach(
        &meshes[origin], origin_edge, meshes[peer].local_id, peer_edge,
        now_ms, &wire));
    for (size_t destination = 0; destination < mesh_count; ++destination) {
        if (destination == origin) {
            continue;
        }
        bool changed = false;
        TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_receive(
            &meshes[destination], &wire, now_ms, NULL, &changed));
        TEST_ASSERT_TRUE(changed);
    }
}

static void connect_pair(
    mosaico_mesh_t *meshes,
    size_t mesh_count,
    size_t first,
    mosaico_edge_t first_edge,
    size_t second,
    mosaico_edge_t second_edge,
    uint32_t now_ms)
{
    distribute_link(
        meshes, mesh_count, first, first_edge, second, second_edge, now_ms);
    distribute_link(
        meshes, mesh_count, second, second_edge, first, first_edge, now_ms);
}

static mosaico_edge_t opposite_edge(mosaico_edge_t edge)
{
    return (mosaico_edge_t)(
        ((edge - MOSAICO_EDGE_TOP + 2) % 4) + MOSAICO_EDGE_TOP);
}

static mosaico_edge_t global_edge_between(
    const test_pose_t *first,
    const test_pose_t *second)
{
    if (second->x == first->x && second->y == first->y - 1) {
        return MOSAICO_EDGE_TOP;
    }
    if (second->x == first->x + 1 && second->y == first->y) {
        return MOSAICO_EDGE_RIGHT;
    }
    if (second->x == first->x && second->y == first->y + 1) {
        return MOSAICO_EDGE_BOTTOM;
    }
    if (second->x == first->x - 1 && second->y == first->y) {
        return MOSAICO_EDGE_LEFT;
    }
    return MOSAICO_EDGE_NONE;
}

static void build_pose_mesh(
    mosaico_mesh_t *meshes,
    const test_pose_t *poses,
    size_t count)
{
    TEST_ASSERT_LESS_OR_EQUAL(MOSAICO_MESH_MAX_NODES, count);
    for (size_t i = 0; i < count; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_init(&meshes[i], i + 1));
    }
    uint32_t now_ms = 100;
    for (size_t first = 0; first < count; ++first) {
        for (size_t second = first + 1; second < count; ++second) {
            const mosaico_edge_t first_global = global_edge_between(
                &poses[first], &poses[second]);
            if (first_global == MOSAICO_EDGE_NONE) {
                continue;
            }
            mosaico_edge_t first_local = MOSAICO_EDGE_NONE;
            mosaico_edge_t second_local = MOSAICO_EDGE_NONE;
            TEST_ASSERT_EQUAL(ESP_OK, mosaico_edge_rotate(
                first_global, poses[first].rotation, &first_local));
            TEST_ASSERT_EQUAL(ESP_OK, mosaico_edge_rotate(
                opposite_edge(first_global), poses[second].rotation,
                &second_local));
            connect_pair(
                meshes, count, first, first_local, second, second_local,
                now_ms++);
        }
    }
}

static void assert_pose_mesh(
    const test_pose_t *poses,
    size_t count)
{
    static mosaico_mesh_t meshes[MOSAICO_MESH_MAX_NODES];
    memset(meshes, 0, sizeof(meshes));
    build_pose_mesh(meshes, poses, count);
    const uint32_t topology_id = mosaico_mesh_get_topology_id(&meshes[0]);
    TEST_ASSERT_NOT_EQUAL(0, topology_id);
    for (size_t observer = 0; observer < count; ++observer) {
        TEST_ASSERT_EQUAL_UINT32(count, meshes[observer].connected_count);
        TEST_ASSERT_EQUAL_UINT8(
            MOSAICO_MESH_CONFLICT_NONE,
            mosaico_mesh_get_conflicts(&meshes[observer]));
        TEST_ASSERT_EQUAL_HEX32(
            topology_id, mosaico_mesh_get_topology_id(&meshes[observer]));
        for (size_t node_index = 0; node_index < count; ++node_index) {
            const mosaico_mesh_node_t *node = mosaico_mesh_get_node(
                &meshes[observer], node_index + 1);
            TEST_ASSERT_NOT_NULL(node);
            TEST_ASSERT_TRUE(node->pose_valid);
            TEST_ASSERT_EQUAL_INT16(poses[node_index].x, node->x);
            TEST_ASSERT_EQUAL_INT16(poses[node_index].y, node->y);
            TEST_ASSERT_EQUAL_UINT16(
                poses[node_index].rotation, node->rotation_degrees);
        }
    }
}

static void build_grid(mosaico_mesh_t meshes[GRID_NODE_COUNT])
{
    for (size_t i = 0; i < GRID_NODE_COUNT; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_init(meshes + i, i + 1));
    }
    uint32_t now_ms = 100;
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 2; ++column) {
            const size_t left = row * 3 + column;
            connect_pair(meshes, GRID_NODE_COUNT, left, MOSAICO_EDGE_RIGHT,
                         left + 1, MOSAICO_EDGE_LEFT, now_ms++);
        }
    }
    for (size_t row = 0; row < 2; ++row) {
        for (size_t column = 0; column < 3; ++column) {
            const size_t top = row * 3 + column;
            connect_pair(meshes, GRID_NODE_COUNT, top, MOSAICO_EDGE_BOTTOM,
                         top + 3, MOSAICO_EDGE_TOP, now_ms++);
        }
    }
}

static void build_full_grid(
    mosaico_mesh_t meshes[FULL_GRID_NODE_COUNT])
{
    for (size_t i = 0; i < FULL_GRID_NODE_COUNT; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_init(meshes + i, i + 1));
    }
    uint32_t now_ms = 100;
    size_t seam_count = 0;
    for (size_t row = 0; row < FULL_GRID_SIDE; ++row) {
        for (size_t column = 0; column + 1 < FULL_GRID_SIDE; ++column) {
            const size_t left = row * FULL_GRID_SIDE + column;
            connect_pair(
                meshes, FULL_GRID_NODE_COUNT, left, MOSAICO_EDGE_RIGHT,
                left + 1, MOSAICO_EDGE_LEFT, now_ms++);
            seam_count++;
        }
    }
    for (size_t row = 0; row + 1 < FULL_GRID_SIDE; ++row) {
        for (size_t column = 0; column < FULL_GRID_SIDE; ++column) {
            const size_t top = row * FULL_GRID_SIDE + column;
            connect_pair(
                meshes, FULL_GRID_NODE_COUNT, top, MOSAICO_EDGE_BOTTOM,
                top + FULL_GRID_SIDE, MOSAICO_EDGE_TOP, now_ms++);
            seam_count++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(24, seam_count);
}

static void distribute_detach(
    mosaico_mesh_t *meshes,
    size_t mesh_count,
    size_t origin,
    mosaico_edge_t origin_edge,
    size_t peer,
    uint32_t now_ms)
{
    mosaico_mesh_wire_link_t wire = {0};
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_detach(
        &meshes[origin], origin_edge, meshes[peer].local_id, now_ms, &wire));
    for (size_t destination = 0; destination < mesh_count; ++destination) {
        if (destination == origin) {
            continue;
        }
        TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_receive(
            &meshes[destination], &wire, now_ms, NULL, NULL));
    }
}

TEST_CASE("nine-node grid converges with stable coordinates", "[mosaico_mesh]")
{
    static mosaico_mesh_t meshes[GRID_NODE_COUNT];
    memset(meshes, 0, sizeof(meshes));
    build_grid(meshes);
    const uint32_t topology_id = mosaico_mesh_get_topology_id(&meshes[0]);
    TEST_ASSERT_NOT_EQUAL(0, topology_id);
    for (size_t observer = 0; observer < GRID_NODE_COUNT; ++observer) {
        TEST_ASSERT_EQUAL_UINT32(GRID_NODE_COUNT, meshes[observer].connected_count);
        TEST_ASSERT_EQUAL_UINT64(1, meshes[observer].root_id);
        TEST_ASSERT_FALSE(meshes[observer].orientation_conflict);
        TEST_ASSERT_EQUAL_HEX32(
            topology_id, mosaico_mesh_get_topology_id(&meshes[observer]));
        for (uint64_t id = 1; id <= GRID_NODE_COUNT; ++id) {
            const mosaico_mesh_node_t *node =
                mosaico_mesh_get_node(&meshes[observer], id);
            TEST_ASSERT_NOT_NULL(node);
            TEST_ASSERT_TRUE(node->pose_valid);
            TEST_ASSERT_EQUAL_INT16((int16_t)((id - 1) % 3), node->x);
            TEST_ASSERT_EQUAL_INT16((int16_t)((id - 1) / 3), node->y);
            TEST_ASSERT_EQUAL_UINT16(0, node->rotation_degrees);
        }
    }
}

TEST_CASE("full-capacity grid converges routes and survives one seam loss",
          "[mosaico_mesh]")
{
    static mosaico_mesh_t meshes[FULL_GRID_NODE_COUNT];
    memset(meshes, 0, sizeof(meshes));
    build_full_grid(meshes);

    const uint32_t topology_id = mosaico_mesh_get_topology_id(&meshes[0]);
    TEST_ASSERT_NOT_EQUAL(0, topology_id);
    for (size_t observer = 0; observer < FULL_GRID_NODE_COUNT; ++observer) {
        TEST_ASSERT_EQUAL_UINT32(
            MOSAICO_MESH_MAX_NODES, meshes[observer].node_count);
        TEST_ASSERT_EQUAL_UINT32(
            FULL_GRID_NODE_COUNT, meshes[observer].connected_count);
        TEST_ASSERT_EQUAL_HEX32(
            topology_id, mosaico_mesh_get_topology_id(&meshes[observer]));
        TEST_ASSERT_EQUAL_UINT8(
            MOSAICO_MESH_CONFLICT_NONE,
            mosaico_mesh_get_conflicts(&meshes[observer]));
    }

    uint64_t path[MOSAICO_MESH_MAX_NODES] = {0};
    size_t path_length = 0;
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_get_path(
        &meshes[0], 1, FULL_GRID_NODE_COUNT, path,
        MOSAICO_MESH_MAX_NODES, &path_length));
    TEST_ASSERT_EQUAL_UINT32(7, path_length);
    TEST_ASSERT_EQUAL_UINT64(1, path[0]);
    TEST_ASSERT_EQUAL_UINT64(FULL_GRID_NODE_COUNT, path[path_length - 1]);

    uint64_t traversal[MOSAICO_MESH_MAX_NODES] = {0};
    TEST_ASSERT_EQUAL_UINT32(
        FULL_GRID_NODE_COUNT,
        mosaico_mesh_get_traversal_order(
            &meshes[0], traversal, MOSAICO_MESH_MAX_NODES));
    for (size_t i = 0; i < FULL_GRID_NODE_COUNT; ++i) {
        for (size_t j = i + 1; j < FULL_GRID_NODE_COUNT; ++j) {
            TEST_ASSERT_NOT_EQUAL(traversal[i], traversal[j]);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(
        0, mosaico_mesh_get_traversal_order(
               &meshes[0], traversal, MOSAICO_MESH_MAX_NODES - 1));

    const mosaico_mesh_wire_link_t extra_node = {
        .version = 1,
        .flags = MOSAICO_MESH_LINK_ACTIVE,
        .ttl = MOSAICO_MESH_DEFAULT_RECORD_TTL,
        .local_edge = MOSAICO_EDGE_TOP,
        .peer_edge = MOSAICO_EDGE_BOTTOM,
        .sequence = 1,
        .boot_id = 17,
        .origin_id = 17,
        .neighbor_id = 1,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, mosaico_mesh_receive(
        &meshes[0], &extra_node, 200, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(MOSAICO_MESH_MAX_NODES, meshes[0].node_count);

    distribute_detach(
        meshes, FULL_GRID_NODE_COUNT, 0, MOSAICO_EDGE_RIGHT, 1, 300);
    distribute_detach(
        meshes, FULL_GRID_NODE_COUNT, 1, MOSAICO_EDGE_LEFT, 0, 301);
    const uint32_t rerouted_topology_id =
        mosaico_mesh_get_topology_id(&meshes[0]);
    TEST_ASSERT_NOT_EQUAL(topology_id, rerouted_topology_id);
    for (size_t observer = 0; observer < FULL_GRID_NODE_COUNT; ++observer) {
        TEST_ASSERT_EQUAL_UINT32(
            FULL_GRID_NODE_COUNT, meshes[observer].connected_count);
        TEST_ASSERT_EQUAL_HEX32(
            rerouted_topology_id,
            mosaico_mesh_get_topology_id(&meshes[observer]));
    }
    memset(path, 0, sizeof(path));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_get_path(
        &meshes[0], 1, FULL_GRID_NODE_COUNT, path,
        MOSAICO_MESH_MAX_NODES, &path_length));
    TEST_ASSERT_EQUAL_UINT32(7, path_length);
    TEST_ASSERT_EQUAL_UINT64(5, path[1]);
}

TEST_CASE("nine-node grid provides a shortest routed path", "[mosaico_mesh]")
{
    static mosaico_mesh_t meshes[GRID_NODE_COUNT];
    memset(meshes, 0, sizeof(meshes));
    build_grid(meshes);
    uint64_t path[MOSAICO_MESH_MAX_NODES] = {0};
    size_t length = 0;
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_get_path(
        &meshes[0], 1, 9, path, MOSAICO_MESH_MAX_NODES, &length));
    TEST_ASSERT_EQUAL_UINT32(5, length);
    TEST_ASSERT_EQUAL_UINT64(1, path[0]);
    TEST_ASSERT_EQUAL_UINT64(9, path[length - 1]);

    uint64_t next_hop = 0;
    mosaico_edge_t local_edge = MOSAICO_EDGE_NONE;
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_get_next_hop(
        &meshes[0], 9, &next_hop, &local_edge));
    TEST_ASSERT_TRUE(next_hop == 2 || next_hop == 4);
    TEST_ASSERT_TRUE(local_edge == MOSAICO_EDGE_RIGHT ||
                     local_edge == MOSAICO_EDGE_BOTTOM);
}

TEST_CASE("three-node traversal follows physical endpoints", "[mosaico_mesh]")
{
    enum { NODE_COUNT = 3 };
    mosaico_mesh_t meshes[NODE_COUNT] = {0};
    const uint64_t ids[NODE_COUNT] = {30, 10, 20};
    for (size_t i = 0; i < NODE_COUNT; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_init(&meshes[i], ids[i]));
    }
    connect_pair(
        meshes, NODE_COUNT, 0, MOSAICO_EDGE_RIGHT,
        1, MOSAICO_EDGE_LEFT, 100);
    connect_pair(
        meshes, NODE_COUNT, 1, MOSAICO_EDGE_BOTTOM,
        2, MOSAICO_EDGE_TOP, 101);

    const uint64_t expected[NODE_COUNT] = {20, 10, 30};
    for (size_t observer = 0; observer < NODE_COUNT; ++observer) {
        uint64_t order[NODE_COUNT] = {0};
        TEST_ASSERT_EQUAL_UINT32(NODE_COUNT, mosaico_mesh_get_traversal_order(
            &meshes[observer], order, NODE_COUNT));
        TEST_ASSERT_EQUAL_UINT64_ARRAY(expected, order, NODE_COUNT);
    }
}

TEST_CASE("nine-node traversal forms a deterministic sweep", "[mosaico_mesh]")
{
    static mosaico_mesh_t meshes[GRID_NODE_COUNT];
    memset(meshes, 0, sizeof(meshes));
    build_grid(meshes);
    const uint64_t expected[GRID_NODE_COUNT] = {1, 2, 3, 6, 5, 4, 7, 8, 9};
    for (size_t observer = 0; observer < GRID_NODE_COUNT; ++observer) {
        uint64_t order[GRID_NODE_COUNT] = {0};
        TEST_ASSERT_EQUAL_UINT32(
            GRID_NODE_COUNT,
            mosaico_mesh_get_traversal_order(
                &meshes[observer], order, GRID_NODE_COUNT));
        TEST_ASSERT_EQUAL_UINT64_ARRAY(expected, order, GRID_NODE_COUNT);
    }
}

TEST_CASE("nine-node arbitrary shapes preserve physical rotations", "[mosaico_mesh]")
{
    static const test_pose_t shapes[][GRID_NODE_COUNT] = {
        {
            {0, 0, 0}, {1, 0, 180}, {2, 0, 0}, {3, 0, 180},
            {4, 0, 0}, {5, 0, 180}, {6, 0, 0}, {7, 0, 180},
            {8, 0, 0},
        },
        {
            {0, 0, 0}, {1, 0, 180}, {2, 0, 0}, {3, 0, 180},
            {4, 0, 0}, {0, 1, 180}, {0, 2, 0}, {0, 3, 180},
            {0, 4, 0},
        },
        {
            {0, 0, 0}, {1, 0, 180}, {2, 0, 180}, {3, 0, 0},
            {4, 0, 180}, {2, 1, 0}, {2, 2, 0}, {1, 2, 180},
            {3, 2, 180},
        },
        {
            {0, 0, 0}, {1, 0, 180}, {1, 1, 0}, {2, 1, 180},
            {2, 2, 180}, {3, 2, 0}, {3, 3, 180}, {4, 3, 180},
            {4, 4, 0},
        },
    };
    for (size_t shape = 0; shape < sizeof(shapes) / sizeof(shapes[0]); ++shape) {
        assert_pose_mesh(shapes[shape], GRID_NODE_COUNT);
    }
}

TEST_CASE("mesh reports overlapping device coordinates", "[mosaico_mesh]")
{
    enum { NODE_COUNT = 5 };
    mosaico_mesh_t meshes[NODE_COUNT] = {0};
    for (size_t i = 0; i < NODE_COUNT; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_init(&meshes[i], i + 1));
    }
    connect_pair(meshes, NODE_COUNT, 0, MOSAICO_EDGE_RIGHT,
                 1, MOSAICO_EDGE_LEFT, 100);
    connect_pair(meshes, NODE_COUNT, 1, MOSAICO_EDGE_BOTTOM,
                 2, MOSAICO_EDGE_TOP, 101);
    connect_pair(meshes, NODE_COUNT, 2, MOSAICO_EDGE_LEFT,
                 3, MOSAICO_EDGE_RIGHT, 102);
    connect_pair(meshes, NODE_COUNT, 3, MOSAICO_EDGE_TOP,
                 4, MOSAICO_EDGE_BOTTOM, 103);

    for (size_t observer = 0; observer < NODE_COUNT; ++observer) {
        TEST_ASSERT_BITS_HIGH(
            MOSAICO_MESH_CONFLICT_POSITION,
            mosaico_mesh_get_conflicts(&meshes[observer]));
        TEST_ASSERT_TRUE(meshes[observer].orientation_conflict);
    }
}

TEST_CASE("mesh rejects one-sided and stale links", "[mosaico_mesh]")
{
    mosaico_mesh_t mesh = {0};
    mosaico_mesh_t peer = {0};
    mosaico_mesh_wire_link_t wire = {0};
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_init(&mesh, 1));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_init(&peer, 2));
    const uint32_t isolated_topology_id = mosaico_mesh_get_topology_id(&mesh);
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_attach(
        &mesh, MOSAICO_EDGE_RIGHT, 2, MOSAICO_EDGE_LEFT, 100, &wire));
    TEST_ASSERT_EQUAL_UINT32(1, mesh.connected_count);

    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_attach(
        &peer, MOSAICO_EDGE_LEFT, 1, MOSAICO_EDGE_RIGHT, 101, &wire));
    bool changed = false;
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_receive(
        &mesh, &wire, 101, NULL, &changed));
    TEST_ASSERT_EQUAL_UINT32(2, mesh.connected_count);
    TEST_ASSERT_NOT_EQUAL(
        isolated_topology_id, mosaico_mesh_get_topology_id(&mesh));
    TEST_ASSERT_TRUE(mosaico_mesh_expire(&mesh, 6101, 5000));
    TEST_ASSERT_EQUAL_UINT32(1, mesh.connected_count);
    TEST_ASSERT_EQUAL_HEX32(
        isolated_topology_id, mosaico_mesh_get_topology_id(&mesh));
}

TEST_CASE("same-edge mesh link produces reciprocal rotation", "[mosaico_mesh]")
{
    mosaico_mesh_t first = {0};
    mosaico_mesh_t second = {0};
    mosaico_mesh_wire_link_t first_wire = {0};
    mosaico_mesh_wire_link_t second_wire = {0};
    bool changed = false;
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_init(&first, 1));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_init(&second, 2));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_attach(
        &first, MOSAICO_EDGE_RIGHT, 2, MOSAICO_EDGE_RIGHT, 100, &first_wire));
    TEST_ASSERT_EQUAL_UINT16(180, first_wire.relative_rotation);
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_attach(
        &second, MOSAICO_EDGE_RIGHT, 1, MOSAICO_EDGE_RIGHT, 100, &second_wire));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_receive(
        &first, &second_wire, 100, NULL, &changed));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_receive(
        &second, &first_wire, 100, NULL, &changed));

    const mosaico_mesh_node_t *second_from_first =
        mosaico_mesh_get_node(&first, 2);
    TEST_ASSERT_NOT_NULL(second_from_first);
    TEST_ASSERT_TRUE(second_from_first->pose_valid);
    TEST_ASSERT_EQUAL_UINT16(180, second_from_first->rotation_degrees);
    TEST_ASSERT_EQUAL_INT16(1, second_from_first->x);
    TEST_ASSERT_EQUAL_INT16(0, second_from_first->y);
}

TEST_CASE("mesh rejects a cross-axis physical link", "[mosaico_mesh]")
{
    mosaico_mesh_t mesh = {0};
    mosaico_mesh_wire_link_t wire = {0};
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_init(&mesh, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mosaico_mesh_attach(
        &mesh, MOSAICO_EDGE_TOP, 2, MOSAICO_EDGE_RIGHT, 100, &wire));
    TEST_ASSERT_EQUAL_UINT32(0, mesh.connected_count);
}

TEST_CASE("mesh rejects a noncanonical link record", "[mosaico_mesh]")
{
    mosaico_mesh_t mesh = {0};
    mosaico_mesh_t peer = {0};
    mosaico_mesh_wire_link_t wire = {0};
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_init(&mesh, 1));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_init(&peer, 2));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_mesh_attach(
        &peer, MOSAICO_EDGE_RIGHT, 1, MOSAICO_EDGE_RIGHT, 100, &wire));
    wire.relative_rotation = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mosaico_mesh_receive(
        &mesh, &wire, 100, NULL, NULL));
}

TEST_CASE("mesh accepts a restarted origin with a reset sequence", "[mosaico_mesh]")
{
    mosaico_mesh_t observer = {0};
    mosaico_mesh_t peer = {0};
    mosaico_mesh_t restarted_peer = {0};
    mosaico_mesh_wire_link_t observer_wire = {0};
    mosaico_mesh_wire_link_t old_wire = {0};
    mosaico_mesh_wire_link_t restarted_wire = {0};
    bool changed = false;

    TEST_ESP_OK(mosaico_mesh_init_with_boot_id(&observer, 1, 0x11));
    TEST_ESP_OK(mosaico_mesh_init_with_boot_id(&peer, 2, 0x22));
    TEST_ESP_OK(mosaico_mesh_attach(
        &observer, MOSAICO_EDGE_RIGHT, 2, MOSAICO_EDGE_LEFT,
        100, &observer_wire));
    TEST_ESP_OK(mosaico_mesh_attach(
        &peer, MOSAICO_EDGE_LEFT, 1, MOSAICO_EDGE_RIGHT, 100, &old_wire));
    for (uint32_t now_ms = 101; now_ms < 120; ++now_ms) {
        TEST_ESP_OK(mosaico_mesh_refresh(
            &peer, MOSAICO_EDGE_LEFT, now_ms, &old_wire));
    }
    TEST_ESP_OK(mosaico_mesh_receive(
        &observer, &old_wire, 120, NULL, &changed));
    TEST_ASSERT_EQUAL_UINT32(2, observer.connected_count);

    TEST_ESP_OK(mosaico_mesh_init_with_boot_id(&restarted_peer, 2, 0x23));
    TEST_ESP_OK(mosaico_mesh_attach(
        &restarted_peer, MOSAICO_EDGE_LEFT, 1, MOSAICO_EDGE_RIGHT,
        121, &restarted_wire));
    TEST_ASSERT_LESS_THAN_UINT32(old_wire.sequence, restarted_wire.sequence);
    TEST_ESP_OK(mosaico_mesh_receive(
        &observer, &restarted_wire, 121, NULL, &changed));
    const mosaico_mesh_node_t *node = mosaico_mesh_get_node(&observer, 2);
    TEST_ASSERT_NOT_NULL(node);
    TEST_ASSERT_EQUAL_HEX32(0x23, node->boot_id);
    TEST_ASSERT_EQUAL_UINT32(restarted_wire.sequence,
                             node->links[MOSAICO_EDGE_LEFT - MOSAICO_EDGE_TOP].sequence);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, mosaico_mesh_receive(
        &observer, &old_wire, 122, NULL, NULL));
}

TEST_CASE("mesh recycles stale unreachable node slots", "[mosaico_mesh]")
{
    mosaico_mesh_t mesh = {0};
    TEST_ESP_OK(mosaico_mesh_init_with_boot_id(&mesh, 1, 0x11));
    for (uint64_t id = 2; id <= MOSAICO_MESH_MAX_NODES; ++id) {
        const mosaico_mesh_wire_link_t wire = {
            .version = 1,
            .ttl = MOSAICO_MESH_DEFAULT_RECORD_TTL,
            .local_edge = MOSAICO_EDGE_TOP,
            .peer_edge = MOSAICO_EDGE_BOTTOM,
            .sequence = 1,
            .boot_id = (uint32_t)id,
            .origin_id = id,
            .neighbor_id = 1,
        };
        TEST_ESP_OK(mosaico_mesh_receive(&mesh, &wire, 100, NULL, NULL));
    }
    TEST_ASSERT_EQUAL_UINT32(MOSAICO_MESH_MAX_NODES, mesh.node_count);
    TEST_ASSERT_TRUE(mosaico_mesh_expire(&mesh, 6000, 5000));
    TEST_ASSERT_EQUAL_UINT32(1, mesh.node_count);

    const mosaico_mesh_wire_link_t new_wire = {
        .version = 1,
        .ttl = MOSAICO_MESH_DEFAULT_RECORD_TTL,
        .local_edge = MOSAICO_EDGE_TOP,
        .peer_edge = MOSAICO_EDGE_BOTTOM,
        .sequence = 1,
        .boot_id = 0x17,
        .origin_id = 17,
        .neighbor_id = 1,
    };
    TEST_ESP_OK(mosaico_mesh_receive(&mesh, &new_wire, 6001, NULL, NULL));
}
