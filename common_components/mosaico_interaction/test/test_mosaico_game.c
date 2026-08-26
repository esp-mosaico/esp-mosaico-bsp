/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_game.h"
#include "mosaico_topology.h"
#include "unity.h"

#define DEVICE_A UINT64_C(0x020000000001)
#define DEVICE_B UINT64_C(0x020000000002)

TEST_CASE("edge pairs resolve the follower display rotation", "[mosaico_topology]")
{
    static const mosaico_edge_t edges[] = {
        MOSAICO_EDGE_TOP,
        MOSAICO_EDGE_RIGHT,
        MOSAICO_EDGE_BOTTOM,
        MOSAICO_EDGE_LEFT,
    };
    static const struct {
        mosaico_edge_t anchor;
        mosaico_edge_t follower;
        uint16_t expected_rotation;
    } cases[] = {
        {MOSAICO_EDGE_TOP, MOSAICO_EDGE_BOTTOM, 0},
        {MOSAICO_EDGE_TOP, MOSAICO_EDGE_TOP, 180},
        {MOSAICO_EDGE_RIGHT, MOSAICO_EDGE_LEFT, 0},
        {MOSAICO_EDGE_RIGHT, MOSAICO_EDGE_RIGHT, 180},
        {MOSAICO_EDGE_BOTTOM, MOSAICO_EDGE_BOTTOM, 180},
        {MOSAICO_EDGE_LEFT, MOSAICO_EDGE_LEFT, 180},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint16_t rotation = UINT16_MAX;
        TEST_ASSERT_EQUAL(ESP_OK, mosaico_topology_resolve_display_rotation(
            cases[i].anchor, cases[i].follower, &rotation));
        TEST_ASSERT_EQUAL_UINT16(cases[i].expected_rotation, rotation);

        mosaico_edge_t global_edge = MOSAICO_EDGE_NONE;
        TEST_ASSERT_EQUAL(ESP_OK, mosaico_edge_rotate(
            cases[i].follower, (uint16_t)((360U - rotation) % 360U),
            &global_edge));
        TEST_ASSERT_EQUAL((cases[i].anchor + 1) % 4 + 1, global_edge);
    }

    for (size_t anchor = 0; anchor < sizeof(edges) / sizeof(edges[0]); ++anchor) {
        for (size_t follower = 0; follower < sizeof(edges) / sizeof(edges[0]); ++follower) {
            uint16_t rotation = UINT16_MAX;
            mosaico_edge_t global_edge = MOSAICO_EDGE_NONE;
            const bool legal = mosaico_edges_can_connect(
                edges[anchor], edges[follower]);
            TEST_ASSERT_EQUAL(legal ? ESP_OK : ESP_ERR_INVALID_ARG,
                              mosaico_topology_resolve_display_rotation(
                                  edges[anchor], edges[follower], &rotation));
            if (!legal) {
                continue;
            }
            TEST_ASSERT_TRUE(rotation == 0 || rotation == 180);
            TEST_ASSERT_EQUAL(ESP_OK, mosaico_edge_rotate(
                edges[follower], (uint16_t)((360U - rotation) % 360U),
                &global_edge));
            TEST_ASSERT_EQUAL(edges[(anchor + 2) % 4], global_edge);
        }
    }
}

TEST_CASE("topology rejects cross-axis physical pairs", "[mosaico_topology]")
{
    mosaico_topology_t topology;
    mosaico_topology_init(&topology);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mosaico_topology_attach(
        &topology, MOSAICO_EDGE_TOP, DEVICE_B, MOSAICO_EDGE_LEFT, 90));
    TEST_ASSERT_FALSE(mosaico_edges_can_connect(
        MOSAICO_EDGE_BOTTOM, MOSAICO_EDGE_RIGHT));
}

TEST_CASE("physical edges map back into a rotated display", "[mosaico_topology]")
{
    mosaico_edge_t display_edge = MOSAICO_EDGE_NONE;
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_edge_rotate(
        MOSAICO_EDGE_RIGHT, 180, &display_edge));
    TEST_ASSERT_EQUAL(MOSAICO_EDGE_LEFT, display_edge);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mosaico_edge_rotate(
        MOSAICO_EDGE_RIGHT, 45, &display_edge));
}

