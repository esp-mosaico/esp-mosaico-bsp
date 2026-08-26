/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "interaction_controller.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mosaico_game.h"
#include "mosaico_mag_classifier.h"
#include "mosaico_mesh.h"
#include "mosaico_peer_link.h"
#include "mosaico_peer_session.h"
#include "mosaico_topology.h"
#include "sdkconfig.h"
#include "diagnostic_stream.h"

static const char *TAG = "interact_ctrl";

#define MOCK_SAMPLE_PERIOD_MS 50
#define HARDWARE_SAMPLE_PERIOD_MS 50
#define MOCK_STRENGTH         1.0f
#define MOCK_CONFIDENCE       1.0f
#define LOCAL_PAIR_CATEGORY   1U
#define PEER_SESSION_TICK_MS  50U
#define ENERGY_WIRE_VERSION   5U
#define TOPOLOGY_SYNC_PERIOD_MS 1000U
#define TOPOLOGY_SYNC_STAGGER_MAX_MS 200U
#define MESH_TOUR_SETTLE_MS     1500U
#define ENERGY_START_SENDER   (1U << 0)
#define DISPLAY_ROTATION_MAX_ATTEMPTS 3U
#define DISPLAY_ROTATION_RETRY_MS     50U

typedef struct {
    uint8_t version;
    uint8_t kind;
    uint8_t flags;
    uint8_t target_index;
    uint32_t event_id;
    uint32_t hop;
    uint32_t topology_id;
    uint64_t origin_id;
    uint64_t destination_id;
} energy_wire_event_t;

typedef struct {
    uint64_t peer_id;
    uint32_t session_id;
    mosaico_edge_t local_edge;
    mosaico_edge_t peer_edge;
    uint16_t relative_rotation;
} topology_destination_t;

_Static_assert(sizeof(energy_wire_event_t) == 32, "unexpected energy event size");

typedef struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t session_lock;
    mosaico_interaction_t interaction;
    mosaico_mag_classifier_t classifier;
    mosaico_mag_presence_tracker_t presence_tracker;
    mosaico_topology_t topology;
    mosaico_mesh_t mesh;
    mosaico_peer_session_manager_t peer_sessions;
    mosaico_energy_transfer_t energy_transfers[4];
    uint32_t energy_event_ids[4];
    energy_wire_event_t energy_routes[4];
    mosaico_idiom_chain_t idiom;
    interaction_controller_snapshot_t snapshot;
    mosaico_edge_mask_t game_applied_mask;
    TaskHandle_t display_rotation_task;
    bsp_display_rotation_t base_display_rotation;
    bsp_display_rotation_t target_display_rotation;
    uint32_t last_topology_sync_ms;
    uint32_t mesh_changed_ms;
    uint32_t mesh_tour_version;
    bool session_ready;
    bool started;
} controller_context_t;

static controller_context_t s_controller;
static const uint32_t s_expected_idiom[4] = {
    MOSAICO_EDGE_TOP,
    MOSAICO_EDGE_RIGHT,
    MOSAICO_EDGE_BOTTOM,
    MOSAICO_EDGE_LEFT,
};

static uint32_t controller_now_ms(void);
static void sync_energy_snapshot_locked(void);

static uint32_t topology_sync_stagger_ms(uint64_t device_id)
{
    const uint32_t folded_id =
        (uint32_t)device_id ^ (uint32_t)(device_id >> 32);
    return folded_id % (TOPOLOGY_SYNC_STAGGER_MAX_MS + 1U);
}

static int controller_edge_index(mosaico_edge_t edge)
{
    return edge >= MOSAICO_EDGE_TOP && edge <= MOSAICO_EDGE_LEFT ?
        (int)edge - (int)MOSAICO_EDGE_TOP : -1;
}

static mosaico_energy_transfer_t *energy_transfer_for_edge(mosaico_edge_t edge)
{
    const int index = controller_edge_index(edge);
    return index >= 0 ? &s_controller.energy_transfers[index] : NULL;
}

static void update_display_rotation_locked(
    bsp_display_rotation_t applied,
    bool pending)
{
    const bool alignment_finished =
        s_controller.snapshot.display_rotation_pending && !pending;
    s_controller.snapshot.display_rotation = (uint16_t)applied;
    s_controller.snapshot.display_rotation_delta =
        (uint16_t)(((int)applied - (int)s_controller.base_display_rotation + 360) % 360);
    s_controller.snapshot.display_rotation_pending = pending;
    if (alignment_finished) {
        for (size_t i = 0; i < 4; ++i) {
            if (s_controller.energy_transfers[i].phase == MOSAICO_ENERGY_SENDING) {
                s_controller.energy_transfers[i].started_ms = controller_now_ms();
                s_controller.energy_transfers[i].progress_permille = 0;
            }
        }
        sync_energy_snapshot_locked();
    }
}

static void display_rotation_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t notification = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &notification, portMAX_DELAY) != pdTRUE ||
            notification == 0) {
            continue;
        }
        const bsp_display_rotation_t requested =
            (bsp_display_rotation_t)(notification - 1U);
        esp_err_t ret = ESP_FAIL;
        bsp_display_rotation_t applied = bsp_display_get_rotation();
        unsigned attempt = 0;
        bool superseded = false;

        for (attempt = 1; attempt <= DISPLAY_ROTATION_MAX_ATTEMPTS; ++attempt) {
            xSemaphoreTake(s_controller.lock, portMAX_DELAY);
            superseded = s_controller.target_display_rotation != requested;
            xSemaphoreGive(s_controller.lock);
            if (superseded) {
                break;
            }

            ret = bsp_display_set_rotation(requested);
            applied = bsp_display_get_rotation();
            if (ret == ESP_OK) {
                if (applied == requested) {
                    break;
                }
                ret = ESP_ERR_INVALID_STATE;
            }
            if (attempt < DISPLAY_ROTATION_MAX_ATTEMPTS) {
                vTaskDelay(pdMS_TO_TICKS(DISPLAY_ROTATION_RETRY_MS));
            }
        }

        xSemaphoreTake(s_controller.lock, portMAX_DELAY);
        superseded = s_controller.target_display_rotation != requested;
        const bool aligned = ret == ESP_OK && applied == requested;
        const bool pending = superseded &&
            s_controller.target_display_rotation != applied;
        update_display_rotation_locked(applied, pending);
        xSemaphoreGive(s_controller.lock);

        if (superseded) {
            ESP_LOGD(TAG, "display rotation superseded: requested=%d applied=%d",
                     (int)requested, (int)applied);
        } else if (aligned) {
            ESP_LOGI(TAG, "negotiated display rotation applied: rotation=%d",
                     (int)applied);
        } else {
            ESP_LOGE(TAG, "display rotation degraded after %u attempts: "
                     "requested=%d applied=%d error=%s",
                     attempt - 1U, (int)requested, (int)applied,
                     esp_err_to_name(ret));
        }
    }
}

static void request_display_rotation(bsp_display_rotation_t target)
{
    if (!s_controller.display_rotation_task) {
        return;
    }
    const bsp_display_rotation_t applied = bsp_display_get_rotation();
    xSemaphoreTake(s_controller.lock, portMAX_DELAY);
    const bool target_changed = s_controller.target_display_rotation != target;
    const bool needs_rotation = applied != target;
    s_controller.target_display_rotation = target;
    if (needs_rotation) {
        s_controller.snapshot.display_rotation_pending = true;
    } else {
        update_display_rotation_locked(applied, false);
    }
    xSemaphoreGive(s_controller.lock);

    if (!target_changed && !needs_rotation) {
        return;
    }
    if (xTaskNotify(s_controller.display_rotation_task, (uint32_t)target + 1U,
                    eSetValueWithOverwrite) != pdPASS) {
        xSemaphoreTake(s_controller.lock, portMAX_DELAY);
        if (s_controller.target_display_rotation == target) {
            update_display_rotation_locked(bsp_display_get_rotation(), false);
        }
        xSemaphoreGive(s_controller.lock);
        ESP_LOGE(TAG, "queue display rotation failed: target=%d error=%s",
                 (int)target, esp_err_to_name(ESP_FAIL));
    }
}

