/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>

#include "pong_protocol.h"
#include "unity.h"

static pong_message_t make_message(pong_message_kind_t kind)
{
    pong_message_t message = {
        .magic = PONG_PROTOCOL_MAGIC,
        .version = PONG_PROTOCOL_VERSION,
        .kind = (uint8_t)kind,
        .payload_len = (uint8_t)pong_protocol_payload_size(kind),
        .session = 0x10203040,
        .sequence = 77,
        .tick = 1234,
    };

    switch (kind) {
    case PONG_MSG_HELLO:
        message.payload.hello.device_id[0] = 0x24;
        message.payload.hello.device_id[5] = 0xaa;
        message.payload.hello.capabilities = 0x55aa;
        message.payload.hello.nonce = 0xabcdef01;
        message.payload.hello.preferred_role = PONG_ROLE_RIGHT;
        break;
    case PONG_MSG_PAIR:
        message.payload.pair.role = PONG_ROLE_LEFT;
        message.payload.pair.dock_state = PONG_DOCK_SEAMLESS;
        message.payload.pair.nonce = 9;
        break;
    case PONG_MSG_READY:
        message.payload.ready.role = PONG_ROLE_RIGHT;
        message.payload.ready.ready = 1;
        break;
    case PONG_MSG_INPUT:
        message.payload.input.role = PONG_ROLE_LEFT;
        message.payload.input.buttons = 3;
        message.payload.input.axis_x_q15 = -12345;
        message.payload.input.axis_y_q15 = 23456;
        message.payload.input.paddle_velocity = -321;
        message.payload.input.paddle_tilt_q12 = 2048;
        message.payload.input.sampled_ms = 98765;
        break;
    case PONG_MSG_SNAPSHOT:
        message.payload.snapshot.ball_x_q4 = 7680;
        message.payload.snapshot.ball_y_q4 = 3200;
        message.payload.snapshot.ball_vx_q2 = -1800;
        message.payload.snapshot.ball_vy_q2 = 700;
        message.payload.snapshot.ball_spin_q4 = -64;
        message.payload.snapshot.paddle_y_q4[0] = 1200;
        message.payload.snapshot.paddle_y_q4[1] = 6400;
        message.payload.snapshot.paddle_velocity_q2[0] = -40;
        message.payload.snapshot.paddle_velocity_q2[1] = 50;
        message.payload.snapshot.paddle_tilt_q12[0] = -1024;
        message.payload.snapshot.paddle_tilt_q12[1] = 2048;
        message.payload.snapshot.event_id = 42;
        message.payload.snapshot.countdown_ms = 900;
        message.payload.snapshot.score[0] = 2;
        message.payload.snapshot.score[1] = 3;
        message.payload.snapshot.serving_role = PONG_ROLE_RIGHT;
        message.payload.snapshot.phase = PONG_PHASE_PLAYING;
        message.payload.snapshot.event = PONG_EVENT_PADDLE_HIT;
        break;
    case PONG_MSG_CONTROL:
        message.payload.control.action = PONG_CONTROL_NEW_ROUND;
        message.payload.control.value = PONG_ROLE_LEFT;
        message.payload.control.argument = 99;
        break;
    case PONG_MSG_PING:
        message.payload.ping.timestamp_ms = 1000;
        message.payload.ping.echoed_timestamp_ms = 950;
        break;
    case PONG_MSG_LAYOUT:
        message.payload.layout.role = PONG_ROLE_RIGHT;
        message.payload.layout.dock_state = PONG_DOCK_REVERSED;
        message.payload.layout.rotation_quarters = 2;
        message.payload.layout.flags = 1;
        message.payload.layout.origin_x = 480;
        message.payload.layout.origin_y = -12;
        break;
    default:
        break;
    }
    return message;
}

TEST_CASE("pong protocol round trips every message kind", "[pong][protocol]")
{
    for (pong_message_kind_t kind = PONG_MSG_HELLO;
         kind <= PONG_MSG_LAYOUT; ++kind) {
        const pong_message_t source = make_message(kind);
        pong_message_t decoded;
        uint8_t first[PONG_PROTOCOL_MAX_WIRE_SIZE];
        uint8_t second[PONG_PROTOCOL_MAX_WIRE_SIZE];
        size_t first_len = 0;
        size_t second_len = 0;

        TEST_ASSERT_TRUE(pong_protocol_encode(&source, first, sizeof(first),
                                              &first_len));
        TEST_ASSERT_LESS_THAN_UINT32(100, first_len);
        TEST_ASSERT_TRUE(pong_protocol_decode(first, first_len, &decoded));
        TEST_ASSERT_TRUE(pong_protocol_encode(&decoded, second, sizeof(second),
                                              &second_len));
        TEST_ASSERT_EQUAL_UINT32(first_len, second_len);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(first, second, first_len);
    }
}

TEST_CASE("pong protocol rejects bad magic version and length",
          "[pong][protocol]")
{
    const pong_message_t source = make_message(PONG_MSG_INPUT);
    pong_message_t decoded;
    uint8_t wire[PONG_PROTOCOL_MAX_WIRE_SIZE];
    uint8_t damaged[PONG_PROTOCOL_MAX_WIRE_SIZE];
    size_t wire_len = 0;
    TEST_ASSERT_TRUE(pong_protocol_encode(&source, wire, sizeof(wire),
                                          &wire_len));

    memcpy(damaged, wire, wire_len);
    damaged[0] ^= 0xff;
    TEST_ASSERT_FALSE(pong_protocol_decode(damaged, wire_len, &decoded));

    memcpy(damaged, wire, wire_len);
    damaged[2]++;
    TEST_ASSERT_FALSE(pong_protocol_decode(damaged, wire_len, &decoded));

    memcpy(damaged, wire, wire_len);
    damaged[4]--;
    TEST_ASSERT_FALSE(pong_protocol_decode(damaged, wire_len, &decoded));

    TEST_ASSERT_FALSE(pong_protocol_decode(wire, wire_len - 1, &decoded));
    TEST_ASSERT_FALSE(pong_protocol_decode(wire, 4, &decoded));
}

TEST_CASE("pong protocol rejects duplicate and old sequence numbers",
          "[pong][protocol]")
{
    TEST_ASSERT_TRUE(pong_protocol_sequence_is_newer(101, 100));
    TEST_ASSERT_FALSE(pong_protocol_sequence_is_newer(100, 100));
    TEST_ASSERT_FALSE(pong_protocol_sequence_is_newer(99, 100));
    TEST_ASSERT_TRUE(pong_protocol_sequence_is_newer(1, UINT32_MAX));
    TEST_ASSERT_FALSE(pong_protocol_sequence_is_newer(UINT32_MAX, 1));
}
