/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PONG_WORLD_WIDTH             960.0f
#define PONG_WORLD_HEIGHT            480.0f
#define PONG_VIEWPORT_WIDTH          480.0f
#define PONG_PADDLE_WIDTH            16.0f
#define PONG_PADDLE_HEIGHT           104.0f
#define PONG_PADDLE_LEFT_X           32.0f
#define PONG_PADDLE_RIGHT_X          (PONG_WORLD_WIDTH - PONG_PADDLE_LEFT_X)
#define PONG_BALL_RADIUS             12.0f
#define PONG_SCORE_TO_WIN            7U
#define PONG_RECONNECT_GRACE_MS      5000U

typedef enum {
    PONG_ROLE_LEFT = 0,
    PONG_ROLE_RIGHT,
} pong_role_t;

typedef enum {
    PONG_PHASE_BOOT = 0,
    PONG_PHASE_CALIBRATING,
    PONG_PHASE_LOBBY,
    PONG_PHASE_COUNTDOWN,
    PONG_PHASE_PLAYING,
    PONG_PHASE_PAUSED,
    PONG_PHASE_ROUND_OVER,
    PONG_PHASE_MATCH_OVER,
} pong_phase_t;

typedef enum {
    PONG_DOCK_UNAVAILABLE = 0,
    PONG_DOCK_WIRELESS,
    PONG_DOCK_ATTACHING,
    PONG_DOCK_SEAMLESS,
    PONG_DOCK_REVERSED,
} pong_dock_state_t;

typedef enum {
    PONG_EVENT_NONE = 0,
    PONG_EVENT_PADDLE_HIT,
    PONG_EVENT_WALL_HIT,
    PONG_EVENT_SEAM_CROSS,
    PONG_EVENT_GOAL,
    PONG_EVENT_SERVE,
    PONG_EVENT_MATCH_WIN,
} pong_event_kind_t;

typedef struct {
    float x;
    float y;
} pong_vec2_t;

typedef struct {
    float y;
    float velocity;
    float tilt;
} pong_paddle_t;

typedef struct {
    pong_vec2_t position;
    pong_vec2_t velocity;
    float spin;
} pong_ball_t;

typedef struct {
    uint32_t tick;
    uint32_t event_id;
    pong_event_kind_t event;
    pong_phase_t phase;
    pong_ball_t ball;
    pong_paddle_t paddles[2];
    uint8_t score[2];
    uint8_t serving_role;
    uint16_t countdown_ms;
} pong_world_t;

typedef struct {
    float axis_x;
    float axis_y;
    uint16_t buttons;
    uint32_t sequence;
    uint32_t sampled_ms;
} pong_input_t;

typedef struct {
    pong_world_t world;
    pong_role_t local_role;
    pong_dock_state_t dock_state;
    bool joystick_ready;
    bool peer_present;
    bool local_ready;
    bool peer_ready;
    bool is_host;
    uint16_t latency_ms;
    int8_t rssi;
    uint8_t packet_loss_percent;
    char peer_label[20];
    char status[48];
} pong_render_snapshot_t;