static void sync_energy_snapshot_locked(void)
{
    const mosaico_energy_transfer_t *selected = NULL;
    uint8_t selected_priority = 0;
    for (size_t i = 0; i < 4; ++i) {
        const mosaico_energy_transfer_t *candidate =
            &s_controller.energy_transfers[i];
        if (!candidate->connected) {
            continue;
        }
        uint8_t priority = 1;
        if (candidate->phase == MOSAICO_ENERGY_SENDING ||
            candidate->phase == MOSAICO_ENERGY_RECEIVING) {
            priority = 4;
        } else if (candidate->phase == MOSAICO_ENERGY_WAIT_COMPLETE) {
            priority = 3;
        } else if (candidate->phase == MOSAICO_ENERGY_RECEIVED) {
            priority = 2;
        }
        if (!selected || priority > selected_priority) {
            selected = candidate;
            selected_priority = priority;
        }
    }
    if (!selected) {
        s_controller.snapshot.energy_session_id = 0;
        s_controller.snapshot.energy_event_id = 0;
        s_controller.snapshot.energy_hop = 0;
        s_controller.snapshot.energy_edge = MOSAICO_EDGE_NONE;
        s_controller.snapshot.energy_phase = MOSAICO_ENERGY_IDLE;
        s_controller.snapshot.energy_progress_permille = 0;
        return;
    }
    s_controller.snapshot.energy_session_id =
        selected->session_id;
    s_controller.snapshot.energy_event_id = s_controller.energy_event_ids[
        controller_edge_index(selected->local_edge)];
    s_controller.snapshot.energy_hop = selected->hop;
    s_controller.snapshot.energy_edge =
        selected->local_edge;
    s_controller.snapshot.energy_phase = selected->phase;
    s_controller.snapshot.energy_progress_permille =
        selected->progress_permille;
}

static void reset_energy_traversal_locked(const char *reason)
{
    bool active = false;
    for (size_t i = 0; i < 4; ++i) {
        mosaico_energy_transfer_t *transfer = &s_controller.energy_transfers[i];
        if (transfer->connected && transfer->phase != MOSAICO_ENERGY_IDLE) {
            active = true;
            (void)mosaico_energy_transfer_reset(transfer);
        }
        s_controller.energy_event_ids[i] = 0;
        memset(&s_controller.energy_routes[i], 0,
               sizeof(s_controller.energy_routes[i]));
    }
    sync_energy_snapshot_locked();
    if (active) {
        ESP_LOGI(TAG, "energy traversal reset: reason=%s topology_id=%08lx",
                 reason, (unsigned long)mosaico_mesh_get_topology_id(
                     &s_controller.mesh));
    }
}

static void send_energy_event(
    const mosaico_energy_transfer_t *transfer,
    const mosaico_energy_event_t *event,
    const energy_wire_event_t *route)
{
    if (!transfer || !event || !route ||
        event->kind == MOSAICO_ENERGY_EVENT_NONE) {
        return;
    }
    energy_wire_event_t wire = *route;
    wire.version = ENERGY_WIRE_VERSION;
    wire.kind = (uint8_t)event->kind;
    wire.hop = event->hop;
    const esp_err_t ret = mosaico_peer_link_send_broadcast_to(
        MOSAICO_PEER_MSG_GAME_EVENT, transfer->peer_id,
        transfer->session_id, transfer->local_edge, transfer->peer_edge,
        0, &wire, sizeof(wire));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "send energy event failed: kind=%u hop=%lu session=%08lx error=%s",
                 (unsigned)event->kind, (unsigned long)event->hop,
                 (unsigned long)transfer->session_id, esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "energy event queued: event=%08lx kind=%u hop=%lu peer=%012" PRIx64,
                 (unsigned long)wire.event_id, (unsigned)wire.kind,
                 (unsigned long)wire.hop, transfer->peer_id);
    }
}

static void send_energy_start(
    const mosaico_energy_transfer_t *transfer,
    const energy_wire_event_t *route,
    bool sender)
{
    if (!transfer || !transfer->connected || !route || route->event_id == 0) {
        return;
    }
    energy_wire_event_t wire = *route;
    wire.version = ENERGY_WIRE_VERSION;
    wire.kind = MOSAICO_ENERGY_EVENT_NONE;
    wire.flags = sender ? ENERGY_START_SENDER : 0;
    const esp_err_t ret = mosaico_peer_link_send_broadcast_to(
        MOSAICO_PEER_MSG_GAME_EVENT, transfer->peer_id,
        transfer->session_id, transfer->local_edge, transfer->peer_edge,
        0, &wire, sizeof(wire));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "send energy start failed: event=%08lx peer=%012" PRIx64
                 " error=%s", (unsigned long)route->event_id, transfer->peer_id,
                 esp_err_to_name(ret));
    }
}

static void sync_mesh_snapshot_locked(void)
{
    s_controller.snapshot.topology_version = s_controller.mesh.topology_version;
    s_controller.snapshot.mesh_node_count =
        (uint16_t)s_controller.mesh.connected_count;
    s_controller.snapshot.mesh_root_id = s_controller.mesh.root_id;
    s_controller.snapshot.mesh_orientation_conflict =
        s_controller.mesh.orientation_conflict;
}

static void flood_mesh_link(
    const mosaico_mesh_wire_link_t *wire,
    uint64_t excluded_peer_id)
{
    if (!wire || wire->ttl == 0) {
        return;
    }
    topology_destination_t neighbors[4] = {0};
    size_t count = 0;
    xSemaphoreTake(s_controller.lock, portMAX_DELAY);
    for (size_t edge = 0; edge < 4; ++edge) {
        const mosaico_neighbor_t *neighbor =
            &s_controller.topology.neighbors[edge];
        const mosaico_energy_transfer_t *transfer =
            &s_controller.energy_transfers[edge];
        if (neighbor->committed && transfer->connected &&
            neighbor->peer_id == transfer->peer_id &&
            neighbor->peer_id != excluded_peer_id) {
            neighbors[count++] = (topology_destination_t) {
                .peer_id = transfer->peer_id,
                .session_id = transfer->session_id,
                .local_edge = transfer->local_edge,
                .peer_edge = transfer->peer_edge,
                .relative_rotation = neighbor->relative_rotation,
            };
        }
    }
    xSemaphoreGive(s_controller.lock);
    for (size_t i = 0; i < count; ++i) {
        const esp_err_t ret = mosaico_peer_link_send_broadcast_to(
            MOSAICO_PEER_MSG_TOPOLOGY_SYNC, neighbors[i].peer_id,
            neighbors[i].session_id, neighbors[i].local_edge,
            neighbors[i].peer_edge, neighbors[i].relative_rotation,
            wire, sizeof(*wire));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "unicast topology sync failed: peer=%012" PRIx64
                     " error=%s", neighbors[i].peer_id,
                     esp_err_to_name(ret));
        }
    }
}

