/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pong_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PONG_PROTOCOL_MAGIC          0x5047U
#define PONG_PROTOCOL_VERSION        1U
#define PONG_PROTOCOL_HEADER_SIZE    18U
#define PONG_PROTOCOL_MAX_WIRE_SIZE  64U

typedef enum {
    PONG_MSG_HELLO = 1,
    PONG_MSG_PAIR,
    PONG_MSG_READY,
    PONG_MSG_INPUT,
    PONG_MSG_SNAPSHOT,
    PONG_MSG_CONTROL,
    PONG_MSG_PING,
    PONG_MSG_LAYOUT,
} pong_message_kind_t;

typedef enum {
    PONG_CONTROL_PAUSE = 1,
    PONG_CONTROL_RESUME,
    PONG_CONTROL_NEW_ROUND,
    PONG_CONTROL_RESET_MATCH,
    PONG_CONTROL_EMOTE,
} pong_control_action_t;

typedef struct __attribute__((packed)) {
    uint8_t device_id[6];
    uint16_t capabilities;
    uint32_t nonce;
    uint8_t preferred_role;
} pong_hello_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t role;
    uint8_t dock_state;
    uint32_t nonce;
} pong_pair_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t role;
    uint8_t ready;
} pong_ready_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t role;
    uint16_t buttons;
    int16_t axis_x_q15;
    int16_t axis_y_q15;
    int16_t paddle_velocity;
    int16_t paddle_tilt_q12;
    uint32_t sampled_ms;
} pong_input_payload_t;

typedef struct __attribute__((packed)) {
    int16_t ball_x_q4;
    int16_t ball_y_q4;
    int16_t ball_vx_q2;
    int16_t ball_vy_q2;
    int16_t ball_spin_q4;
    int16_t paddle_y_q4[2];
    int16_t paddle_velocity_q2[2];
    int16_t paddle_tilt_q12[2];
    uint32_t event_id;
    uint16_t countdown_ms;
    uint8_t score[2];
    uint8_t serving_role;
    uint8_t phase;
    uint8_t event;
} pong_snapshot_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t action;
    uint8_t value;
    uint32_t argument;
} pong_control_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t timestamp_ms;
    uint32_t echoed_timestamp_ms;
} pong_ping_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t role;
    uint8_t dock_state;
    uint8_t rotation_quarters;
    uint8_t flags;
    int16_t origin_x;
    int16_t origin_y;
} pong_layout_payload_t;

typedef union __attribute__((packed)) {
    pong_hello_payload_t hello;
    pong_pair_payload_t pair;
    pong_ready_payload_t ready;
    pong_input_payload_t input;
    pong_snapshot_payload_t snapshot;
    pong_control_payload_t control;
    pong_ping_payload_t ping;
    pong_layout_payload_t layout;
} pong_message_payload_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t kind;
    uint8_t payload_len;
    uint8_t flags;
    uint32_t session;
    uint32_t sequence;
    uint32_t tick;
    pong_message_payload_t payload;
} pong_message_t;

#ifdef __cplusplus
static_assert(PONG_PROTOCOL_MAX_WIRE_SIZE < 100U,
              "Pong protocol must fit the peer payload");
static_assert(PONG_PROTOCOL_HEADER_SIZE + sizeof(pong_message_payload_t) <=
                  PONG_PROTOCOL_MAX_WIRE_SIZE,
              "Pong wire message exceeds its declared maximum");
#else
_Static_assert(PONG_PROTOCOL_MAX_WIRE_SIZE < 100U,
               "Pong protocol must fit the peer payload");
_Static_assert(PONG_PROTOCOL_HEADER_SIZE + sizeof(pong_message_payload_t) <=
                   PONG_PROTOCOL_MAX_WIRE_SIZE,
               "Pong wire message exceeds its declared maximum");
#endif

/**
 * Encode one validated message into a canonical little-endian wire frame.
 */
bool pong_protocol_encode(const pong_message_t *message, uint8_t *wire,
                          size_t wire_capacity, size_t *wire_len);

/**
 * Decode one complete untrusted wire frame after validating all lengths.
 */
bool pong_protocol_decode(const uint8_t *wire, size_t wire_len,
                          pong_message_t *message);

/**
 * Return the exact payload length for a kind, or zero for an invalid kind.
 */
size_t pong_protocol_payload_size(pong_message_kind_t kind);

/** Return true when candidate is newer, including uint32 wraparound. */
bool pong_protocol_sequence_is_newer(uint32_t candidate, uint32_t previous);

#ifdef __cplusplus
}
#endif
