/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_peer_session.h"
#include "mosaico_topology.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "mosaico_session";

static int edge_index(mosaico_edge_t edge)
{
    switch (edge) {
    case MOSAICO_EDGE_TOP:    return 0;
    case MOSAICO_EDGE_RIGHT:  return 1;
    case MOSAICO_EDGE_BOTTOM: return 2;
    case MOSAICO_EDGE_LEFT:   return 3;
    default:                  return -1;
    }
}

static uint32_t elapsed_ms(uint32_t now_ms, uint32_t then_ms)
{
    return now_ms - then_ms;
}

static uint32_t next_session_id(
    mosaico_peer_session_manager_t *manager,
    uint32_t now_ms)
{
    manager->session_counter++;
    uint32_t session_id = now_ms ^
        (uint32_t)manager->device_id ^
        (uint32_t)(manager->device_id >> 32) ^
        (manager->session_counter * 0x9e3779b9U);
    if (session_id == 0) {
        session_id = manager->session_counter;
        if (session_id == 0) {
            session_id = 1;
        }
    }
    return session_id;
}

static void emit_event(
    mosaico_peer_session_manager_t *manager,
    const mosaico_peer_session_slot_t *slot,
    mosaico_peer_session_event_type_t type,
    mosaico_peer_message_type_t message_type,
    uint64_t target_id,
    mosaico_peer_detach_reason_t detach_reason)
{
    const mosaico_peer_session_event_t event = {
        .type = type,
        .message_type = message_type,
        .detach_reason = detach_reason,
        .target_id = target_id,
        .peer_id = slot->peer_id,
        .session_id = slot->session_id,
        .local_edge = slot->local_edge,
        .peer_edge = slot->peer_edge,
        .relative_rotation = slot->relative_rotation,
    };
    manager->event_cb(&event, manager->user_ctx);
}

static void send_message(
    mosaico_peer_session_manager_t *manager,
    mosaico_peer_session_slot_t *slot,
    mosaico_peer_message_type_t type,
    uint64_t target_id,
    uint32_t now_ms)
{
    slot->last_tx_ms = now_ms;
    emit_event(manager, slot, MOSAICO_PEER_SESSION_EVENT_SEND,
               type, target_id, MOSAICO_PEER_DETACH_LOCAL_RELEASE);
}

static void notify_attached(
    mosaico_peer_session_manager_t *manager,
    mosaico_peer_session_slot_t *slot)
{
    if (slot->attached_notified) {
        return;
    }
    slot->attached_notified = true;
    ESP_LOGI(TAG, "committed: session=%08lx local=%s peer=%012llx/%s",
             (unsigned long)slot->session_id,
             mosaico_edge_to_string(slot->local_edge),
             (unsigned long long)slot->peer_id,
             mosaico_edge_to_string(slot->peer_edge));
    emit_event(manager, slot, MOSAICO_PEER_SESSION_EVENT_ATTACHED,
               MOSAICO_PEER_MSG_CONTACT_COMMIT, slot->peer_id,
               MOSAICO_PEER_DETACH_LOCAL_RELEASE);
}

static void notify_detached(
    mosaico_peer_session_manager_t *manager,
    mosaico_peer_session_slot_t *slot,
    mosaico_peer_detach_reason_t reason)
{
    if (!slot->attached_notified) {
        return;
    }
    slot->attached_notified = false;
    ESP_LOGI(TAG, "detached: session=%08lx local=%s peer=%012llx reason=%d",
             (unsigned long)slot->session_id,
             mosaico_edge_to_string(slot->local_edge),
             (unsigned long long)slot->peer_id, reason);
    emit_event(manager, slot, MOSAICO_PEER_SESSION_EVENT_DETACHED,
               MOSAICO_PEER_MSG_CONTACT_RELEASE, slot->peer_id, reason);
}

static void clear_slot(mosaico_peer_session_slot_t *slot)
{
    memset(slot, 0, sizeof(*slot));
}

static void begin_claim(
    mosaico_peer_session_manager_t *manager,
    mosaico_peer_session_slot_t *slot,
    mosaico_edge_t local_edge,
    uint16_t relative_rotation,
    uint32_t now_ms)
{
    const bool local_present = slot->local_present;
    clear_slot(slot);
    slot->state = MOSAICO_PEER_SESSION_CLAIMING;
    slot->local_present = local_present;
    slot->session_id = next_session_id(manager, now_ms);
    slot->started_ms = now_ms;
    slot->local_edge = local_edge;
    /* A broadcast claim cannot know which edge the receiving device detected. */
    slot->peer_edge = MOSAICO_EDGE_NONE;
    slot->relative_rotation = relative_rotation;
    send_message(manager, slot, MOSAICO_PEER_MSG_CONTACT_CLAIM, 0, now_ms);
}

