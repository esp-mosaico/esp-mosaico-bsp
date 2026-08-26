/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mosaico_peer_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOSAICO_PEER_SESSION_EDGE_COUNT 4

typedef enum {
    MOSAICO_PEER_SESSION_IDLE = 0,
    MOSAICO_PEER_SESSION_CLAIMING,
    MOSAICO_PEER_SESSION_WAIT_COMMIT,
    MOSAICO_PEER_SESSION_COMMITTED,
    MOSAICO_PEER_SESSION_RELEASING,
} mosaico_peer_session_state_t;

typedef enum {
    MOSAICO_PEER_SESSION_EVENT_SEND = 0,
    MOSAICO_PEER_SESSION_EVENT_ATTACHED,
    MOSAICO_PEER_SESSION_EVENT_DETACHED,
} mosaico_peer_session_event_type_t;

typedef enum {
    MOSAICO_PEER_DETACH_LOCAL_RELEASE = 0,
    MOSAICO_PEER_DETACH_REMOTE_RELEASE,
    MOSAICO_PEER_DETACH_STALE,
} mosaico_peer_detach_reason_t;

typedef struct {
    uint32_t retry_interval_ms;
    uint32_t handshake_timeout_ms;
    uint32_t heartbeat_interval_ms;
    uint32_t stale_timeout_ms;
    uint8_t release_retries;
} mosaico_peer_session_config_t;

#define MOSAICO_PEER_SESSION_CONFIG_DEFAULT() { \
    .retry_interval_ms = 200,                   \
    .handshake_timeout_ms = 2000,               \
    .heartbeat_interval_ms = 500,               \
    .stale_timeout_ms = 2500,                   \
    .release_retries = 3,                       \
}

typedef struct {
    mosaico_peer_session_event_type_t type;
    mosaico_peer_message_type_t message_type;
    mosaico_peer_detach_reason_t detach_reason;
    uint64_t target_id;
    uint64_t peer_id;
    uint32_t session_id;
    mosaico_edge_t local_edge;
    mosaico_edge_t peer_edge;
    uint16_t relative_rotation;
} mosaico_peer_session_event_t;

typedef void (*mosaico_peer_session_event_cb_t)(
    const mosaico_peer_session_event_t *event,
    void *user_ctx);

typedef struct {
    mosaico_peer_session_state_t state;
    bool local_present;
    bool attached_notified;
    uint8_t release_tx_count;
    uint64_t peer_id;
    uint32_t session_id;
    uint32_t started_ms;
    uint32_t last_tx_ms;
    uint32_t last_rx_ms;
    mosaico_edge_t local_edge;
    mosaico_edge_t peer_edge;
    uint16_t relative_rotation;
} mosaico_peer_session_slot_t;

typedef struct {
    uint64_t device_id;
    uint32_t session_counter;
    mosaico_peer_session_config_t config;
    mosaico_peer_session_event_cb_t event_cb;
    void *user_ctx;
    mosaico_peer_session_slot_t slots[MOSAICO_PEER_SESSION_EDGE_COUNT];
} mosaico_peer_session_manager_t;

esp_err_t mosaico_peer_session_init(
    mosaico_peer_session_manager_t *manager,
    uint64_t device_id,
    const mosaico_peer_session_config_t *config,
    mosaico_peer_session_event_cb_t event_cb,
    void *user_ctx);

void mosaico_peer_session_reset(mosaico_peer_session_manager_t *manager);

esp_err_t mosaico_peer_session_local_contact(
    mosaico_peer_session_manager_t *manager,
    mosaico_edge_t local_edge,
    uint16_t relative_rotation,
    uint32_t now_ms);

esp_err_t mosaico_peer_session_local_release(
    mosaico_peer_session_manager_t *manager,
    mosaico_edge_t local_edge,
    uint32_t now_ms);

esp_err_t mosaico_peer_session_receive(
    mosaico_peer_session_manager_t *manager,
    const mosaico_peer_message_t *message,
    uint32_t now_ms);

void mosaico_peer_session_tick(
    mosaico_peer_session_manager_t *manager,
    uint32_t now_ms);

const mosaico_peer_session_slot_t *mosaico_peer_session_get(
    const mosaico_peer_session_manager_t *manager,
    mosaico_edge_t local_edge);

const char *mosaico_peer_session_state_to_string(mosaico_peer_session_state_t state);

#ifdef __cplusplus
}
#endif