static void refresh_mesh_links(uint32_t now_ms)
{
    mosaico_mesh_wire_link_t wires[4] = {0};
    size_t wire_count = 0;
    xSemaphoreTake(s_controller.lock, portMAX_DELAY);
    for (size_t i = 0; i < 4; ++i) {
        if (mosaico_mesh_refresh(
                &s_controller.mesh,
                (mosaico_edge_t)(i + MOSAICO_EDGE_TOP), now_ms,
                &wires[wire_count]) == ESP_OK) {
            wire_count++;
        }
    }
    if (mosaico_mesh_expire(
            &s_controller.mesh, now_ms, MOSAICO_MESH_RECORD_STALE_MS)) {
        reset_energy_traversal_locked("topology_expired");
        sync_mesh_snapshot_locked();
        s_controller.mesh_changed_ms = now_ms;
    }
    s_controller.last_topology_sync_ms = now_ms;
    xSemaphoreGive(s_controller.lock);
    for (size_t i = 0; i < wire_count; ++i) {
        flood_mesh_link(&wires[i], 0);
    }
}

static bool prepare_mesh_route_locked(
    energy_wire_event_t *route,
    mosaico_energy_transfer_t *outgoing)
{
    if (!route || !outgoing || route->event_id == 0) {
        return false;
    }
    uint64_t device_ids[MOSAICO_MESH_MAX_NODES] = {0};
    const size_t count = mosaico_mesh_get_traversal_order(
        &s_controller.mesh, device_ids, MOSAICO_MESH_MAX_NODES);
    while (route->target_index < count &&
           device_ids[route->target_index] == s_controller.snapshot.device_id) {
        route->target_index++;
    }
    if (route->target_index >= count) {
        return false;
    }
    route->destination_id = device_ids[route->target_index];
    uint64_t next_hop_id = 0;
    mosaico_edge_t local_edge = MOSAICO_EDGE_NONE;
    if (mosaico_mesh_get_next_hop(
            &s_controller.mesh, route->destination_id,
            &next_hop_id, &local_edge) != ESP_OK) {
        return false;
    }
    mosaico_energy_transfer_t *transfer = energy_transfer_for_edge(local_edge);
    if (!transfer || !transfer->connected || transfer->peer_id != next_hop_id) {
        return false;
    }
    route->hop++;
    const int index = controller_edge_index(local_edge);
    s_controller.energy_event_ids[index] = route->event_id;
    s_controller.energy_routes[index] = *route;
    if (mosaico_energy_transfer_begin(
            transfer, true, route->hop, controller_now_ms()) != ESP_OK) {
        return false;
    }
    *outgoing = *transfer;
    sync_energy_snapshot_locked();
    return true;
}

static void publish_energy_diagnostic(
    const char *event_name,
    const mosaico_energy_transfer_t *transfer)
{
    char record[192];
    snprintf(record, sizeof(record),
             "MAGDIAG,version=1,device=%012" PRIx64
             ",event=%s,peer=%012" PRIx64 ",session=%08lx,edge=%s,phase=%s,progress=%u,t_ms=%lu",
             s_controller.snapshot.device_id, event_name,
             transfer->peer_id,
             (unsigned long)transfer->session_id,
             mosaico_edge_to_string(transfer->local_edge),
             mosaico_energy_phase_to_string(transfer->phase),
             transfer->progress_permille,
             (unsigned long)controller_now_ms());
    diagnostic_stream_publish(record);
}

static uint32_t controller_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void process_local_presence(
    mosaico_edge_t edge,
    bool present,
    uint32_t timestamp_ms)
{
    if (!s_controller.session_ready) {
        return;
    }
    if (!present) {
        xSemaphoreTake(s_controller.lock, portMAX_DELAY);
        s_controller.game_applied_mask &= ~mosaico_edge_to_mask(edge);
        xSemaphoreGive(s_controller.lock);
    }
    xSemaphoreTake(s_controller.session_lock, portMAX_DELAY);
    esp_err_t ret = present ?
        mosaico_peer_session_local_contact(
            &s_controller.peer_sessions, edge, 0, timestamp_ms) :
        mosaico_peer_session_local_release(
            &s_controller.peer_sessions, edge, timestamp_ms);
    xSemaphoreGive(s_controller.session_lock);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "process local peer presence failed: edge=%s present=%d error=%s",
                 mosaico_edge_to_string(edge), present, esp_err_to_name(ret));
    }
}

#if CONFIG_MAGNETIC_INTERACTION_USE_HARDWARE
static void hardware_presence_event_callback(
    const mosaico_mag_presence_event_t *event,
    void *user_ctx)
{
    (void)user_ctx;
    xSemaphoreTake(s_controller.lock, portMAX_DELAY);
    s_controller.snapshot.detected_mask = event->attached_mask;
    s_controller.snapshot.state = event->attached_mask ?
        MOSAICO_INTERACTION_STATE_ATTACHED : MOSAICO_INTERACTION_STATE_IDLE;
    s_controller.snapshot.attached_edge = mosaico_edge_from_mask(event->attached_mask);
    s_controller.snapshot.attached_rotation = 0;
    snprintf(s_controller.snapshot.last_event, sizeof(s_controller.snapshot.last_event),
             "%s %s",
             event->type == MOSAICO_MAG_PRESENCE_CONTACT ? "CONTACT" : "RELEASE",
             mosaico_edge_to_string(event->edge));
    xSemaphoreGive(s_controller.lock);

    char record[160];
    snprintf(record, sizeof(record),
             "MAGDIAG,version=1,device=%012" PRIx64 ",event=%s,edge=%s,mask=0x%02x,t_ms=%lu",
             s_controller.snapshot.device_id,
             event->type == MOSAICO_MAG_PRESENCE_CONTACT ? "CONTACT" : "RELEASE",
             mosaico_edge_to_string(event->edge), event->attached_mask,
             (unsigned long)event->timestamp_ms);
    diagnostic_stream_publish(record);
    process_local_presence(
        event->edge, event->type == MOSAICO_MAG_PRESENCE_CONTACT,
        event->timestamp_ms);
}
#endif

static void update_game_locked(mosaico_edge_t edge)
{
    const uint32_t peer_category =
        edge == MOSAICO_EDGE_LEFT || edge == MOSAICO_EDGE_RIGHT ? 1U : 2U;
    s_controller.snapshot.pair_match =
        mosaico_pair_matches(LOCAL_PAIR_CATEGORY, peer_category);

    if (s_controller.idiom.count < MOSAICO_IDIOM_MAX_TOKENS) {
        esp_err_t ret = mosaico_idiom_chain_append(&s_controller.idiom, (uint32_t)edge);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "append simulated idiom token failed: %s", esp_err_to_name(ret));
        }
    }
    s_controller.snapshot.idiom_count = s_controller.idiom.count;
    memcpy(s_controller.snapshot.idiom_tokens, s_controller.idiom.tokens,
           sizeof(s_controller.snapshot.idiom_tokens));
    s_controller.snapshot.idiom_complete =
        mosaico_idiom_chain_matches(&s_controller.idiom, s_expected_idiom,
                                    sizeof(s_expected_idiom) / sizeof(s_expected_idiom[0]));
}

static const char *detach_reason_to_string(mosaico_peer_detach_reason_t reason)
{
    switch (reason) {
    case MOSAICO_PEER_DETACH_LOCAL_RELEASE:  return "LOCAL_RELEASE";
    case MOSAICO_PEER_DETACH_REMOTE_RELEASE: return "REMOTE_RELEASE";
    case MOSAICO_PEER_DETACH_STALE:          return "STALE";
    default:                                 return "UNKNOWN";
    }
}

