/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "mosaico_peer_session.h"
#include "mosaico_topology.h"
#include "unity.h"

#define DEVICE_A_ID 0x020000000001ULL
#define DEVICE_B_ID 0x020000000002ULL
#define MAX_RECORDED_EVENTS 32

typedef struct {
    mosaico_peer_session_event_t events[MAX_RECORDED_EVENTS];
    size_t count;
} event_recorder_t;

static mosaico_edge_t opposite_edge(mosaico_edge_t edge)
{
    return (mosaico_edge_t)(
        ((edge - MOSAICO_EDGE_TOP + 2) % 4) + MOSAICO_EDGE_TOP);
}

static void record_event(
    const mosaico_peer_session_event_t *event,
    void *user_ctx)
{
    event_recorder_t *recorder = user_ctx;
    TEST_ASSERT_LESS_THAN(MAX_RECORDED_EVENTS, recorder->count);
    recorder->events[recorder->count++] = *event;
}

static const mosaico_peer_session_event_t *latest_send(
    const event_recorder_t *recorder,
    mosaico_peer_message_type_t type)
{
    for (size_t i = recorder->count; i > 0; --i) {
        const mosaico_peer_session_event_t *event = &recorder->events[i - 1];
        if (event->type == MOSAICO_PEER_SESSION_EVENT_SEND &&
            event->message_type == type) {
            return event;
        }
    }
    return NULL;
}

static mosaico_peer_message_t message_from_event(
    uint64_t source_id,
    const mosaico_peer_session_event_t *event)
{
    TEST_ASSERT_NOT_NULL(event);
    return (mosaico_peer_message_t) {
        .type = event->message_type,
        .source_id = source_id,
        .target_id = event->target_id,
        .session_id = event->session_id,
        .local_edge = event->local_edge,
        .peer_edge = event->peer_edge,
        .relative_rotation = event->relative_rotation,
    };
}

static void deliver_latest(
    mosaico_peer_session_manager_t *destination,
    uint64_t source_id,
    const event_recorder_t *source_recorder,
    mosaico_peer_message_type_t type,
    uint32_t now_ms)
{
    const mosaico_peer_message_t message = message_from_event(
        source_id, latest_send(source_recorder, type));
    TEST_ASSERT_EQUAL(ESP_OK,
                      mosaico_peer_session_receive(destination, &message, now_ms));
}

static void initialize_pair_edges(
    mosaico_peer_session_manager_t *manager_a,
    event_recorder_t *recorder_a,
    mosaico_peer_session_manager_t *manager_b,
    event_recorder_t *recorder_b,
    mosaico_edge_t edge_a,
    mosaico_edge_t edge_b)
{
    memset(recorder_a, 0, sizeof(*recorder_a));
    memset(recorder_b, 0, sizeof(*recorder_b));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_peer_session_init(
        manager_a, DEVICE_A_ID, NULL, record_event, recorder_a));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_peer_session_init(
        manager_b, DEVICE_B_ID, NULL, record_event, recorder_b));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_peer_session_local_contact(
        manager_a, edge_a, 0, 10));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_peer_session_local_contact(
        manager_b, edge_b, 0, 10));
}

static void initialize_pair(
    mosaico_peer_session_manager_t *manager_a,
    event_recorder_t *recorder_a,
    mosaico_peer_session_manager_t *manager_b,
    event_recorder_t *recorder_b)
{
    initialize_pair_edges(manager_a, recorder_a, manager_b, recorder_b,
                          MOSAICO_EDGE_TOP, MOSAICO_EDGE_BOTTOM);
}

static void complete_handshake(
    mosaico_peer_session_manager_t *manager_a,
    event_recorder_t *recorder_a,
    mosaico_peer_session_manager_t *manager_b,
    event_recorder_t *recorder_b)
{
    deliver_latest(manager_a, DEVICE_B_ID, recorder_b,
                   MOSAICO_PEER_MSG_CONTACT_CLAIM, 20);
    deliver_latest(manager_b, DEVICE_A_ID, recorder_a,
                   MOSAICO_PEER_MSG_CONTACT_CLAIM, 21);
    deliver_latest(manager_a, DEVICE_B_ID, recorder_b,
                   MOSAICO_PEER_MSG_CONTACT_ACK, 22);
    deliver_latest(manager_b, DEVICE_A_ID, recorder_a,
                   MOSAICO_PEER_MSG_CONTACT_COMMIT, 23);
    deliver_latest(manager_a, DEVICE_B_ID, recorder_b,
                   MOSAICO_PEER_MSG_CONTACT_COMMIT, 24);
}