TEST_CASE("energy transfer supports a same-edge session", "[mosaico_game]")
{
    mosaico_energy_transfer_t a = {0};
    mosaico_energy_transfer_t b = {0};
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_init(&a, DEVICE_A, 1000));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_init(&b, DEVICE_B, 1000));

    TEST_ASSERT_EQUAL(ESP_OK,
        mosaico_energy_transfer_attach(&a, DEVICE_B, 0x100,
                                       MOSAICO_EDGE_TOP, MOSAICO_EDGE_TOP, 2000));
    TEST_ASSERT_EQUAL(ESP_OK,
        mosaico_energy_transfer_attach(&b, DEVICE_A, 0x100,
                                       MOSAICO_EDGE_TOP, MOSAICO_EDGE_TOP, 2000));
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_SENDING, a.phase);
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_WAIT_HANDOFF, b.phase);

    mosaico_energy_event_t event = {0};
    mosaico_energy_event_t response = {0};
    TEST_ASSERT_TRUE(mosaico_energy_transfer_update(&a, 3000, &event));
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_EVENT_HANDOFF, event.kind);
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_receive(
        &b, event.kind, event.hop, 3000, &response));
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_RECEIVING, b.phase);
    TEST_ASSERT_TRUE(mosaico_energy_transfer_update(&b, 4000, &event));
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_EVENT_COMPLETE, event.kind);
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_receive(
        &a, event.kind, event.hop, 4000, &response));
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_SENT, a.phase);
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_RECEIVED, b.phase);
}

TEST_CASE("energy transfer reverses role for odd session", "[mosaico_game]")
{
    mosaico_energy_transfer_t a = {0};
    mosaico_energy_transfer_t b = {0};
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_init(&a, DEVICE_A, 1000));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_init(&b, DEVICE_B, 1000));

    TEST_ASSERT_EQUAL(ESP_OK,
        mosaico_energy_transfer_attach(&a, DEVICE_B, 0x101, 1, 3, 0));
    TEST_ASSERT_EQUAL(ESP_OK,
        mosaico_energy_transfer_attach(&b, DEVICE_A, 0x101, 3, 1, 0));
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_WAIT_HANDOFF, a.phase);
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_SENDING, b.phase);
}

TEST_CASE("energy transfer can be restarted with an explicit chain role", "[mosaico_game]")
{
    mosaico_energy_transfer_t transfer = {0};
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_init(&transfer, DEVICE_A, 1000));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_attach(
        &transfer, DEVICE_B, 0x100, MOSAICO_EDGE_RIGHT, MOSAICO_EDGE_LEFT, 0));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_begin(
        &transfer, false, 2, 50));
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_WAIT_HANDOFF, transfer.phase);
    TEST_ASSERT_EQUAL_UINT32(2, transfer.hop);
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_begin(
        &transfer, true, 3, 100));
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_SENDING, transfer.phase);
    TEST_ASSERT_EQUAL_UINT32(100, transfer.started_ms);
}

TEST_CASE("energy transfer can connect without starting an animation", "[mosaico_game]")
{
    mosaico_energy_transfer_t transfer = {0};
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_init(&transfer, DEVICE_A, 1000));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_connect(
        &transfer, DEVICE_B, 0x100, MOSAICO_EDGE_RIGHT, MOSAICO_EDGE_LEFT));
    TEST_ASSERT_TRUE(transfer.connected);
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_IDLE, transfer.phase);
    TEST_ASSERT_EQUAL_UINT32(0, transfer.hop);
}

TEST_CASE("energy reset preserves the committed peer link", "[mosaico_game]")
{
    mosaico_energy_transfer_t transfer = {0};
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_init(&transfer, DEVICE_A, 1000));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_attach(
        &transfer, DEVICE_B, 0x100, MOSAICO_EDGE_RIGHT, MOSAICO_EDGE_LEFT, 50));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_reset(&transfer));
    TEST_ASSERT_TRUE(transfer.connected);
    TEST_ASSERT_EQUAL_UINT64(DEVICE_B, transfer.peer_id);
    TEST_ASSERT_EQUAL_UINT32(0x100, transfer.session_id);
    TEST_ASSERT_EQUAL(MOSAICO_EDGE_RIGHT, transfer.local_edge);
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_IDLE, transfer.phase);
    TEST_ASSERT_EQUAL_UINT32(0, transfer.hop);
    TEST_ASSERT_EQUAL_UINT16(0, transfer.progress_permille);
}