static void publish_peer_diagnostic(
    const char *event_name,
    const mosaico_peer_session_event_t *event,
    const char *reason)
{
    char record[192];
    snprintf(record, sizeof(record),
             "MAGDIAG,version=1,device=%012" PRIx64
             ",event=%s,peer=%012" PRIx64 ",session=%08lx,local=%s,remote=%s,reason=%s,t_ms=%lu",
             s_controller.snapshot.device_id, event_name, event->peer_id,
             (unsigned long)event->session_id,
             mosaico_edge_to_string(event->local_edge),
             mosaico_edge_to_string(event->peer_edge), reason,
             (unsigned long)controller_now_ms());
    diagnostic_stream_publish(record);
}

static void peer_session_event_callback(
    const mosaico_peer_session_event_t *event,
    void *user_ctx)
{
    (void)user_ctx;
    if (event->type == MOSAICO_PEER_SESSION_EVENT_SEND) {
        esp_err_t ret = mosaico_peer_link_send_broadcast_to(
            event->message_type, event->target_id, event->session_id,
            event->local_edge, event->peer_edge, event->relative_rotation,
            NULL, 0);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "send peer session frame failed: type=%d session=%08lx error=%s",
                     event->message_type, (unsigned long)event->session_id,
                     esp_err_to_name(ret));
        }
        return;
    }

    bool mesh_link_ready = false;
    bool rotation_requested = false;
    uint16_t mesh_rotation = 0;
    mosaico_mesh_wire_link_t mesh_wire = {0};
    xSemaphoreTake(s_controller.lock, portMAX_DELAY);
    if (event->type == MOSAICO_PEER_SESSION_EVENT_ATTACHED) {
        esp_err_t ret = mosaico_topology_attach(
            &s_controller.topology, event->local_edge, event->peer_id,
            event->peer_edge, event->relative_rotation);
        if (ret == ESP_OK) {
            const uint32_t previous_topology_version =
                s_controller.mesh.topology_version;
            const esp_err_t mesh_ret = mosaico_mesh_attach(
                &s_controller.mesh, event->local_edge, event->peer_id,
                event->peer_edge, controller_now_ms(), &mesh_wire);
            if (mesh_ret == ESP_OK) {
                mesh_link_ready = true;
                if (s_controller.mesh.topology_version !=
                    previous_topology_version) {
                    reset_energy_traversal_locked("link_attached");
                }
                sync_mesh_snapshot_locked();
                s_controller.mesh_changed_ms = controller_now_ms();
                const mosaico_mesh_node_t *local_node = mosaico_mesh_get_node(
                    &s_controller.mesh, s_controller.snapshot.device_id);
                if (local_node && local_node->pose_valid) {
                    mesh_rotation = local_node->rotation_degrees;
                    rotation_requested = true;
                }
            } else {
                ESP_LOGE(TAG, "attach mesh link failed: peer=%012" PRIx64
                         " edge=%s error=%s", event->peer_id,
                         mosaico_edge_to_string(event->local_edge),
                         esp_err_to_name(mesh_ret));
            }
            s_controller.snapshot.committed_mask |=
                mosaico_edge_to_mask(event->local_edge);
            s_controller.snapshot.peer_connected = true;
            s_controller.snapshot.last_peer_id = event->peer_id;
            strlcpy(s_controller.snapshot.last_event, "PEER ATTACHED",
                    sizeof(s_controller.snapshot.last_event));
            mosaico_energy_transfer_t *transfer =
                energy_transfer_for_edge(event->local_edge);
            const bool duplicate_energy_session = transfer &&
                transfer->connected && transfer->peer_id == event->peer_id &&
                transfer->session_id == event->session_id &&
                transfer->local_edge == event->local_edge &&
                transfer->peer_edge == event->peer_edge;
            esp_err_t app_ret = mosaico_energy_transfer_connect(
                transfer, event->peer_id,
                event->session_id, event->local_edge, event->peer_edge);
            if (app_ret == ESP_OK && !duplicate_energy_session) {
                s_controller.energy_event_ids[
                    controller_edge_index(event->local_edge)] = 0;
                memset(&s_controller.energy_routes[
                           controller_edge_index(event->local_edge)], 0,
                       sizeof(energy_wire_event_t));
                sync_energy_snapshot_locked();
            } else if (app_ret != ESP_OK) {
                ESP_LOGW(TAG, "connect energy route skipped: peer=%012" PRIx64
                         " session=%08lx edge=%s error=%s",
                         event->peer_id, (unsigned long)event->session_id,
                         mosaico_edge_to_string(event->local_edge),
                         esp_err_to_name(app_ret));
            }
            const mosaico_edge_mask_t edge_bit =
                mosaico_edge_to_mask(event->local_edge);
            if ((s_controller.game_applied_mask & edge_bit) == 0) {
                update_game_locked(event->local_edge);
                s_controller.game_applied_mask |= edge_bit;
            }
        } else {
            ESP_LOGE(TAG, "commit peer topology failed: local=%s peer=%012" PRIx64
                     " error=%s", mosaico_edge_to_string(event->local_edge),
                     event->peer_id, esp_err_to_name(ret));
        }
    } else {
        const uint32_t previous_topology_version =
            s_controller.mesh.topology_version;
        const esp_err_t mesh_ret = mosaico_mesh_detach(
            &s_controller.mesh, event->local_edge, event->peer_id,
            controller_now_ms(), &mesh_wire);
        if (mesh_ret == ESP_OK) {
            mesh_link_ready = true;
            if (s_controller.mesh.topology_version !=
                previous_topology_version) {
                reset_energy_traversal_locked("link_detached");
            }
            sync_mesh_snapshot_locked();
            s_controller.mesh_changed_ms = controller_now_ms();
            const mosaico_mesh_node_t *local_node = mosaico_mesh_get_node(
                &s_controller.mesh, s_controller.snapshot.device_id);
            if (local_node && local_node->pose_valid) {
                mesh_rotation = local_node->rotation_degrees;
                rotation_requested = true;
            }
        } else if (mesh_ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "detach mesh link failed: peer=%012" PRIx64
                     " edge=%s error=%s", event->peer_id,
                     mosaico_edge_to_string(event->local_edge),
                     esp_err_to_name(mesh_ret));
        }
        esp_err_t ret = mosaico_topology_detach(
            &s_controller.topology, event->local_edge, event->peer_id);
        if (ret == ESP_OK || ret == ESP_ERR_NOT_FOUND) {
            s_controller.snapshot.committed_mask &=
                ~mosaico_edge_to_mask(event->local_edge);
            s_controller.snapshot.peer_connected =
                s_controller.snapshot.committed_mask != 0;
            strlcpy(s_controller.snapshot.last_event, "PEER DETACHED",
                    sizeof(s_controller.snapshot.last_event));
            const esp_err_t app_ret = mosaico_energy_transfer_detach(
                energy_transfer_for_edge(event->local_edge), event->peer_id,
                event->session_id);
            if (app_ret == ESP_OK) {
                const int index = controller_edge_index(event->local_edge);
                s_controller.energy_event_ids[index] = 0;
                memset(&s_controller.energy_routes[index], 0,
                       sizeof(s_controller.energy_routes[index]));
                sync_energy_snapshot_locked();
            } else if (app_ret != ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "stop energy transfer failed: peer=%012" PRIx64
                         " session=%08lx error=%s",
                         event->peer_id, (unsigned long)event->session_id,
                         esp_err_to_name(app_ret));
            }
        } else {
            ESP_LOGE(TAG, "detach peer topology failed: local=%s peer=%012" PRIx64
                     " error=%s", mosaico_edge_to_string(event->local_edge),
                     event->peer_id, esp_err_to_name(ret));
        }
    }
    xSemaphoreGive(s_controller.lock);

    if (mesh_link_ready) {
        flood_mesh_link(&mesh_wire, 0);
    }
    if (rotation_requested) {
        request_display_rotation((bsp_display_rotation_t)(
            ((uint16_t)s_controller.base_display_rotation + mesh_rotation) % 360U));
    }

    if (event->type == MOSAICO_PEER_SESSION_EVENT_ATTACHED) {
        publish_peer_diagnostic("PEER_ATTACHED", event, "COMMIT");
    } else {
        publish_peer_diagnostic("PEER_DETACHED", event,
                                detach_reason_to_string(event->detach_reason));
    }
}

