/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mosaico_interaction.h"
#include "mosaico_mesh.h"
#include "mosaico_peer_link.h"
#include "mosaico_peer_session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOSAICO_NETWORK_APP_PAYLOAD_SIZE 64
#define MOSAICO_NETWORK_DEFAULT_TASK_STACK_SIZE 5120

typedef struct mosaico_network *mosaico_network_handle_t;

typedef enum {
    MOSAICO_NETWORK_EVENT_NEIGHBOR_ATTACHED = 0,
    MOSAICO_NETWORK_EVENT_NEIGHBOR_DETACHED,
    MOSAICO_NETWORK_EVENT_TOPOLOGY_CHANGED,
    MOSAICO_NETWORK_EVENT_TOPOLOGY_CONFLICT,
    MOSAICO_NETWORK_EVENT_MESSAGE_RECEIVED,
    MOSAICO_NETWORK_EVENT_MESSAGE_DELIVERED,
    MOSAICO_NETWORK_EVENT_MESSAGE_FAILED,
} mosaico_network_event_type_t;

typedef struct {
    uint64_t device_id;
    int16_t x;
    int16_t y;
    uint16_t rotation_degrees;
} mosaico_network_node_t;

typedef struct {
    uint64_t device_id;
    uint64_t root_id;
    uint32_t topology_id;
    uint8_t node_count;
    uint8_t conflict_flags;
} mosaico_network_snapshot_t;

typedef struct {
    uint64_t peer_id;
    uint32_t session_id;
    mosaico_edge_t local_edge;
    mosaico_edge_t peer_edge;
    uint16_t relative_rotation;
} mosaico_network_neighbor_event_t;

typedef struct {
    uint64_t source_id;
    uint64_t destination_id;
    uint16_t service_id;
    uint8_t payload_len;
    uint8_t payload[MOSAICO_NETWORK_APP_PAYLOAD_SIZE];
} mosaico_network_message_t;

typedef struct {
    uint64_t destination_id;
    uint32_t message_id;
    uint16_t service_id;
    esp_err_t error;
} mosaico_network_delivery_event_t;

typedef struct {
    mosaico_network_event_type_t type;
    mosaico_network_snapshot_t snapshot;
    union {
        mosaico_network_neighbor_event_t neighbor;
        mosaico_network_message_t message;
        mosaico_network_delivery_event_t delivery;
    } data;
} mosaico_network_event_t;

typedef void (*mosaico_network_event_cb_t)(
    const mosaico_network_event_t *event,
    void *user_ctx);

typedef void (*mosaico_network_raw_message_cb_t)(
    const mosaico_peer_message_t *message,
    int8_t rssi,
    void *user_ctx);

typedef struct {
    uint32_t network_id;
    bool manage_wifi;
    uint8_t channel;
    uint8_t receive_queue_depth;
    uint16_t worker_stack_size;
    uint16_t task_stack_size;
    UBaseType_t worker_priority;
    UBaseType_t task_priority;
    uint32_t topology_sync_interval_ms;
    uint32_t app_retry_interval_ms;
    uint8_t app_retry_count;
    mosaico_peer_session_config_t session;
    mosaico_network_event_cb_t event_cb;
    mosaico_network_raw_message_cb_t raw_message_cb;
    void *user_ctx;
} mosaico_network_config_t;

#define MOSAICO_NETWORK_CONFIG_DEFAULT() { \
    .network_id = MOSAICO_PEER_DEFAULT_NETWORK_ID, \
    .manage_wifi = true, \
    .channel = 1, \
    .receive_queue_depth = 32, \
    .worker_stack_size = 4096, \
    .task_stack_size = MOSAICO_NETWORK_DEFAULT_TASK_STACK_SIZE, \
    .worker_priority = 5, \
    .task_priority = 5, \
    .topology_sync_interval_ms = 1000, \
    .app_retry_interval_ms = 250, \
    .app_retry_count = 5, \
    .session = MOSAICO_PEER_SESSION_CONFIG_DEFAULT(), \
    .event_cb = NULL, \
    .raw_message_cb = NULL, \
    .user_ctx = NULL, \
}

/** Start one network instance and take ownership according to @c manage_wifi. */
esp_err_t mosaico_network_start(
    const mosaico_network_config_t *config,
    mosaico_network_handle_t *out_handle);

/** Stop the instance. Do not call this function from an event callback. */
esp_err_t mosaico_network_stop(mosaico_network_handle_t handle);

/** Report one debounced physical edge contact or release. */
esp_err_t mosaico_network_set_contact(
    mosaico_network_handle_t handle,
    mosaico_edge_t local_edge,
    bool present,
    uint32_t timestamp_ms);

/** Queue a reliable routed unicast. Completion is reported as an event. */
esp_err_t mosaico_network_send(
    mosaico_network_handle_t handle,
    uint64_t destination_id,
    uint16_t service_id,
    const void *payload,
    size_t payload_len);

/** Send a best-effort deduplicated message to the connected component. */
esp_err_t mosaico_network_broadcast(
    mosaico_network_handle_t handle,
    uint16_t service_id,
    const void *payload,
    size_t payload_len);

/** Copy the current local view of topology identity and health. */
esp_err_t mosaico_network_get_snapshot(
    mosaico_network_handle_t handle,
    mosaico_network_snapshot_t *snapshot);

/** Copy connected node poses. Returns the number of copied entries. */
size_t mosaico_network_get_nodes(
    mosaico_network_handle_t handle,
    mosaico_network_node_t *nodes,
    size_t capacity);

#ifdef __cplusplus
}
#endif