TEST_CASE("duplicate attach does not restart energy transfer", "[mosaico_game]")
{
    mosaico_energy_transfer_t transfer = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
        mosaico_energy_transfer_init(&transfer, DEVICE_A, 1000));
    TEST_ASSERT_EQUAL(ESP_OK,
        mosaico_energy_transfer_attach(&transfer, DEVICE_B, 0x100, 1, 3, 100));
    mosaico_energy_event_t event = {0};
    TEST_ASSERT_FALSE(mosaico_energy_transfer_update(&transfer, 600, &event));
    TEST_ASSERT_EQUAL_UINT16(500, transfer.progress_permille);

    TEST_ASSERT_EQUAL(ESP_OK,
        mosaico_energy_transfer_attach(&transfer, DEVICE_B, 0x100, 1, 3, 900));
    TEST_ASSERT_EQUAL_UINT32(100, transfer.started_ms);
    TEST_ASSERT_EQUAL_UINT16(500, transfer.progress_permille);
}

TEST_CASE("detach clears only the matching energy session", "[mosaico_game]")
{
    mosaico_energy_transfer_t transfer = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
        mosaico_energy_transfer_init(&transfer, DEVICE_A, 1000));
    TEST_ASSERT_EQUAL(ESP_OK,
        mosaico_energy_transfer_attach(&transfer, DEVICE_B, 0x100, 1, 3, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
        mosaico_energy_transfer_detach(&transfer, DEVICE_B, 0x101));
    TEST_ASSERT_TRUE(transfer.connected);
    TEST_ASSERT_EQUAL(ESP_OK,
        mosaico_energy_transfer_detach(&transfer, DEVICE_B, 0x100));
    TEST_ASSERT_FALSE(transfer.connected);
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_IDLE, transfer.phase);
}

TEST_CASE("energy transfer ignores an earlier scheduler timestamp", "[mosaico_game]")
{
    mosaico_energy_transfer_t transfer = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
        mosaico_energy_transfer_init(&transfer, DEVICE_A, 1000));
    TEST_ASSERT_EQUAL(ESP_OK,
        mosaico_energy_transfer_attach(&transfer, DEVICE_B, 0x100, 1, 3, 100));
    mosaico_energy_event_t event = {0};
    TEST_ASSERT_FALSE(mosaico_energy_transfer_update(&transfer, 99, &event));
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_SENDING, transfer.phase);
    TEST_ASSERT_EQUAL_UINT16(0, transfer.progress_permille);
}

TEST_CASE("energy handoff retries and duplicate packets are idempotent", "[mosaico_game]")
{
    mosaico_energy_transfer_t sender = {0};
    mosaico_energy_transfer_t receiver = {0};
    mosaico_energy_event_t event = {0};
    mosaico_energy_event_t response = {0};
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_init(&sender, DEVICE_A, 100));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_init(&receiver, DEVICE_B, 100));
    TEST_ASSERT_EQUAL(ESP_OK,
        mosaico_energy_transfer_attach(&sender, DEVICE_B, 0x100, 1, 3, 0));
    TEST_ASSERT_EQUAL(ESP_OK,
        mosaico_energy_transfer_attach(&receiver, DEVICE_A, 0x100, 3, 1, 0));
    TEST_ASSERT_TRUE(mosaico_energy_transfer_update(&sender, 100, &event));
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_EVENT_HANDOFF, event.kind);
    TEST_ASSERT_FALSE(mosaico_energy_transfer_update(&sender, 299, &event));
    TEST_ASSERT_TRUE(mosaico_energy_transfer_update(&sender, 300, &event));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_receive(
        &receiver, event.kind, event.hop, 300, &response));
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_RECEIVING, receiver.phase);
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_EVENT_ACCEPTED, response.kind);
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_receive(
        &sender, response.kind, response.hop, 300, &event));
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_SENT, sender.phase);
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_receive(
        &receiver, MOSAICO_ENERGY_EVENT_HANDOFF, 1, 301, &response));
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_EVENT_ACCEPTED, response.kind);
    TEST_ASSERT_TRUE(mosaico_energy_transfer_update(&receiver, 400, &event));
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_EVENT_COMPLETE, event.kind);
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_energy_transfer_receive(
        &sender, event.kind, event.hop, 400, &response));
    TEST_ASSERT_EQUAL(MOSAICO_ENERGY_SENT, sender.phase);
}