static void interaction_event_callback(
    const mosaico_interaction_event_t *event,
    void *user_ctx)
{
    (void)user_ctx;
    xSemaphoreTake(s_controller.lock, portMAX_DELAY);
    strlcpy(s_controller.snapshot.last_event,
            mosaico_interaction_event_to_string(event->type),
            sizeof(s_controller.snapshot.last_event));
    s_controller.snapshot.state = s_controller.interaction.state;

    switch (event->type) {
    case MOSAICO_INTERACTION_CONTACT:
        s_controller.snapshot.attached_edge = event->edge;
        s_controller.snapshot.attached_rotation = event->relative_rotation;
        break;

    case MOSAICO_INTERACTION_EDGE_CHANGED:
        s_controller.snapshot.attached_edge = event->edge;
        s_controller.snapshot.attached_rotation = event->relative_rotation;
        break;

    case MOSAICO_INTERACTION_RELEASE:
        s_controller.snapshot.attached_edge = MOSAICO_EDGE_NONE;
        s_controller.snapshot.attached_rotation = 0;
        break;

    case MOSAICO_INTERACTION_ORBIT_CW:
        s_controller.snapshot.orbit_cw_count++;
        break;

    case MOSAICO_INTERACTION_ORBIT_CCW:
        s_controller.snapshot.orbit_ccw_count++;
        break;

    case MOSAICO_INTERACTION_ORIENTATION_CHANGED:
        s_controller.snapshot.attached_rotation = event->relative_rotation;
        break;

    case MOSAICO_INTERACTION_APPROACH:
    default:
        break;
    }
    xSemaphoreGive(s_controller.lock);

    if (event->type == MOSAICO_INTERACTION_CONTACT) {
        process_local_presence(event->edge, true, event->timestamp_ms);
    } else if (event->type == MOSAICO_INTERACTION_EDGE_CHANGED) {
        process_local_presence(event->previous_edge, false, event->timestamp_ms);
        process_local_presence(event->edge, true, event->timestamp_ms);
    } else if (event->type == MOSAICO_INTERACTION_RELEASE) {
        process_local_presence(event->previous_edge, false, event->timestamp_ms);
    }
}

static void peer_receive_callback(
    const uint8_t source_mac[6],
    int8_t rssi,
    const mosaico_peer_message_t *message,
    void *user_ctx)
{
    (void)source_mac;
    (void)user_ctx;
    if (message->source_id == mosaico_peer_link_get_device_id()) {
        return;
    }
    if (message->type == MOSAICO_PEER_MSG_DIAGNOSTIC) {
        diagnostic_stream_receive(
            message->source_id, rssi, message->payload, message->payload_len);
        return;
    }
    if (message->target_id != 0 &&
        message->target_id != mosaico_peer_link_get_device_id()) {
        return;
    }
    xSemaphoreTake(s_controller.lock, portMAX_DELAY);
    s_controller.snapshot.last_peer_id = message->source_id;
    xSemaphoreGive(s_controller.lock);
    ESP_LOGD(TAG, "peer message: source=%012" PRIx64 " target=%012" PRIx64
             " type=%d edge=%s rssi=%d",
             message->source_id, message->target_id, message->type,
             mosaico_edge_to_string(message->local_edge), rssi);
    if (message->type == MOSAICO_PEER_MSG_TOPOLOGY_SYNC) {
        if (message->payload_len != sizeof(mosaico_mesh_wire_link_t)) {
            ESP_LOGW(TAG, "reject topology sync with length=%u", message->payload_len);
            return;
        }
        mosaico_mesh_wire_link_t wire = {0};
        memcpy(&wire, message->payload, sizeof(wire));
        bool changed = false;
        bool accepted = false;
        uint16_t rotation_degrees = 0;
        mosaico_mesh_wire_link_t forward = {0};
        xSemaphoreTake(s_controller.lock, portMAX_DELAY);
        bool direct_neighbor = false;
        for (size_t edge = 0; edge < 4; ++edge) {
            const mosaico_neighbor_t *neighbor =
                &s_controller.topology.neighbors[edge];
            const mosaico_energy_transfer_t *transfer =
                &s_controller.energy_transfers[edge];
            if (neighbor->committed && neighbor->peer_id == message->source_id &&
                transfer->connected &&
                transfer->session_id == message->session_id &&
                transfer->local_edge == message->peer_edge &&
                transfer->peer_edge == message->local_edge) {
                direct_neighbor = true;
                break;
            }
        }
        esp_err_t topology_ret = direct_neighbor ?
            mosaico_mesh_receive(
                &s_controller.mesh, &wire, controller_now_ms(),
                &forward, &changed) :
            ESP_ERR_NOT_FOUND;
        accepted = topology_ret == ESP_OK;
        if (accepted && changed) {
            reset_energy_traversal_locked("topology_changed");
            sync_mesh_snapshot_locked();
            s_controller.mesh_changed_ms = controller_now_ms();
            const mosaico_mesh_node_t *local_node = mosaico_mesh_get_node(
                &s_controller.mesh, s_controller.snapshot.device_id);
            if (local_node && local_node->pose_valid) {
                rotation_degrees = local_node->rotation_degrees;
            }
        }
        xSemaphoreGive(s_controller.lock);
        if (topology_ret != ESP_OK && topology_ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "reject topology route: source=%012" PRIx64 " error=%s",
                     message->source_id, esp_err_to_name(topology_ret));
            return;
        }
        if (changed) {
            request_display_rotation((bsp_display_rotation_t)(
                ((uint16_t)s_controller.base_display_rotation + rotation_degrees) % 360U));
            ESP_LOGI(TAG, "mesh topology updated: nodes=%u root=%012" PRIx64
                     " rotation=%u conflict=%d",
                     s_controller.snapshot.mesh_node_count,
                     s_controller.snapshot.mesh_root_id,
                     rotation_degrees,
                     s_controller.snapshot.mesh_orientation_conflict);
        }
        if (accepted && forward.ttl > 0) {
            flood_mesh_link(&forward, message->source_id);
        }
        return;
    }
    if (message->type == MOSAICO_PEER_MSG_GAME_EVENT) {
        if (message->payload_len != sizeof(energy_wire_event_t)) {
            ESP_LOGW(TAG, "reject energy event with length=%u", message->payload_len);
            return;
        }
        energy_wire_event_t wire = {0};
        memcpy(&wire, message->payload, sizeof(wire));
        if (wire.version != ENERGY_WIRE_VERSION ||
            wire.kind > MOSAICO_ENERGY_EVENT_COMPLETE ||
            (wire.flags & ~ENERGY_START_SENDER) != 0 || wire.event_id == 0 ||
            wire.hop == 0 || wire.topology_id == 0 ||
            wire.origin_id == 0 || wire.destination_id == 0) {
            ESP_LOGW(TAG, "reject invalid energy route: version=%u kind=%u",
                     wire.version, wire.kind);
            return;
        }
        mosaico_energy_transfer_t *transfer =
            energy_transfer_for_edge(message->peer_edge);
        const int edge_index = controller_edge_index(message->peer_edge);
        mosaico_energy_event_t response = {0};
        mosaico_energy_transfer_t transfer_snapshot = {0};
        uint32_t local_topology_id = 0;
        xSemaphoreTake(s_controller.lock, portMAX_DELAY);
        local_topology_id = mosaico_mesh_get_topology_id(&s_controller.mesh);
        const bool matches_session = transfer && transfer->peer_id == message->source_id &&
            transfer->session_id == message->session_id &&
            transfer->local_edge == message->peer_edge &&
            transfer->peer_edge == message->local_edge;
        const bool matches_route = edge_index >= 0 &&
            s_controller.energy_event_ids[edge_index] == wire.event_id &&
            s_controller.energy_routes[edge_index].origin_id == wire.origin_id &&
            s_controller.energy_routes[edge_index].topology_id == wire.topology_id &&
            transfer && transfer->hop == wire.hop;
        esp_err_t app_ret = wire.topology_id == local_topology_id ?
            ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
        if (wire.topology_id == local_topology_id &&
            matches_session && wire.kind == MOSAICO_ENERGY_EVENT_NONE) {
            const bool sender = (wire.flags & ENERGY_START_SENDER) != 0;
            app_ret = matches_route ? ESP_OK : mosaico_energy_transfer_begin(
                transfer, sender, wire.hop, controller_now_ms());
            if (app_ret == ESP_OK) {
                s_controller.energy_event_ids[edge_index] = wire.event_id;
                s_controller.energy_routes[edge_index] = wire;
            }
        } else if (wire.topology_id == local_topology_id && matches_session) {
            if (wire.kind == MOSAICO_ENERGY_EVENT_HANDOFF &&
                !matches_route) {
                app_ret = mosaico_energy_transfer_begin(
                    transfer, false, wire.hop, controller_now_ms());
                if (app_ret == ESP_OK) {
                    s_controller.energy_event_ids[edge_index] = wire.event_id;
                    s_controller.energy_routes[edge_index] = wire;
                }
            } else if (matches_route) {
                app_ret = ESP_OK;
            }
            if (app_ret == ESP_OK) {
                app_ret = mosaico_energy_transfer_receive(
                    transfer, (mosaico_energy_event_kind_t)wire.kind, wire.hop,
                    controller_now_ms(), &response);
            }
        }
        if (app_ret == ESP_OK) {
            sync_energy_snapshot_locked();
            transfer_snapshot = *transfer;
        }
        xSemaphoreGive(s_controller.lock);
        if (app_ret != ESP_OK) {
            ESP_LOGW(TAG, "reject energy event: source=%012" PRIx64
                     " kind=%u event=%08lx hop=%lu session=%08lx "
                     "topology=%08lx local=%08lx session_match=%d "
                     "route_match=%d error=%s",
                     message->source_id, wire.kind, (unsigned long)wire.event_id,
                     (unsigned long)wire.hop,
                     (unsigned long)message->session_id,
                     (unsigned long)wire.topology_id,
                     (unsigned long)local_topology_id,
                     matches_session, matches_route,
                     esp_err_to_name(app_ret));
            return;
        }
        ESP_LOGI(TAG, "energy event received: kind=%u hop=%lu session=%08lx phase=%s",
                 wire.kind, (unsigned long)wire.hop,
                 (unsigned long)message->session_id,
                 mosaico_energy_phase_to_string(transfer_snapshot.phase));
        send_energy_event(&transfer_snapshot, &response, &wire);
        if (transfer_snapshot.phase == MOSAICO_ENERGY_SENT) {
            publish_energy_diagnostic("ENERGY_COMPLETE", &transfer_snapshot);
        }
        return;
    }
    if (!s_controller.session_ready) {
        return;
    }
    xSemaphoreTake(s_controller.session_lock, portMAX_DELAY);
    esp_err_t ret = mosaico_peer_session_receive(
        &s_controller.peer_sessions, message, controller_now_ms());
    xSemaphoreGive(s_controller.session_lock);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "reject peer session frame: source=%012" PRIx64
                 " type=%d error=%s", message->source_id, message->type,
                 esp_err_to_name(ret));
    }
}