TEST_CASE("peer sessions commit every physical edge pairing", "[mosaico_peer]")
{
    static const mosaico_edge_t edges[] = {
        MOSAICO_EDGE_TOP,
        MOSAICO_EDGE_RIGHT,
        MOSAICO_EDGE_BOTTOM,
        MOSAICO_EDGE_LEFT,
    };
    for (size_t a = 0; a < sizeof(edges) / sizeof(edges[0]); ++a) {
        for (size_t b = 0; b < sizeof(edges) / sizeof(edges[0]); ++b) {
            if (!mosaico_edges_can_connect(edges[a], edges[b])) {
                continue;
            }
            mosaico_peer_session_manager_t manager_a;
            mosaico_peer_session_manager_t manager_b;
            event_recorder_t recorder_a;
            event_recorder_t recorder_b;
            initialize_pair_edges(&manager_a, &recorder_a, &manager_b, &recorder_b,
                                  edges[a], edges[b]);
            complete_handshake(&manager_a, &recorder_a, &manager_b, &recorder_b);

            const mosaico_peer_session_slot_t *slot_a =
                mosaico_peer_session_get(&manager_a, edges[a]);
            const mosaico_peer_session_slot_t *slot_b =
                mosaico_peer_session_get(&manager_b, edges[b]);
            TEST_ASSERT_EQUAL(MOSAICO_PEER_SESSION_COMMITTED, slot_a->state);
            TEST_ASSERT_EQUAL(MOSAICO_PEER_SESSION_COMMITTED, slot_b->state);
            TEST_ASSERT_EQUAL_HEX64(DEVICE_B_ID, slot_a->peer_id);
            TEST_ASSERT_EQUAL_HEX64(DEVICE_A_ID, slot_b->peer_id);
            TEST_ASSERT_EQUAL(edges[b], slot_a->peer_edge);
            TEST_ASSERT_EQUAL(edges[a], slot_b->peer_edge);
            TEST_ASSERT_EQUAL_HEX32(slot_a->session_id, slot_b->session_id);
            uint16_t expected_a = 0;
            uint16_t expected_b = 0;
            TEST_ASSERT_EQUAL(ESP_OK, mosaico_topology_resolve_display_rotation(
                edges[a], edges[b], &expected_a));
            TEST_ASSERT_EQUAL(ESP_OK, mosaico_topology_resolve_display_rotation(
                edges[b], edges[a], &expected_b));
            TEST_ASSERT_EQUAL_UINT16(expected_a, slot_a->relative_rotation);
            TEST_ASSERT_EQUAL_UINT16(expected_b, slot_b->relative_rotation);
        }
    }
}

TEST_CASE("peer sessions ignore cross-axis contact claims", "[mosaico_peer]")
{
    mosaico_peer_session_manager_t manager_a;
    mosaico_peer_session_manager_t manager_b;
    event_recorder_t recorder_a;
    event_recorder_t recorder_b;
    initialize_pair_edges(
        &manager_a, &recorder_a, &manager_b, &recorder_b,
        MOSAICO_EDGE_TOP, MOSAICO_EDGE_RIGHT);

    deliver_latest(&manager_a, DEVICE_B_ID, &recorder_b,
                   MOSAICO_PEER_MSG_CONTACT_CLAIM, 20);
    deliver_latest(&manager_b, DEVICE_A_ID, &recorder_a,
                   MOSAICO_PEER_MSG_CONTACT_CLAIM, 21);
    TEST_ASSERT_NULL(latest_send(&recorder_a, MOSAICO_PEER_MSG_CONTACT_ACK));
    TEST_ASSERT_NULL(latest_send(&recorder_b, MOSAICO_PEER_MSG_CONTACT_ACK));
    TEST_ASSERT_EQUAL(
        MOSAICO_PEER_SESSION_CLAIMING,
        mosaico_peer_session_get(&manager_a, MOSAICO_EDGE_TOP)->state);
    TEST_ASSERT_EQUAL(
        MOSAICO_PEER_SESSION_CLAIMING,
        mosaico_peer_session_get(&manager_b, MOSAICO_EDGE_RIGHT)->state);
}

