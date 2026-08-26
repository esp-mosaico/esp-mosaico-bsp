/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mosaico_peer_link.h"
#include "pong_protocol.h"
#include "pong_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PONG_NETWORK_INPUT_PERIOD_MS     20U
#define PONG_NETWORK_SNAPSHOT_PERIOD_MS  33U
#define PONG_NETWORK_PAUSE_TIMEOUT_MS    800U
#define PONG_NETWORK_GRACE_TIMEOUT_MS    PONG_RECONNECT_GRACE_MS

typedef enum {
    PONG_NETWORK_EVENT_NONE = 0,
    PONG_NETWORK_EVENT_PAIRED,
    PONG_NETWORK_EVENT_READY_CHANGED,
    PONG_NETWORK_EVENT_INPUT,
    PONG_NETWORK_EVENT_SNAPSHOT,
    PONG_NETWORK_EVENT_CONTROL,
    PONG_NETWORK_EVENT_LAYOUT,
    PONG_NETWORK_EVENT_PEER_PAUSED,
    PONG_NETWORK_EVENT_PEER_RESUMED,
    PONG_NETWORK_EVENT_PEER_LOST,
    PONG_NETWORK_EVENT_PEER_REBOOT,
} pong_network_event_kind_t;

typedef struct {
    pong_network_event_kind_t kind;
    uint64_t peer_id;
    uint32_t session_id;
    union {
        pong_input_t input;
        pong_world_t snapshot;
        pong_control_payload_t control;
        pong_dock_state_t dock_state;
        bool ready;
    } data;
} pong_network_event_t;

typedef struct {
    bool started;
    bool paired;
    bool paused;
    bool local_ready;
    bool peer_ready;
    bool is_host;
    pong_role_t local_role;
    uint64_t device_id;
    uint32_t boot_id;
    uint64_t peer_id;
    uint32_t peer_boot_id;
    uint32_t session_id;
    uint16_t rtt_ms;
    int8_t rssi;
    uint8_t packet_loss_percent;
    uint32_t last_receive_ms;
} pong_network_status_t;

/**
 * Start the singleton peer link and Pong discovery state machine.
 */
esp_err_t pong_network_start(void);

/**
 * Stop networking and release all resources owned by this module.
 */
esp_err_t pong_network_stop(void);

/**
 * Drain received frames and advance discovery, ping, and timeout handling.
 */
void pong_network_tick(uint32_t now_ms);

esp_err_t pong_network_set_ready(bool ready);
esp_err_t pong_network_send_input(const pong_input_t *input);
esp_err_t pong_network_send_snapshot(const pong_world_t *world);
esp_err_t pong_network_send_control(pong_control_action_t action,
                                    uint32_t argument);
esp_err_t pong_network_send_layout(pong_dock_state_t state);

bool pong_network_poll_event(pong_network_event_t *event);
esp_err_t pong_network_get_status(pong_network_status_t *status);

/**
 * Serialized peer-link adapter used by the docking session SEND callback.
 *
 * Only contact-session message types are accepted. The adapter is safe to call
 * from pong_docking_tick() and pong_docking_receive_peer_message().
 */
esp_err_t pong_network_send_peer_message(
    mosaico_peer_message_type_t type,
    uint64_t target_id,
    uint32_t session_id,
    mosaico_edge_t local_edge,
    mosaico_edge_t peer_edge,
    uint16_t relative_rotation);

#ifdef __cplusplus
}
#endif