static void peer_session_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        const uint32_t now_ms = controller_now_ms();
        xSemaphoreTake(s_controller.session_lock, portMAX_DELAY);
        mosaico_peer_session_tick(&s_controller.peer_sessions, now_ms);
        xSemaphoreGive(s_controller.session_lock);
        const uint32_t energy_now_ms = controller_now_ms();
        mosaico_energy_event_t energy_events[4] = {0};
        mosaico_energy_transfer_t energy_snapshots[4] = {0};
        energy_wire_event_t energy_routes[4] = {0};
        size_t transition_count = 0;
        mosaico_energy_transfer_t route_starts[5] = {0};
        energy_wire_event_t start_routes[5] = {0};
        size_t route_start_count = 0;
        xSemaphoreTake(s_controller.lock, portMAX_DELAY);
        if (!s_controller.snapshot.display_rotation_pending) {
            for (size_t i = 0; i < 4; ++i) {
                mosaico_energy_event_t event = {0};
                mosaico_energy_transfer_t *transfer =
                    &s_controller.energy_transfers[i];
                if (!mosaico_energy_transfer_update(
                        transfer, energy_now_ms, &event)) {
                    continue;
                }
                energy_events[transition_count] = event;
                energy_snapshots[transition_count] = *transfer;
                energy_routes[transition_count] = s_controller.energy_routes[i];
                transition_count++;
                if (transfer->phase == MOSAICO_ENERGY_RECEIVED &&
                    s_controller.energy_routes[i].topology_id != 0 &&
                    route_start_count < 5) {
                    energy_wire_event_t route = s_controller.energy_routes[i];
                    route.kind = MOSAICO_ENERGY_EVENT_NONE;
                    route.flags = 0;
                    if (prepare_mesh_route_locked(
                            &route, &route_starts[route_start_count])) {
                        const bool reverses_on_same_edge =
                            route_starts[route_start_count].local_edge ==
                            transfer->local_edge;
                        start_routes[route_start_count++] = route;
                        if (!reverses_on_same_edge) {
                            (void)mosaico_energy_transfer_reset(transfer);
                            s_controller.energy_event_ids[i] = 0;
                            memset(&s_controller.energy_routes[i], 0,
                                   sizeof(s_controller.energy_routes[i]));
                        }
                    }
                }
            }
        }
        sync_energy_snapshot_locked();
        const bool topology_sync_due =
            (uint32_t)(energy_now_ms - s_controller.last_topology_sync_ms) >=
            TOPOLOGY_SYNC_PERIOD_MS;
        uint64_t traversal[MOSAICO_MESH_MAX_NODES] = {0};
        const size_t traversal_count = mosaico_mesh_get_traversal_order(
            &s_controller.mesh, traversal, MOSAICO_MESH_MAX_NODES);
        if (route_start_count < 5 && traversal_count > 1 &&
            !s_controller.mesh.orientation_conflict &&
            traversal[0] == s_controller.snapshot.device_id &&
            s_controller.mesh_tour_version != s_controller.mesh.topology_version &&
            (uint32_t)(energy_now_ms - s_controller.mesh_changed_ms) >=
                MESH_TOUR_SETTLE_MS) {
            energy_wire_event_t route = {
                .version = ENERGY_WIRE_VERSION,
                .event_id = energy_now_ms ^ (uint32_t)s_controller.snapshot.device_id ^
                    s_controller.mesh.topology_version,
                .topology_id = mosaico_mesh_get_topology_id(&s_controller.mesh),
                .origin_id = s_controller.snapshot.device_id,
            };
            if (route.event_id == 0) {
                route.event_id = 1;
            }
            if (prepare_mesh_route_locked(
                    &route, &route_starts[route_start_count])) {
                start_routes[route_start_count++] = route;
                s_controller.mesh_tour_version = s_controller.mesh.topology_version;
                ESP_LOGI(TAG, "mesh traversal started: event=%08lx nodes=%u "
                         "start=%012" PRIx64 " end=%012" PRIx64,
                         (unsigned long)route.event_id,
                         (unsigned)traversal_count,
                         traversal[0], traversal[traversal_count - 1]);
            }
        }
        xSemaphoreGive(s_controller.lock);
        for (size_t i = 0; i < transition_count; ++i) {
            const mosaico_energy_transfer_t *energy_snapshot = &energy_snapshots[i];
            const mosaico_energy_event_t *energy_event = &energy_events[i];
            ESP_LOGI(TAG, "energy transition: event=%08lx topology=%08lx "
                     "peer=%012" PRIx64 " session=%08lx phase=%s kind=%u hop=%lu",
                     (unsigned long)energy_routes[i].event_id,
                     (unsigned long)energy_routes[i].topology_id,
                     energy_snapshot->peer_id,
                     (unsigned long)energy_snapshot->session_id,
                     mosaico_energy_phase_to_string(energy_snapshot->phase),
                     (unsigned)energy_event->kind,
                     (unsigned long)energy_event->hop);
            send_energy_event(energy_snapshot, energy_event, &energy_routes[i]);
            if (energy_snapshot->phase == MOSAICO_ENERGY_SENT ||
                energy_snapshot->phase == MOSAICO_ENERGY_RECEIVED) {
                publish_energy_diagnostic("ENERGY_COMPLETE", energy_snapshot);
            }
        }
        for (size_t i = 0; i < route_start_count; ++i) {
            send_energy_start(&route_starts[i], &start_routes[i], false);
            ESP_LOGI(TAG, "mesh energy routed: event=%08lx hop=%lu destination=%012" PRIx64
                     " peer=%012" PRIx64 " edge=%s target=%u",
                     (unsigned long)start_routes[i].event_id,
                     (unsigned long)start_routes[i].hop,
                     start_routes[i].destination_id,
                     route_starts[i].peer_id,
                     mosaico_edge_to_string(route_starts[i].local_edge),
                     start_routes[i].target_index);
        }
        if (topology_sync_due) {
            refresh_mesh_links(energy_now_ms);
        }
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PEER_SESSION_TICK_MS));
    }
}