TEST_CASE("ambiguous local edges do not guess a peer mapping", "[mosaico_peer]")
{
    mosaico_peer_session_manager_t manager;
    event_recorder_t recorder = {0};
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_peer_session_init(
        &manager, DEVICE_A_ID, NULL, record_event, &recorder));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_peer_session_local_contact(
        &manager, MOSAICO_EDGE_RIGHT, 0, 10));
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_peer_session_local_contact(
        &manager, MOSAICO_EDGE_LEFT, 0, 11));
    const size_t event_count = recorder.count;
    const mosaico_peer_message_t claim = {
        .type = MOSAICO_PEER_MSG_CONTACT_CLAIM,
        .source_id = DEVICE_B_ID,
        .session_id = 0x1234,
        .local_edge = MOSAICO_EDGE_LEFT,
        .peer_edge = MOSAICO_EDGE_NONE,
    };

    TEST_ASSERT_EQUAL(ESP_OK,
                      mosaico_peer_session_receive(&manager, &claim, 20));
    TEST_ASSERT_EQUAL_UINT32(event_count, recorder.count);
    TEST_ASSERT_EQUAL(MOSAICO_PEER_SESSION_CLAIMING,
                      mosaico_peer_session_get(&manager, MOSAICO_EDGE_RIGHT)->state);
    TEST_ASSERT_EQUAL(MOSAICO_PEER_SESSION_CLAIMING,
                      mosaico_peer_session_get(&manager, MOSAICO_EDGE_LEFT)->state);
}

TEST_CASE("committed edges do not make one new edge ambiguous", "[mosaico_peer]")
{
    static const mosaico_edge_t committed_edges[] = {
        MOSAICO_EDGE_TOP,
        MOSAICO_EDGE_RIGHT,
        MOSAICO_EDGE_LEFT,
    };
    for (size_t committed_count = 1;
         committed_count <= sizeof(committed_edges) / sizeof(committed_edges[0]);
         ++committed_count) {
        mosaico_peer_session_manager_t manager;
        event_recorder_t recorder = {0};
        TEST_ASSERT_EQUAL(ESP_OK, mosaico_peer_session_init(
            &manager, DEVICE_B_ID, NULL, record_event, &recorder));
        for (mosaico_edge_t edge = MOSAICO_EDGE_TOP;
             edge <= MOSAICO_EDGE_LEFT; ++edge) {
            TEST_ASSERT_EQUAL(ESP_OK, mosaico_peer_session_local_contact(
                &manager, edge, 0, 10 + edge));
        }
        for (size_t i = 0; i < committed_count; ++i) {
            mosaico_peer_session_slot_t *slot =
                &manager.slots[committed_edges[i] - MOSAICO_EDGE_TOP];
            slot->state = MOSAICO_PEER_SESSION_COMMITTED;
            slot->peer_id = DEVICE_A_ID;
            slot->session_id = 0x1000 + i;
            slot->peer_edge = opposite_edge(committed_edges[i]);
            slot->attached_notified = true;
        }
        const mosaico_peer_message_t claim = {
            .type = MOSAICO_PEER_MSG_CONTACT_CLAIM,
            .source_id = DEVICE_A_ID,
            .session_id = 0x2345,
            .local_edge = MOSAICO_EDGE_TOP,
            .peer_edge = MOSAICO_EDGE_NONE,
        };

        TEST_ASSERT_EQUAL(ESP_OK,
                          mosaico_peer_session_receive(&manager, &claim, 30));
        const mosaico_peer_session_slot_t *new_slot =
            mosaico_peer_session_get(&manager, MOSAICO_EDGE_BOTTOM);
        TEST_ASSERT_EQUAL(MOSAICO_PEER_SESSION_WAIT_COMMIT, new_slot->state);
        TEST_ASSERT_EQUAL_HEX64(DEVICE_A_ID, new_slot->peer_id);
        TEST_ASSERT_EQUAL_HEX32(claim.session_id, new_slot->session_id);
        const mosaico_peer_session_event_t *ack =
            latest_send(&recorder, MOSAICO_PEER_MSG_CONTACT_ACK);
        TEST_ASSERT_NOT_NULL(ack);
        TEST_ASSERT_EQUAL(MOSAICO_EDGE_BOTTOM, ack->local_edge);
    }
}

