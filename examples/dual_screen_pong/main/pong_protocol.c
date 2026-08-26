/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include "pong_protocol.h"

#include <string.h>

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static uint16_t get_u16(const uint8_t *in)
{
    return (uint16_t)in[0] | ((uint16_t)in[1] << 8);
}

static uint32_t get_u32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static void put_i16(uint8_t *out, int16_t value)
{
    put_u16(out, (uint16_t)value);
}

static int16_t get_i16(const uint8_t *in)
{
    return (int16_t)get_u16(in);
}

bool pong_protocol_sequence_is_newer(uint32_t candidate, uint32_t previous)
{
    return (int32_t)(candidate - previous) > 0;
}

size_t pong_protocol_payload_size(pong_message_kind_t kind)
{
    switch (kind) {
    case PONG_MSG_HELLO:
        return 13;
    case PONG_MSG_PAIR:
        return 6;
    case PONG_MSG_READY:
        return 2;
    case PONG_MSG_INPUT:
        return 15;
    case PONG_MSG_SNAPSHOT:
        return 33;
    case PONG_MSG_CONTROL:
        return 6;
    case PONG_MSG_PING:
        return 8;
    case PONG_MSG_LAYOUT:
        return 8;
    default:
        return 0;
    }
}

static bool encode_payload(const pong_message_t *message, uint8_t *out)
{
    switch ((pong_message_kind_t)message->kind) {
    case PONG_MSG_HELLO:
        for (size_t i = 0; i < sizeof(message->payload.hello.device_id); ++i) {
            out[i] = message->payload.hello.device_id[i];
        }
        put_u16(out + 6, message->payload.hello.capabilities);
        put_u32(out + 8, message->payload.hello.nonce);
        out[12] = message->payload.hello.preferred_role;
        return message->payload.hello.preferred_role <= PONG_ROLE_RIGHT;
    case PONG_MSG_PAIR:
        out[0] = message->payload.pair.role;
        out[1] = message->payload.pair.dock_state;
        put_u32(out + 2, message->payload.pair.nonce);
        return out[0] <= PONG_ROLE_RIGHT && out[1] <= PONG_DOCK_REVERSED;
    case PONG_MSG_READY:
        out[0] = message->payload.ready.role;
        out[1] = message->payload.ready.ready;
        return out[0] <= PONG_ROLE_RIGHT && out[1] <= 1U;
    case PONG_MSG_INPUT:
        out[0] = message->payload.input.role;
        put_u16(out + 1, message->payload.input.buttons);
        put_i16(out + 3, message->payload.input.axis_x_q15);
        put_i16(out + 5, message->payload.input.axis_y_q15);
        put_i16(out + 7, message->payload.input.paddle_velocity);
        put_i16(out + 9, message->payload.input.paddle_tilt_q12);
        put_u32(out + 11, message->payload.input.sampled_ms);
        return out[0] <= PONG_ROLE_RIGHT;
    case PONG_MSG_SNAPSHOT: {
        const pong_snapshot_payload_t p = message->payload.snapshot;
        put_i16(out, p.ball_x_q4);
        put_i16(out + 2, p.ball_y_q4);
        put_i16(out + 4, p.ball_vx_q2);
        put_i16(out + 6, p.ball_vy_q2);
        put_i16(out + 8, p.ball_spin_q4);
        for (size_t i = 0; i < 2; ++i) {
            put_i16(out + 10 + i * 2, p.paddle_y_q4[i]);
            put_i16(out + 14 + i * 2, p.paddle_velocity_q2[i]);
            put_i16(out + 18 + i * 2, p.paddle_tilt_q12[i]);
        }
        put_u32(out + 22, p.event_id);
        put_u16(out + 26, p.countdown_ms);
        out[28] = p.score[0];
        out[29] = p.score[1];
        out[30] = p.serving_role;
        out[31] = p.phase;
        out[32] = p.event;
        return p.serving_role <= PONG_ROLE_RIGHT &&
               p.phase <= PONG_PHASE_MATCH_OVER &&
               p.event <= PONG_EVENT_MATCH_WIN;
    }
    case PONG_MSG_CONTROL:
        out[0] = message->payload.control.action;
        out[1] = message->payload.control.value;
        put_u32(out + 2, message->payload.control.argument);
        return out[0] >= PONG_CONTROL_PAUSE &&
               out[0] <= PONG_CONTROL_EMOTE;
    case PONG_MSG_PING:
        put_u32(out, message->payload.ping.timestamp_ms);
        put_u32(out + 4, message->payload.ping.echoed_timestamp_ms);
        return true;
    case PONG_MSG_LAYOUT:
        out[0] = message->payload.layout.role;
        out[1] = message->payload.layout.dock_state;
        out[2] = message->payload.layout.rotation_quarters;
        out[3] = message->payload.layout.flags;
        put_i16(out + 4, message->payload.layout.origin_x);
        put_i16(out + 6, message->payload.layout.origin_y);
        return out[0] <= PONG_ROLE_RIGHT &&
               out[1] <= PONG_DOCK_REVERSED && out[2] <= 3U;
    default:
        return false;
    }
}

bool pong_protocol_encode(const pong_message_t *message, uint8_t *wire,
                          size_t wire_capacity, size_t *wire_len)
{
    if (message == NULL || wire == NULL || wire_len == NULL ||
        message->magic != PONG_PROTOCOL_MAGIC ||
        message->version != PONG_PROTOCOL_VERSION) {
        return false;
    }
    const size_t payload_len =
        pong_protocol_payload_size((pong_message_kind_t)message->kind);
    const size_t total_len = PONG_PROTOCOL_HEADER_SIZE + payload_len;
    if (payload_len == 0U || message->payload_len != payload_len ||
        total_len > wire_capacity || total_len > PONG_PROTOCOL_MAX_WIRE_SIZE) {
        return false;
    }

    put_u16(wire, message->magic);
    wire[2] = message->version;
    wire[3] = message->kind;
    wire[4] = message->payload_len;
    wire[5] = message->flags;
    put_u32(wire + 6, message->session);
    put_u32(wire + 10, message->sequence);
    put_u32(wire + 14, message->tick);
    if (!encode_payload(message, wire + PONG_PROTOCOL_HEADER_SIZE)) {
        return false;
    }
    *wire_len = total_len;
    return true;
}

