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

#ifdef __cplusplus
extern "C" {
#endif

#define MOSAICO_IDIOM_MAX_TOKENS 4
#define MOSAICO_ENERGY_TRANSFER_DEFAULT_DURATION_MS 650U
#define MOSAICO_ENERGY_TRANSFER_RETRY_MS 200U

typedef struct {
    uint32_t tokens[MOSAICO_IDIOM_MAX_TOKENS];
    size_t count;
} mosaico_idiom_chain_t;

typedef enum {
    MOSAICO_ENERGY_IDLE = 0,
    MOSAICO_ENERGY_WAIT_HANDOFF,
    MOSAICO_ENERGY_SENDING,
    MOSAICO_ENERGY_WAIT_COMPLETE,
    MOSAICO_ENERGY_RECEIVING,
    MOSAICO_ENERGY_SENT,
    MOSAICO_ENERGY_RECEIVED,
} mosaico_energy_phase_t;

typedef enum {
    MOSAICO_ENERGY_EVENT_NONE = 0,
    MOSAICO_ENERGY_EVENT_HANDOFF,
    MOSAICO_ENERGY_EVENT_ACCEPTED,
    MOSAICO_ENERGY_EVENT_COMPLETE,
} mosaico_energy_event_kind_t;

typedef struct {
    mosaico_energy_event_kind_t kind;
    uint32_t hop;
} mosaico_energy_event_t;

typedef struct {
    uint64_t local_id;
    uint64_t peer_id;
    uint32_t session_id;
    uint32_t started_ms;
    uint32_t duration_ms;
    uint32_t last_tx_ms;
    uint32_t hop;
    uint16_t progress_permille;
    mosaico_edge_t local_edge;
    mosaico_edge_t peer_edge;
    mosaico_energy_phase_t phase;
    bool connected;
} mosaico_energy_transfer_t;

bool mosaico_pair_matches(uint32_t local_category, uint32_t peer_category);

void mosaico_idiom_chain_reset(mosaico_idiom_chain_t *chain);
esp_err_t mosaico_idiom_chain_append(mosaico_idiom_chain_t *chain, uint32_t token);
esp_err_t mosaico_idiom_chain_remove_last(mosaico_idiom_chain_t *chain);
bool mosaico_idiom_chain_matches(
    const mosaico_idiom_chain_t *chain,
    const uint32_t *expected,
    size_t expected_count);

esp_err_t mosaico_energy_transfer_init(
    mosaico_energy_transfer_t *transfer,
    uint64_t local_id,
    uint32_t duration_ms);

esp_err_t mosaico_energy_transfer_connect(
    mosaico_energy_transfer_t *transfer,
    uint64_t peer_id,
    uint32_t session_id,
    mosaico_edge_t local_edge,
    mosaico_edge_t peer_edge);

esp_err_t mosaico_energy_transfer_attach(
    mosaico_energy_transfer_t *transfer,
    uint64_t peer_id,
    uint32_t session_id,
    mosaico_edge_t local_edge,
    mosaico_edge_t peer_edge,
    uint32_t now_ms);

esp_err_t mosaico_energy_transfer_detach(
    mosaico_energy_transfer_t *transfer,
    uint64_t peer_id,
    uint32_t session_id);

esp_err_t mosaico_energy_transfer_reset(mosaico_energy_transfer_t *transfer);

esp_err_t mosaico_energy_transfer_begin(
    mosaico_energy_transfer_t *transfer,
    bool sender,
    uint32_t hop,
    uint32_t now_ms);

bool mosaico_energy_transfer_update(
    mosaico_energy_transfer_t *transfer,
    uint32_t now_ms,
    mosaico_energy_event_t *event);

esp_err_t mosaico_energy_transfer_receive(
    mosaico_energy_transfer_t *transfer,
    mosaico_energy_event_kind_t kind,
    uint32_t hop,
    uint32_t now_ms,
    mosaico_energy_event_t *response);

const char *mosaico_energy_phase_to_string(mosaico_energy_phase_t phase);

#ifdef __cplusplus
}
#endif