TEST_CASE("directed session frames require both negotiated edges", "[mosaico_peer]")
{
    mosaico_peer_session_manager_t manager;
    event_recorder_t recorder = {0};
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_peer_session_init(
        &manager, DEVICE_A_ID, NULL, record_event, &recorder));
    const mosaico_peer_message_t ack = {
        .type = MOSAICO_PEER_MSG_CONTACT_ACK,
        .source_id = DEVICE_B_ID,
        .target_id = DEVICE_A_ID,
        .session_id = 0x1234,
        .local_edge = MOSAICO_EDGE_TOP,
        .peer_edge = MOSAICO_EDGE_NONE,
    };

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      mosaico_peer_session_receive(&manager, &ack, 20));
}

TEST_CASE("directed frames reject a changed remote edge", "[mosaico_peer]")
{
    mosaico_peer_session_manager_t manager_a;
    mosaico_peer_session_manager_t manager_b;
    event_recorder_t recorder_a;
    event_recorder_t recorder_b;
    initialize_pair(&manager_a, &recorder_a, &manager_b, &recorder_b);
    complete_handshake(&manager_a, &recorder_a, &manager_b, &recorder_b);

    const mosaico_peer_session_slot_t *slot =
        mosaico_peer_session_get(&manager_a, MOSAICO_EDGE_TOP);
    const uint32_t last_rx_ms = slot->last_rx_ms;
    const size_t event_count = recorder_a.count;
    mosaico_peer_message_t message = message_from_event(
        DEVICE_B_ID, latest_send(&recorder_b, MOSAICO_PEER_MSG_CONTACT_COMMIT));
    message.local_edge = MOSAICO_EDGE_RIGHT;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      mosaico_peer_session_receive(&manager_a, &message, 100));
    TEST_ASSERT_EQUAL(MOSAICO_PEER_SESSION_COMMITTED, slot->state);
    TEST_ASSERT_EQUAL_UINT32(last_rx_ms, slot->last_rx_ms);
    TEST_ASSERT_EQUAL_UINT32(event_count, recorder_a.count);

    message.type = MOSAICO_PEER_MSG_CONTACT_RELEASE;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      mosaico_peer_session_receive(&manager_a, &message, 101));
    TEST_ASSERT_EQUAL(MOSAICO_PEER_SESSION_COMMITTED, slot->state);
    TEST_ASSERT_EQUAL_UINT32(last_rx_ms, slot->last_rx_ms);
    TEST_ASSERT_EQUAL_UINT32(event_count, recorder_a.count);
}

TEST_CASE("duplicate claim cannot change a negotiated remote edge", "[mosaico_peer]")
{
    mosaico_peer_session_manager_t manager_a;
    mosaico_peer_session_manager_t manager_b;
    event_recorder_t recorder_a;
    event_recorder_t recorder_b;
    initialize_pair(&manager_a, &recorder_a, &manager_b, &recorder_b);
    complete_handshake(&manager_a, &recorder_a, &manager_b, &recorder_b);

    const mosaico_peer_session_slot_t *slot =
        mosaico_peer_session_get(&manager_a, MOSAICO_EDGE_TOP);
    const uint32_t last_rx_ms = slot->last_rx_ms;
    const size_t event_count = recorder_a.count;
    mosaico_peer_message_t claim = message_from_event(
        DEVICE_B_ID, latest_send(&recorder_b, MOSAICO_PEER_MSG_CONTACT_CLAIM));
    claim.session_id = slot->session_id;
    claim.local_edge = MOSAICO_EDGE_RIGHT;

    TEST_ASSERT_EQUAL(ESP_OK,
                      mosaico_peer_session_receive(&manager_a, &claim, 100));
    TEST_ASSERT_EQUAL(MOSAICO_EDGE_BOTTOM, slot->peer_edge);
    TEST_ASSERT_EQUAL_UINT32(last_rx_ms, slot->last_rx_ms);
    TEST_ASSERT_EQUAL_UINT32(event_count, recorder_a.count);
}