static bool decode_payload(const uint8_t *in, pong_message_t *message)
{
    switch ((pong_message_kind_t)message->kind) {
    case PONG_MSG_HELLO:
        for (size_t i = 0; i < sizeof(message->payload.hello.device_id); ++i) {
            message->payload.hello.device_id[i] = in[i];
        }
        message->payload.hello.capabilities = get_u16(in + 6);
        message->payload.hello.nonce = get_u32(in + 8);
        message->payload.hello.preferred_role = in[12];
        return in[12] <= PONG_ROLE_RIGHT;
    case PONG_MSG_PAIR:
        message->payload.pair.role = in[0];
        message->payload.pair.dock_state = in[1];
        message->payload.pair.nonce = get_u32(in + 2);
        return in[0] <= PONG_ROLE_RIGHT && in[1] <= PONG_DOCK_REVERSED;
    case PONG_MSG_READY:
        message->payload.ready.role = in[0];
        message->payload.ready.ready = in[1];
        return in[0] <= PONG_ROLE_RIGHT && in[1] <= 1U;
    case PONG_MSG_INPUT:
        message->payload.input.role = in[0];
        message->payload.input.buttons = get_u16(in + 1);
        message->payload.input.axis_x_q15 = get_i16(in + 3);
        message->payload.input.axis_y_q15 = get_i16(in + 5);
        message->payload.input.paddle_velocity = get_i16(in + 7);
        message->payload.input.paddle_tilt_q12 = get_i16(in + 9);
        message->payload.input.sampled_ms = get_u32(in + 11);
        return in[0] <= PONG_ROLE_RIGHT;
    case PONG_MSG_SNAPSHOT: {
        pong_snapshot_payload_t p = {0};
        p.ball_x_q4 = get_i16(in);
        p.ball_y_q4 = get_i16(in + 2);
        p.ball_vx_q2 = get_i16(in + 4);
        p.ball_vy_q2 = get_i16(in + 6);
        p.ball_spin_q4 = get_i16(in + 8);
        for (size_t i = 0; i < 2; ++i) {
            p.paddle_y_q4[i] = get_i16(in + 10 + i * 2);
            p.paddle_velocity_q2[i] = get_i16(in + 14 + i * 2);
            p.paddle_tilt_q12[i] = get_i16(in + 18 + i * 2);
        }
        p.event_id = get_u32(in + 22);
        p.countdown_ms = get_u16(in + 26);
        p.score[0] = in[28];
        p.score[1] = in[29];
        p.serving_role = in[30];
        p.phase = in[31];
        p.event = in[32];
        message->payload.snapshot = p;
        return p.serving_role <= PONG_ROLE_RIGHT &&
               p.phase <= PONG_PHASE_MATCH_OVER &&
               p.event <= PONG_EVENT_MATCH_WIN;
    }
    case PONG_MSG_CONTROL:
        message->payload.control.action = in[0];
        message->payload.control.value = in[1];
        message->payload.control.argument = get_u32(in + 2);
        return in[0] >= PONG_CONTROL_PAUSE &&
               in[0] <= PONG_CONTROL_EMOTE;
    case PONG_MSG_PING:
        message->payload.ping.timestamp_ms = get_u32(in);
        message->payload.ping.echoed_timestamp_ms = get_u32(in + 4);
        return true;
    case PONG_MSG_LAYOUT:
        message->payload.layout.role = in[0];
        message->payload.layout.dock_state = in[1];
        message->payload.layout.rotation_quarters = in[2];
        message->payload.layout.flags = in[3];
        message->payload.layout.origin_x = get_i16(in + 4);
        message->payload.layout.origin_y = get_i16(in + 6);
        return in[0] <= PONG_ROLE_RIGHT &&
               in[1] <= PONG_DOCK_REVERSED && in[2] <= 3U;
    default:
        return false;
    }
}

bool pong_protocol_decode(const uint8_t *wire, size_t wire_len,
                          pong_message_t *message)
{
    if (wire == NULL || message == NULL ||
        wire_len < PONG_PROTOCOL_HEADER_SIZE ||
        wire_len > PONG_PROTOCOL_MAX_WIRE_SIZE ||
        get_u16(wire) != PONG_PROTOCOL_MAGIC ||
        wire[2] != PONG_PROTOCOL_VERSION) {
        return false;
    }
    const pong_message_kind_t kind = (pong_message_kind_t)wire[3];
    const size_t expected_payload = pong_protocol_payload_size(kind);
    if (expected_payload == 0U || wire[4] != expected_payload ||
        wire_len != PONG_PROTOCOL_HEADER_SIZE + expected_payload) {
        return false;
    }

    pong_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    decoded.magic = get_u16(wire);
    decoded.version = wire[2];
    decoded.kind = wire[3];
    decoded.payload_len = wire[4];
    decoded.flags = wire[5];
    decoded.session = get_u32(wire + 6);
    decoded.sequence = get_u32(wire + 10);
    decoded.tick = get_u32(wire + 14);
    if (!decode_payload(wire + PONG_PROTOCOL_HEADER_SIZE, &decoded)) {
        return false;
    }
    *message = decoded;
    return true;
}
