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
#include "freertos/FreeRTOS.h"
#include "mosaico_interaction.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOSAICO_PEER_PAYLOAD_SIZE 100
#define MOSAICO_PEER_DEFAULT_NETWORK_ID 0x4D4F5341U

typedef enum {
    MOSAICO_PEER_MSG_HELLO = 1,
    MOSAICO_PEER_MSG_CONTACT_CLAIM,
    MOSAICO_PEER_MSG_CONTACT_ACK,
    MOSAICO_PEER_MSG_CONTACT_COMMIT,
    MOSAICO_PEER_MSG_CONTACT_RELEASE,
    MOSAICO_PEER_MSG_TOPOLOGY_SYNC,
    MOSAICO_PEER_MSG_GAME_EVENT,
    MOSAICO_PEER_MSG_APP_DATA,
    MOSAICO_PEER_MSG_DIAGNOSTIC,
} mosaico_peer_message_type_t;

typedef struct {
    mosaico_peer_message_type_t type;
    uint64_t source_id;
    uint32_t source_boot_id;
    uint64_t target_id;
    uint32_t session_id;
    uint32_t sequence;
    mosaico_edge_t local_edge;
    mosaico_edge_t peer_edge;
    uint16_t relative_rotation;
    uint8_t payload_len;
    uint8_t payload[MOSAICO_PEER_PAYLOAD_SIZE];
} mosaico_peer_message_t;

typedef struct {
    uint32_t network_id;
    bool manage_wifi;
    uint8_t channel;
    uint8_t receive_queue_depth;
    uint16_t worker_stack_size;
    UBaseType_t worker_priority;
} mosaico_peer_link_config_t;

#define MOSAICO_PEER_LINK_CONFIG_DEFAULT() { \
    .network_id = MOSAICO_PEER_DEFAULT_NETWORK_ID, \
    .manage_wifi = true,                       \
    .channel = 1,                             \
    .receive_queue_depth = 32,                \
    .worker_stack_size = 4096,                \
    .worker_priority = 5,                     \
}

typedef void (*mosaico_peer_receive_cb_t)(
    const uint8_t source_mac[6],
    int8_t rssi,
    const mosaico_peer_message_t *message,
    void *user_ctx);

esp_err_t mosaico_peer_link_init(
    const mosaico_peer_link_config_t *config,
    mosaico_peer_receive_cb_t receive_cb,
    void *user_ctx);

esp_err_t mosaico_peer_link_deinit(void);

uint64_t mosaico_peer_link_get_device_id(void);
uint32_t mosaico_peer_link_get_boot_id(void);

esp_err_t mosaico_peer_link_send_broadcast(
    mosaico_peer_message_type_t type,
    uint32_t session_id,
    mosaico_edge_t local_edge,
    mosaico_edge_t peer_edge,
    uint16_t relative_rotation,
    const void *payload,
    size_t payload_len);

esp_err_t mosaico_peer_link_send_broadcast_to(
    mosaico_peer_message_type_t type,
    uint64_t target_id,
    uint32_t session_id,
    mosaico_edge_t local_edge,
    mosaico_edge_t peer_edge,
    uint16_t relative_rotation,
    const void *payload,
    size_t payload_len);

#ifdef __cplusplus
}
#endif