static bool message_edges_are_valid(const mosaico_peer_message_t *message)
{
    if (edge_index(message->local_edge) < 0) {
        return false;
    }
    if (message->type == MOSAICO_PEER_MSG_CONTACT_CLAIM) {
        return message->peer_edge == MOSAICO_EDGE_NONE;
    }
    uint16_t expected_rotation = 0;
    return mosaico_topology_resolve_display_rotation(
               message->local_edge, message->peer_edge,
               &expected_rotation) == ESP_OK &&
           message->relative_rotation == expected_rotation;
}

static bool message_matches_slot(
    const mosaico_peer_session_slot_t *slot,
    const mosaico_peer_message_t *message)
{
    if (slot->local_edge != message->peer_edge) {
        return false;
    }
    return edge_index(slot->peer_edge) < 0 ||
           slot->peer_edge == message->local_edge;
}

static mosaico_peer_session_slot_t *select_claim_slot(
    mosaico_peer_session_manager_t *manager,
    const mosaico_peer_message_t *message)
{
    mosaico_peer_session_slot_t *candidate = NULL;
    size_t candidate_count = 0;

    for (int i = 0; i < MOSAICO_PEER_SESSION_EDGE_COUNT; ++i) {
        mosaico_peer_session_slot_t *slot = &manager->slots[i];
        if (!slot->local_present ||
            slot->state == MOSAICO_PEER_SESSION_RELEASING ||
            !mosaico_edges_can_connect(slot->local_edge, message->local_edge)) {
            continue;
        }
        if (slot->peer_id == message->source_id &&
            slot->session_id == message->session_id) {
            if (edge_index(slot->peer_edge) >= 0 &&
                slot->peer_edge != message->local_edge) {
                ESP_LOGW(TAG, "claim edge changed: peer=%012llx "
                         "expected=%s received=%s",
                         (unsigned long long)message->source_id,
                         mosaico_edge_to_string(slot->peer_edge),
                         mosaico_edge_to_string(message->local_edge));
                return NULL;
            }
            return slot;
        }
        if (slot->state == MOSAICO_PEER_SESSION_COMMITTED) {
            continue;
        }
        if (slot->peer_id != 0 && slot->peer_id != message->source_id) {
            continue;
        }
        candidate = slot;
        candidate_count++;
    }

    if (candidate_count > 1) {
        ESP_LOGW(TAG, "ambiguous contact claim: peer=%012llx remote=%s candidates=%u",
                 (unsigned long long)message->source_id,
                 mosaico_edge_to_string(message->local_edge),
                 (unsigned)candidate_count);
        return NULL;
    }
    return candidate;
}