#if !CONFIG_MAGNETIC_INTERACTION_USE_HARDWARE
static void mock_sample_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        mosaico_mag_observation_t observation = {
            .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
        };
        xSemaphoreTake(s_controller.lock, portMAX_DELAY);
        observation.edge = s_controller.snapshot.simulated_edge;
        observation.relative_rotation = s_controller.snapshot.simulated_rotation;
        observation.valid = observation.edge != MOSAICO_EDGE_NONE;
        observation.strength = observation.valid ? MOCK_STRENGTH : 0.0f;
        observation.confidence = observation.valid ? MOCK_CONFIDENCE : 0.0f;
        xSemaphoreGive(s_controller.lock);

        esp_err_t ret = mosaico_interaction_process(&s_controller.interaction, &observation);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "process mock observation failed: %s", esp_err_to_name(ret));
        }
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MOCK_SAMPLE_PERIOD_MS));
    }
}
#endif

#if CONFIG_MAGNETIC_INTERACTION_USE_HARDWARE
static void hardware_sample_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    bool previous_valid = false;
    mosaico_mag_calibration_state_t previous_calibration = MOSAICO_MAG_CALIBRATING;
    while (1) {
        struct bmm150_mag_data right = {0};
        struct bmm150_mag_data left = {0};
        const esp_err_t right_ret = bsp_magnetometer_read(BSP_MAGNETOMETER_0, &right);
        const esp_err_t left_ret = bsp_magnetometer_read(BSP_MAGNETOMETER_1, &left);
        const bool sensors_valid = right_ret == ESP_OK && left_ret == ESP_OK;
        if (sensors_valid != previous_valid) {
            if (sensors_valid) {
                ESP_LOGI(TAG, "magnetometer samples recovered");
            } else {
                ESP_LOGW(TAG, "magnetometer sample invalid: right=%s left=%s",
                         esp_err_to_name(right_ret), esp_err_to_name(left_ret));
            }
            previous_valid = sensors_valid;
        }

        const mosaico_mag_sample_t sample = {
            .right = {
                .valid = right_ret == ESP_OK,
                .x = right.x,
                .y = right.y,
                .z = right.z,
            },
            .left = {
                .valid = left_ret == ESP_OK,
                .x = left.x,
                .y = left.y,
                .z = left.z,
            },
            .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
        };
        mosaico_mag_classification_t classification = {0};
        esp_err_t ret = mosaico_mag_classifier_process(
            &s_controller.classifier, &sample, &classification);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "classify magnetometer sample failed: %s", esp_err_to_name(ret));
        } else {
            ret = mosaico_mag_presence_tracker_process(
                &s_controller.presence_tracker, &classification);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "process magnetic presence failed: %s", esp_err_to_name(ret));
            }
        }

        xSemaphoreTake(s_controller.lock, portMAX_DELAY);
        s_controller.snapshot.sensors_valid = sensors_valid && classification.valid;
        s_controller.snapshot.saturated = classification.saturated;
        s_controller.snapshot.filtered_samples = classification.filtered_samples;
        s_controller.snapshot.calibration_state = s_controller.classifier.calibration_state;
        s_controller.snapshot.baseline_right_y = s_controller.classifier.baseline_right_y;
        xSemaphoreGive(s_controller.lock);
        if (s_controller.classifier.calibration_state != previous_calibration) {
            previous_calibration = s_controller.classifier.calibration_state;
            const char *state = previous_calibration == MOSAICO_MAG_CALIBRATION_READY ?
                "READY" : previous_calibration == MOSAICO_MAG_CALIBRATION_FAILED ?
                "FAILED" : "CALIBRATING";
            char record[160];
            snprintf(record, sizeof(record),
                     "MAGDIAG,version=1,device=%012" PRIx64
                     ",event=CALIBRATION,state=%s,baseline_right_y=%ld,rejections=%u,t_ms=%lu",
                     s_controller.snapshot.device_id, state,
                     (long)s_controller.classifier.baseline_right_y,
                     s_controller.classifier.calibration_rejections,
                     (unsigned long)sample.timestamp_ms);
            diagnostic_stream_publish(record);
        }
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(HARDWARE_SAMPLE_PERIOD_MS));
    }
}
#endif

esp_err_t interaction_controller_start(void)
{
    ESP_RETURN_ON_FALSE(!s_controller.started, ESP_ERR_INVALID_STATE, TAG,
                        "controller is already running");
    memset(&s_controller, 0, sizeof(s_controller));
    s_controller.lock = xSemaphoreCreateMutex();
    s_controller.session_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_controller.lock && s_controller.session_lock,
                        ESP_ERR_NO_MEM, TAG, "create controller mutexes failed");
    s_controller.base_display_rotation = bsp_display_get_rotation();
    s_controller.target_display_rotation = s_controller.base_display_rotation;
    s_controller.snapshot.display_rotation =
        (uint16_t)s_controller.base_display_rotation;
    mosaico_topology_init(&s_controller.topology);
    mosaico_idiom_chain_reset(&s_controller.idiom);
    strlcpy(s_controller.snapshot.last_event, "READY",
            sizeof(s_controller.snapshot.last_event));
#if CONFIG_MAGNETIC_INTERACTION_USE_HARDWARE
    s_controller.snapshot.hardware_source = true;
