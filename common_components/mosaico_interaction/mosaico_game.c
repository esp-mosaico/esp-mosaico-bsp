/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_game.h"

#include <string.h>

bool mosaico_pair_matches(uint32_t local_category, uint32_t peer_category)
{
    return local_category != 0 && local_category == peer_category;
}

void mosaico_idiom_chain_reset(mosaico_idiom_chain_t *chain)
{
    if (chain) {
        memset(chain, 0, sizeof(*chain));
    }
}

esp_err_t mosaico_idiom_chain_append(mosaico_idiom_chain_t *chain, uint32_t token)
{
    if (!chain || token == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (chain->count >= MOSAICO_IDIOM_MAX_TOKENS) {
        return ESP_ERR_NO_MEM;
    }
    chain->tokens[chain->count++] = token;
    return ESP_OK;
}

esp_err_t mosaico_idiom_chain_remove_last(mosaico_idiom_chain_t *chain)
{
    if (!chain) {
        return ESP_ERR_INVALID_ARG;
    }
    if (chain->count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    chain->tokens[--chain->count] = 0;
    return ESP_OK;
}

bool mosaico_idiom_chain_matches(
    const mosaico_idiom_chain_t *chain,
    const uint32_t *expected,
    size_t expected_count)
{
    return chain && expected && chain->count == expected_count &&
           expected_count <= MOSAICO_IDIOM_MAX_TOKENS &&
           memcmp(chain->tokens, expected, expected_count * sizeof(expected[0])) == 0;
}

esp_err_t mosaico_energy_transfer_init(
    mosaico_energy_transfer_t *transfer,
    uint64_t local_id,
    uint32_t duration_ms)
{
    if (!transfer || local_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(transfer, 0, sizeof(*transfer));
    transfer->local_id = local_id;
    transfer->duration_ms = duration_ms ?
        duration_ms : MOSAICO_ENERGY_TRANSFER_DEFAULT_DURATION_MS;
    return ESP_OK;
}

esp_err_t mosaico_energy_transfer_connect(
    mosaico_energy_transfer_t *transfer,
    uint64_t peer_id,
    uint32_t session_id,
    mosaico_edge_t local_edge,
    mosaico_edge_t peer_edge)
{
    if (!transfer || transfer->local_id == 0 || peer_id == 0 ||
        peer_id == transfer->local_id || session_id == 0 ||
        local_edge < MOSAICO_EDGE_TOP || local_edge > MOSAICO_EDGE_LEFT ||
        peer_edge < MOSAICO_EDGE_TOP || peer_edge > MOSAICO_EDGE_LEFT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (transfer->connected) {
        return transfer->peer_id == peer_id &&
               transfer->session_id == session_id &&
               transfer->local_edge == local_edge &&
               transfer->peer_edge == peer_edge ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    transfer->peer_id = peer_id;
    transfer->session_id = session_id;
    transfer->progress_permille = 0;
    transfer->local_edge = local_edge;
    transfer->peer_edge = peer_edge;
    transfer->phase = MOSAICO_ENERGY_IDLE;
    transfer->connected = true;
    return ESP_OK;
}

esp_err_t mosaico_energy_transfer_attach(
    mosaico_energy_transfer_t *transfer,
    uint64_t peer_id,
    uint32_t session_id,
    mosaico_edge_t local_edge,
    mosaico_edge_t peer_edge,
    uint32_t now_ms)
{
    const bool already_connected = transfer && transfer->connected;
    esp_err_t ret = mosaico_energy_transfer_connect(
        transfer, peer_id, session_id, local_edge, peer_edge);
    if (ret != ESP_OK || already_connected) {
        return ret;
    }
    const uint64_t lower_id = transfer->local_id < peer_id ?
        transfer->local_id : peer_id;
    const uint64_t higher_id = transfer->local_id < peer_id ?
        peer_id : transfer->local_id;
    const uint64_t sender_id = (session_id & 1U) ? higher_id : lower_id;
    return mosaico_energy_transfer_begin(
        transfer, transfer->local_id == sender_id, 1, now_ms);
}

esp_err_t mosaico_energy_transfer_detach(
    mosaico_energy_transfer_t *transfer,
    uint64_t peer_id,
    uint32_t session_id)
{
    if (!transfer || peer_id == 0 || session_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!transfer->connected || transfer->peer_id != peer_id ||
        transfer->session_id != session_id) {
        return ESP_ERR_NOT_FOUND;
    }

    const uint64_t local_id = transfer->local_id;
    const uint32_t duration_ms = transfer->duration_ms;
    memset(transfer, 0, sizeof(*transfer));
    transfer->local_id = local_id;
    transfer->duration_ms = duration_ms;
    return ESP_OK;
}

esp_err_t mosaico_energy_transfer_reset(mosaico_energy_transfer_t *transfer)
{
    if (!transfer || !transfer->connected) {
        return ESP_ERR_INVALID_STATE;
    }
    transfer->started_ms = 0;
    transfer->last_tx_ms = 0;
    transfer->hop = 0;
    transfer->progress_permille = 0;
    transfer->phase = MOSAICO_ENERGY_IDLE;
    return ESP_OK;
}

esp_err_t mosaico_energy_transfer_begin(
    mosaico_energy_transfer_t *transfer,
    bool sender,
    uint32_t hop,
    uint32_t now_ms)
{
    if (!transfer || !transfer->connected || hop == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    transfer->started_ms = now_ms;
    transfer->last_tx_ms = 0;
    transfer->hop = hop;
    transfer->progress_permille = 0;
    transfer->phase = sender ? MOSAICO_ENERGY_SENDING :
        MOSAICO_ENERGY_WAIT_HANDOFF;
    return ESP_OK;
}

bool mosaico_energy_transfer_update(
    mosaico_energy_transfer_t *transfer,
    uint32_t now_ms,
    mosaico_energy_event_t *event)
{
    if (event) {
        memset(event, 0, sizeof(*event));
    }
    if (!transfer || !event || !transfer->connected) {
        return false;
    }

    if (transfer->phase == MOSAICO_ENERGY_WAIT_COMPLETE) {
        if ((uint32_t)(now_ms - transfer->last_tx_ms) >=
            MOSAICO_ENERGY_TRANSFER_RETRY_MS) {
            transfer->last_tx_ms = now_ms;
            event->kind = MOSAICO_ENERGY_EVENT_HANDOFF;
            event->hop = transfer->hop;
            return true;
        }
        return false;
    }
    if (transfer->phase != MOSAICO_ENERGY_SENDING &&
        transfer->phase != MOSAICO_ENERGY_RECEIVING) {
        return false;
    }

    if (now_ms < transfer->started_ms &&
        transfer->started_ms - now_ms < (UINT32_MAX / 2U)) {
        return false;
    }
    const uint32_t elapsed_ms = now_ms - transfer->started_ms;
    if (elapsed_ms < transfer->duration_ms) {
        transfer->progress_permille =
            (uint16_t)(((uint64_t)elapsed_ms * 1000U) / transfer->duration_ms);
        return false;
    }

    transfer->progress_permille = 1000;
    if (transfer->phase == MOSAICO_ENERGY_SENDING) {
        transfer->phase = MOSAICO_ENERGY_WAIT_COMPLETE;
        transfer->last_tx_ms = now_ms;
        event->kind = MOSAICO_ENERGY_EVENT_HANDOFF;
    } else {
        transfer->phase = MOSAICO_ENERGY_RECEIVED;
        event->kind = MOSAICO_ENERGY_EVENT_COMPLETE;
    }
    event->hop = transfer->hop;
    return true;
}

esp_err_t mosaico_energy_transfer_receive(
    mosaico_energy_transfer_t *transfer,
    mosaico_energy_event_kind_t kind,
    uint32_t hop,
    uint32_t now_ms,
    mosaico_energy_event_t *response)
{
    if (!transfer || !response || !transfer->connected || hop == 0 ||
        (kind != MOSAICO_ENERGY_EVENT_HANDOFF &&
         kind != MOSAICO_ENERGY_EVENT_ACCEPTED &&
         kind != MOSAICO_ENERGY_EVENT_COMPLETE)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(response, 0, sizeof(*response));
    if (hop != transfer->hop) {
        return ESP_ERR_NOT_FOUND;
    }

    if (kind == MOSAICO_ENERGY_EVENT_ACCEPTED ||
        kind == MOSAICO_ENERGY_EVENT_COMPLETE) {
        if (transfer->phase == MOSAICO_ENERGY_WAIT_COMPLETE ||
            transfer->phase == MOSAICO_ENERGY_SENT) {
            transfer->phase = MOSAICO_ENERGY_SENT;
            return ESP_OK;
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (transfer->phase == MOSAICO_ENERGY_WAIT_HANDOFF) {
        transfer->phase = MOSAICO_ENERGY_RECEIVING;
        transfer->started_ms = now_ms;
        transfer->progress_permille = 0;
        response->kind = MOSAICO_ENERGY_EVENT_ACCEPTED;
        response->hop = transfer->hop;
        return ESP_OK;
    }
    if (transfer->phase == MOSAICO_ENERGY_RECEIVED) {
        response->kind = MOSAICO_ENERGY_EVENT_COMPLETE;
        response->hop = transfer->hop;
        return ESP_OK;
    }
    if (transfer->phase == MOSAICO_ENERGY_RECEIVING) {
        response->kind = MOSAICO_ENERGY_EVENT_ACCEPTED;
        response->hop = transfer->hop;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

const char *mosaico_energy_phase_to_string(mosaico_energy_phase_t phase)
{
    switch (phase) {
    case MOSAICO_ENERGY_IDLE:      return "IDLE";
    case MOSAICO_ENERGY_WAIT_HANDOFF: return "WAIT_HANDOFF";
    case MOSAICO_ENERGY_SENDING:   return "SENDING";
    case MOSAICO_ENERGY_WAIT_COMPLETE: return "WAIT_COMPLETE";
    case MOSAICO_ENERGY_RECEIVING: return "RECEIVING";
    case MOSAICO_ENERGY_SENT:      return "SENT";
    case MOSAICO_ENERGY_RECEIVED:  return "RECEIVED";
    default:                       return "UNKNOWN";
    }
}