TEST_CASE("lost commit recovers through acknowledgement retry", "[mosaico_peer]")
{
    mosaico_peer_session_manager_t manager_a;
    mosaico_peer_session_manager_t manager_b;
    event_recorder_t recorder_a;
    event_recorder_t recorder_b;
    initialize_pair(&manager_a, &recorder_a, &manager_b, &recorder_b);

    deliver_latest(&manager_b, DEVICE_A_ID, &recorder_a,
                   MOSAICO_PEER_MSG_CONTACT_CLAIM, 20);
    deliver_latest(&manager_a, DEVICE_B_ID, &recorder_b,
                   MOSAICO_PEER_MSG_CONTACT_ACK, 21);
    TEST_ASSERT_EQUAL(
        MOSAICO_PEER_SESSION_WAIT_COMMIT,
        mosaico_peer_session_get(&manager_b, MOSAICO_EDGE_BOTTOM)->state);

    mosaico_peer_session_tick(&manager_b, 220);
    deliver_latest(&manager_a, DEVICE_B_ID, &recorder_b,
                   MOSAICO_PEER_MSG_CONTACT_ACK, 221);
    deliver_latest(&manager_b, DEVICE_A_ID, &recorder_a,
                   MOSAICO_PEER_MSG_CONTACT_COMMIT, 222);
    TEST_ASSERT_EQUAL(
        MOSAICO_PEER_SESSION_COMMITTED,
        mosaico_peer_session_get(&manager_b, MOSAICO_EDGE_BOTTOM)->state);
}

TEST_CASE("release detaches both peers and clears sessions", "[mosaico_peer]")
{
    mosaico_peer_session_manager_t manager_a;
    mosaico_peer_session_manager_t manager_b;
    event_recorder_t recorder_a;
    event_recorder_t recorder_b;
    initialize_pair(&manager_a, &recorder_a, &manager_b, &recorder_b);
    complete_handshake(&manager_a, &recorder_a, &manager_b, &recorder_b);

    TEST_ASSERT_EQUAL(ESP_OK, mosaico_peer_session_local_release(
        &manager_a, MOSAICO_EDGE_TOP, 1000));
    deliver_latest(&manager_b, DEVICE_A_ID, &recorder_a,
                   MOSAICO_PEER_MSG_CONTACT_RELEASE, 1001);
    TEST_ASSERT_EQUAL(ESP_OK, mosaico_peer_session_local_release(
        &manager_b, MOSAICO_EDGE_BOTTOM, 1002));

    mosaico_peer_session_tick(&manager_a, 1200);
    mosaico_peer_session_tick(&manager_b, 1202);
    mosaico_peer_session_tick(&manager_a, 1400);
    mosaico_peer_session_tick(&manager_b, 1402);
    mosaico_peer_session_tick(&manager_a, 1600);
    mosaico_peer_session_tick(&manager_b, 1602);
    TEST_ASSERT_EQUAL(
        MOSAICO_PEER_SESSION_IDLE,
        mosaico_peer_session_get(&manager_a, MOSAICO_EDGE_TOP)->state);
    TEST_ASSERT_EQUAL(
        MOSAICO_PEER_SESSION_IDLE,
        mosaico_peer_session_get(&manager_b, MOSAICO_EDGE_BOTTOM)->state);
}

TEST_CASE("stale committed peer detaches and renegotiates", "[mosaico_peer]")
{
    mosaico_peer_session_manager_t manager_a;
    mosaico_peer_session_manager_t manager_b;
    event_recorder_t recorder_a;
    event_recorder_t recorder_b;
    initialize_pair(&manager_a, &recorder_a, &manager_b, &recorder_b);
    complete_handshake(&manager_a, &recorder_a, &manager_b, &recorder_b);

    mosaico_peer_session_tick(&manager_a, 2524);
    const mosaico_peer_session_slot_t *slot =
        mosaico_peer_session_get(&manager_a, MOSAICO_EDGE_TOP);
    TEST_ASSERT_EQUAL(MOSAICO_PEER_SESSION_CLAIMING, slot->state);
    TEST_ASSERT_NOT_NULL(latest_send(
        &recorder_a, MOSAICO_PEER_MSG_CONTACT_CLAIM));
}