esp_err_t mosaico_peer_session_init(
    mosaico_peer_session_manager_t *manager,
    uint64_t device_id,
    const mosaico_peer_session_config_t *config,
    mosaico_peer_session_event_cb_t event_cb,
    void *user_ctx)
{
    ESP_RETURN_ON_FALSE(manager && device_id && event_cb, ESP_ERR_INVALID_ARG, TAG,
                        "invalid session manager arguments");
    const mosaico_peer_session_config_t active = config ? *config :
        (mosaico_peer_session_config_t)MOSAICO_PEER_SESSION_CONFIG_DEFAULT();
    ESP_RETURN_ON_FALSE(active.retry_interval_ms > 0 &&
                        active.handshake_timeout_ms > active.retry_interval_ms &&
                        active.heartbeat_interval_ms > 0 &&
                        active.stale_timeout_ms > active.heartbeat_interval_ms &&
                        active.release_retries > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid session timing configuration");
    memset(manager, 0, sizeof(*manager));
    manager->device_id = device_id;
    manager->config = active;
    manager->event_cb = event_cb;
    manager->user_ctx = user_ctx;
    ESP_LOGI(TAG, "initialized: device=%012llx retry=%lums stale=%lums",
             (unsigned long long)device_id,
             (unsigned long)active.retry_interval_ms,
             (unsigned long)active.stale_timeout_ms);
    return ESP_OK;
}

void mosaico_peer_session_reset(mosaico_peer_session_manager_t *manager)
{
    if (!manager) {
        return;
    }
    const uint64_t device_id = manager->device_id;
    const mosaico_peer_session_config_t config = manager->config;
    const mosaico_peer_session_event_cb_t event_cb = manager->event_cb;
    void *user_ctx = manager->user_ctx;
    memset(manager, 0, sizeof(*manager));
    manager->device_id = device_id;
    manager->config = config;
    manager->event_cb = event_cb;
    manager->user_ctx = user_ctx;
}

esp_err_t mosaico_peer_session_local_contact(
    mosaico_peer_session_manager_t *manager,
    mosaico_edge_t local_edge,
    uint16_t relative_rotation,
    uint32_t now_ms)
{
    const int index = edge_index(local_edge);
    ESP_RETURN_ON_FALSE(manager && manager->event_cb && index >= 0 &&
                        relative_rotation <= 270 && relative_rotation % 90 == 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid local contact");
    mosaico_peer_session_slot_t *slot = &manager->slots[index];
    slot->local_present = true;
    slot->relative_rotation = relative_rotation;
    if (slot->state == MOSAICO_PEER_SESSION_IDLE ||
        slot->state == MOSAICO_PEER_SESSION_RELEASING) {
        begin_claim(manager, slot, local_edge, relative_rotation, now_ms);
    }
    return ESP_OK;
}

esp_err_t mosaico_peer_session_local_release(
    mosaico_peer_session_manager_t *manager,
    mosaico_edge_t local_edge,
    uint32_t now_ms)
{
    const int index = edge_index(local_edge);
    ESP_RETURN_ON_FALSE(manager && manager->event_cb && index >= 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid local release");
    mosaico_peer_session_slot_t *slot = &manager->slots[index];
    slot->local_present = false;
    if (slot->state == MOSAICO_PEER_SESSION_IDLE) {
        return ESP_OK;
    }
    notify_detached(manager, slot, MOSAICO_PEER_DETACH_LOCAL_RELEASE);
    if (slot->peer_id == 0) {
        clear_slot(slot);
        return ESP_OK;
    }
    slot->state = MOSAICO_PEER_SESSION_RELEASING;
    slot->started_ms = now_ms;
    slot->release_tx_count = 1;
    send_message(manager, slot, MOSAICO_PEER_MSG_CONTACT_RELEASE,
                 slot->peer_id, now_ms);
    return ESP_OK;
}

static void receive_claim(
    mosaico_peer_session_manager_t *manager,
    mosaico_peer_session_slot_t *slot,
    const mosaico_peer_message_t *message,
    uint32_t now_ms)
{
    if (!slot->local_present || slot->state == MOSAICO_PEER_SESSION_RELEASING) {
        return;
    }
    if (slot->state == MOSAICO_PEER_SESSION_COMMITTED) {
        if (slot->peer_id == message->source_id &&
            slot->session_id == message->session_id) {
            slot->last_rx_ms = now_ms;
            send_message(manager, slot, MOSAICO_PEER_MSG_CONTACT_COMMIT,
                         slot->peer_id, now_ms);
        }
        return;
    }

    if (manager->device_id < message->source_id) {
        if (slot->state != MOSAICO_PEER_SESSION_CLAIMING) {
            begin_claim(manager, slot, slot->local_edge,
                        slot->relative_rotation, now_ms);
        } else {
            send_message(manager, slot, MOSAICO_PEER_MSG_CONTACT_CLAIM, 0, now_ms);
        }
        return;
    }

    if (slot->state == MOSAICO_PEER_SESSION_WAIT_COMMIT &&
        slot->peer_id != 0 && slot->peer_id < message->source_id) {
        return;
    }

    slot->state = MOSAICO_PEER_SESSION_WAIT_COMMIT;
    slot->peer_id = message->source_id;
    slot->session_id = message->session_id;
    slot->started_ms = now_ms;
    slot->last_rx_ms = now_ms;
    slot->peer_edge = message->local_edge;
    if (mosaico_topology_resolve_display_rotation(
            slot->local_edge, slot->peer_edge,
            &slot->relative_rotation) != ESP_OK) {
        return;
    }
    send_message(manager, slot, MOSAICO_PEER_MSG_CONTACT_ACK,
                 slot->peer_id, now_ms);
}

static void receive_ack(
    mosaico_peer_session_manager_t *manager,
    mosaico_peer_session_slot_t *slot,
    const mosaico_peer_message_t *message,
    uint32_t now_ms)
{
    if (slot->session_id != message->session_id || !slot->local_present) {
        return;
    }
    if (slot->state == MOSAICO_PEER_SESSION_COMMITTED) {
        if (slot->peer_id == message->source_id) {
            slot->last_rx_ms = now_ms;
            send_message(manager, slot, MOSAICO_PEER_MSG_CONTACT_COMMIT,
                         slot->peer_id, now_ms);
        }
        return;
    }
    if (slot->state != MOSAICO_PEER_SESSION_CLAIMING) {
        return;
    }
    slot->state = MOSAICO_PEER_SESSION_COMMITTED;
    slot->peer_id = message->source_id;
    slot->peer_edge = message->local_edge;
    if (mosaico_topology_resolve_display_rotation(
            slot->local_edge, slot->peer_edge,
            &slot->relative_rotation) != ESP_OK) {
        slot->state = MOSAICO_PEER_SESSION_CLAIMING;
        return;
    }
    slot->last_rx_ms = now_ms;
    notify_attached(manager, slot);
    send_message(manager, slot, MOSAICO_PEER_MSG_CONTACT_COMMIT,
                 slot->peer_id, now_ms);
}

static void receive_commit(
    mosaico_peer_session_manager_t *manager,
    mosaico_peer_session_slot_t *slot,
    const mosaico_peer_message_t *message,
    uint32_t now_ms)
{
    if (!slot->local_present || slot->peer_id != message->source_id ||
        slot->session_id != message->session_id) {
        return;
    }
    slot->last_rx_ms = now_ms;
    if (slot->state == MOSAICO_PEER_SESSION_WAIT_COMMIT) {
        slot->state = MOSAICO_PEER_SESSION_COMMITTED;
        notify_attached(manager, slot);
        send_message(manager, slot, MOSAICO_PEER_MSG_CONTACT_COMMIT,
                     slot->peer_id, now_ms);
    }
}

static void receive_release(
    mosaico_peer_session_manager_t *manager,
    mosaico_peer_session_slot_t *slot,
    const mosaico_peer_message_t *message,
    uint32_t now_ms)
{
    if (slot->peer_id != message->source_id ||
        slot->session_id != message->session_id) {
        return;
    }
    if (slot->state == MOSAICO_PEER_SESSION_RELEASING) {
        slot->last_rx_ms = now_ms;
        return;
    }
    notify_detached(manager, slot, MOSAICO_PEER_DETACH_REMOTE_RELEASE);
    slot->state = MOSAICO_PEER_SESSION_RELEASING;
    slot->started_ms = now_ms;
    slot->last_rx_ms = now_ms;
    slot->release_tx_count = 1;
    send_message(manager, slot, MOSAICO_PEER_MSG_CONTACT_RELEASE,
                 slot->peer_id, now_ms);
}

esp_err_t mosaico_peer_session_receive(
    mosaico_peer_session_manager_t *manager,
    const mosaico_peer_message_t *message,
    uint32_t now_ms)
{
    ESP_RETURN_ON_FALSE(manager && manager->event_cb && message,
                        ESP_ERR_INVALID_ARG, TAG, "invalid received message");
    if (message->source_id == 0 || message->source_id == manager->device_id ||
        (message->target_id != 0 && message->target_id != manager->device_id)) {
        return ESP_OK;
    }
    if (message->type < MOSAICO_PEER_MSG_CONTACT_CLAIM ||
        message->type > MOSAICO_PEER_MSG_CONTACT_RELEASE) {
        return ESP_OK;
    }
    if ((message->type == MOSAICO_PEER_MSG_CONTACT_CLAIM && message->target_id != 0) ||
        (message->type != MOSAICO_PEER_MSG_CONTACT_CLAIM &&
         message->target_id != manager->device_id)) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(message->session_id != 0 && message_edges_are_valid(message),
                        ESP_ERR_INVALID_ARG, TAG, "invalid contact session frame");
    mosaico_peer_session_slot_t *slot = NULL;
    if (message->type == MOSAICO_PEER_MSG_CONTACT_CLAIM) {
        slot = select_claim_slot(manager, message);
        if (!slot) {
            return ESP_OK;
        }
    } else {
        const int index = edge_index(message->peer_edge);
        slot = &manager->slots[index];
        ESP_RETURN_ON_FALSE(message_matches_slot(slot, message),
                            ESP_ERR_INVALID_ARG, TAG,
                            "contact frame does not match negotiated edges");
    }

    switch (message->type) {
    case MOSAICO_PEER_MSG_CONTACT_CLAIM:
        receive_claim(manager, slot, message, now_ms);
        break;
    case MOSAICO_PEER_MSG_CONTACT_ACK:
        receive_ack(manager, slot, message, now_ms);
        break;
    case MOSAICO_PEER_MSG_CONTACT_COMMIT:
        receive_commit(manager, slot, message, now_ms);
        break;
    case MOSAICO_PEER_MSG_CONTACT_RELEASE:
        receive_release(manager, slot, message, now_ms);
        break;
    default:
        break;
    }
    return ESP_OK;
}

void mosaico_peer_session_tick(
    mosaico_peer_session_manager_t *manager,
    uint32_t now_ms)
{
    if (!manager || !manager->event_cb) {
        return;
    }
    for (int i = 0; i < MOSAICO_PEER_SESSION_EDGE_COUNT; ++i) {
        mosaico_peer_session_slot_t *slot = &manager->slots[i];
        switch (slot->state) {
        case MOSAICO_PEER_SESSION_CLAIMING:
            if (elapsed_ms(now_ms, slot->started_ms) >=
                manager->config.handshake_timeout_ms) {
                begin_claim(manager, slot, slot->local_edge,
                            slot->relative_rotation, now_ms);
            } else if (elapsed_ms(now_ms, slot->last_tx_ms) >=
                       manager->config.retry_interval_ms) {
                send_message(manager, slot, MOSAICO_PEER_MSG_CONTACT_CLAIM, 0, now_ms);
            }
            break;

        case MOSAICO_PEER_SESSION_WAIT_COMMIT:
            if (elapsed_ms(now_ms, slot->started_ms) >=
                manager->config.handshake_timeout_ms) {
                begin_claim(manager, slot, slot->local_edge,
                            slot->relative_rotation, now_ms);
            } else if (elapsed_ms(now_ms, slot->last_tx_ms) >=
                       manager->config.retry_interval_ms) {
                send_message(manager, slot, MOSAICO_PEER_MSG_CONTACT_ACK,
                             slot->peer_id, now_ms);
            }
            break;

        case MOSAICO_PEER_SESSION_COMMITTED:
            if (elapsed_ms(now_ms, slot->last_rx_ms) >=
                manager->config.stale_timeout_ms) {
                notify_detached(manager, slot, MOSAICO_PEER_DETACH_STALE);
                if (slot->local_present) {
                    begin_claim(manager, slot, slot->local_edge,
                                slot->relative_rotation, now_ms);
                } else {
                    clear_slot(slot);
                }
            } else if (elapsed_ms(now_ms, slot->last_tx_ms) >=
                       manager->config.heartbeat_interval_ms) {
                send_message(manager, slot, MOSAICO_PEER_MSG_CONTACT_COMMIT,
                             slot->peer_id, now_ms);
            }
            break;

        case MOSAICO_PEER_SESSION_RELEASING:
            if (slot->release_tx_count < manager->config.release_retries &&
                elapsed_ms(now_ms, slot->last_tx_ms) >=
                manager->config.retry_interval_ms) {
                slot->release_tx_count++;
                send_message(manager, slot, MOSAICO_PEER_MSG_CONTACT_RELEASE,
                             slot->peer_id, now_ms);
            } else if (slot->release_tx_count >= manager->config.release_retries &&
                       elapsed_ms(now_ms, slot->last_tx_ms) >=
                       manager->config.retry_interval_ms) {
                if (slot->local_present) {
                    begin_claim(manager, slot, slot->local_edge,
                                slot->relative_rotation, now_ms);
                } else {
                    clear_slot(slot);
                }
            }
            break;

        case MOSAICO_PEER_SESSION_IDLE:
        default:
            break;
        }
    }
}

const mosaico_peer_session_slot_t *mosaico_peer_session_get(
    const mosaico_peer_session_manager_t *manager,
    mosaico_edge_t local_edge)
{
    const int index = edge_index(local_edge);
    return manager && index >= 0 ? &manager->slots[index] : NULL;
}

const char *mosaico_peer_session_state_to_string(mosaico_peer_session_state_t state)
{
    static const char *const names[] = {
        [MOSAICO_PEER_SESSION_IDLE] = "IDLE",
        [MOSAICO_PEER_SESSION_CLAIMING] = "CLAIMING",
        [MOSAICO_PEER_SESSION_WAIT_COMMIT] = "WAIT_COMMIT",
        [MOSAICO_PEER_SESSION_COMMITTED] = "COMMITTED",
        [MOSAICO_PEER_SESSION_RELEASING] = "RELEASING",
    };
    return state <= MOSAICO_PEER_SESSION_RELEASING && names[state] ?
        names[state] : "INVALID";
}