#endif

    esp_err_t ret = mosaico_interaction_init(
        &s_controller.interaction, NULL, interaction_event_callback, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "initialize interaction engine failed: %s", esp_err_to_name(ret));
        return ret;
    }

    mosaico_peer_link_config_t peer_config =
        MOSAICO_PEER_LINK_CONFIG_DEFAULT();
    peer_config.network_id = CONFIG_MAGNETIC_INTERACTION_NETWORK_ID;
    peer_config.channel = CONFIG_MAGNETIC_INTERACTION_ESPNOW_CHANNEL;
    peer_config.receive_queue_depth = 64;
    ret = mosaico_peer_link_init(&peer_config, peer_receive_callback, NULL);
    if (ret == ESP_OK) {
        s_controller.snapshot.radio_ready = true;
        s_controller.snapshot.device_id = mosaico_peer_link_get_device_id();
        ret = mosaico_mesh_init_with_boot_id(
            &s_controller.mesh, s_controller.snapshot.device_id,
            mosaico_peer_link_get_boot_id());
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "initialize mesh topology failed: %s", esp_err_to_name(ret));
            return ret;
        }
        const uint32_t now_ms = controller_now_ms();
        const uint32_t stagger_ms = topology_sync_stagger_ms(
            s_controller.snapshot.device_id);
        /* Offset only the first deadline; later refreshes retain the 1 s cadence
         * and remain well inside the mesh stale timeout. */
        s_controller.last_topology_sync_ms =
            now_ms - (TOPOLOGY_SYNC_PERIOD_MS - stagger_ms);
        sync_mesh_snapshot_locked();
        for (size_t i = 0; i < 4; ++i) {
            ret = mosaico_energy_transfer_init(
                &s_controller.energy_transfers[i], s_controller.snapshot.device_id,
                MOSAICO_ENERGY_TRANSFER_DEFAULT_DURATION_MS);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "initialize edge energy transfer failed: edge=%u error=%s",
                         (unsigned)i, esp_err_to_name(ret));
                return ret;
            }
        }
        if (xTaskCreate(display_rotation_task, "display_rotation", 4096, NULL, 4,
                        &s_controller.display_rotation_task) != pdPASS) {
            ESP_LOGE(TAG, "create display rotation task failed");
            return ESP_ERR_NO_MEM;
        }
        ret = mosaico_peer_session_init(
            &s_controller.peer_sessions, s_controller.snapshot.device_id,
            NULL, peer_session_event_callback, NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "initialize peer sessions failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_controller.session_ready = true;
        if (xTaskCreate(peer_session_task, "peer_session", 4096, NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "create peer session task failed");
            return ESP_ERR_NO_MEM;
        }
        esp_err_t diag_ret = diagnostic_stream_start();
        if (diag_ret != ESP_OK) {
            ESP_LOGW(TAG, "ESP-NOW diagnostics unavailable: %s",
                     esp_err_to_name(diag_ret));
        }
    } else {
        ESP_LOGW(TAG, "ESP-NOW unavailable; local interaction continues: %s",
                 esp_err_to_name(ret));
    }

#if CONFIG_MAGNETIC_INTERACTION_USE_HARDWARE
    {
        ret = mosaico_mag_classifier_init(
            &s_controller.classifier, &MOSAICO_MAG_CALIBRATION_S31_V1);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "initialize magnetic classifier failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_controller.snapshot.calibration_state = MOSAICO_MAG_CALIBRATING;
        ret = mosaico_mag_presence_tracker_init(
            &s_controller.presence_tracker, NULL, hardware_presence_event_callback, NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "initialize magnetic presence tracker failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = bsp_magnetometer_init_all();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "initialize magnetometers failed: %s", esp_err_to_name(ret));
            return ret;
        }
        if (xTaskCreate(hardware_sample_task, "hardware_magnetic", 4096, NULL, 6, NULL) != pdPASS) {
            ESP_LOGE(TAG, "create hardware magnetic task failed");
            return ESP_ERR_NO_MEM;
        }
    }
#else
    if (xTaskCreate(mock_sample_task, "mock_magnetic", 4096, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create mock magnetic task failed");
        return ESP_ERR_NO_MEM;
    }
#endif
    s_controller.started = true;
#if CONFIG_MAGNETIC_INTERACTION_USE_HARDWARE
    const char *source = "hardware";
#else
    const char *source = "mock";
#endif
    ESP_LOGI(TAG, "controller started: source=%s", source);
    return ESP_OK;
}

void interaction_controller_set_mock_edge(mosaico_edge_t edge)
{
    if (!s_controller.started || s_controller.snapshot.hardware_source ||
        edge > MOSAICO_EDGE_UNKNOWN) {
        return;
    }
    xSemaphoreTake(s_controller.lock, portMAX_DELAY);
    s_controller.snapshot.simulated_edge = edge;
    xSemaphoreGive(s_controller.lock);
}

void interaction_controller_rotate_mock(void)
{
    if (!s_controller.started || s_controller.snapshot.hardware_source) {
        return;
    }
    xSemaphoreTake(s_controller.lock, portMAX_DELAY);
    s_controller.snapshot.simulated_rotation =
        (s_controller.snapshot.simulated_rotation + 180) % 360;
    xSemaphoreGive(s_controller.lock);
}

void interaction_controller_reset_game(void)
{
    if (!s_controller.started) {
        return;
    }
    xSemaphoreTake(s_controller.lock, portMAX_DELAY);
    mosaico_idiom_chain_reset(&s_controller.idiom);
    memset(s_controller.snapshot.idiom_tokens, 0,
           sizeof(s_controller.snapshot.idiom_tokens));
    s_controller.snapshot.idiom_count = 0;
    s_controller.snapshot.idiom_complete = false;
    s_controller.snapshot.pair_match = false;
    xSemaphoreGive(s_controller.lock);
}

void interaction_controller_recalibrate(void)
{
#if CONFIG_MAGNETIC_INTERACTION_USE_HARDWARE
    if (!s_controller.started || !s_controller.snapshot.hardware_source) {
        return;
    }
    xSemaphoreTake(s_controller.lock, portMAX_DELAY);
    const mosaico_edge_mask_t previous_mask = s_controller.snapshot.detected_mask;
    mosaico_mag_classifier_reset(&s_controller.classifier);
    mosaico_mag_presence_tracker_reset(&s_controller.presence_tracker);
    s_controller.snapshot.detected_mask = 0;
    s_controller.snapshot.calibration_state = MOSAICO_MAG_CALIBRATING;
    s_controller.snapshot.baseline_right_y = 0;
    s_controller.snapshot.filtered_samples = 0;
    strlcpy(s_controller.snapshot.last_event, "RECALIBRATING",
            sizeof(s_controller.snapshot.last_event));
    xSemaphoreGive(s_controller.lock);
    static const mosaico_edge_t edges[] = {
        MOSAICO_EDGE_TOP, MOSAICO_EDGE_RIGHT,
        MOSAICO_EDGE_BOTTOM, MOSAICO_EDGE_LEFT,
    };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i) {
        if (previous_mask & mosaico_edge_to_mask(edges[i])) {
            process_local_presence(edges[i], false, controller_now_ms());
        }
    }
    ESP_LOGI(TAG, "manual magnetic recalibration requested");
#endif
}

void interaction_controller_get_snapshot(interaction_controller_snapshot_t *snapshot)
{
    if (!snapshot || !s_controller.started) {
        return;
    }
    xSemaphoreTake(s_controller.lock, portMAX_DELAY);
    *snapshot = s_controller.snapshot;
    if (!snapshot->hardware_source) {
        snapshot->state = s_controller.interaction.state;
    }
    xSemaphoreGive(s_controller.lock);
}
