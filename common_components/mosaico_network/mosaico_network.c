/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_network.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MOSAICO_NETWORK_APP_VERSION 1U
#define MOSAICO_NETWORK_APP_TTL 15U
#define MOSAICO_NETWORK_RECENT_COUNT 32U
#define MOSAICO_NETWORK_PENDING_COUNT 8U
#define MOSAICO_NETWORK_EVENT_QUEUE_DEPTH 32U
#define MOSAICO_NETWORK_TASK_PERIOD_MS 50U
#define MOSAICO_NETWORK_APP_ACK (1U << 0)

static const char *TAG = "mosaico_network";

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t flags;
    uint8_t ttl;
    uint8_t payload_len;
    uint16_t service_id;
    uint16_t reserved;
    uint32_t message_id;
    uint32_t topology_id;
    uint32_t message_boot_id;
    uint64_t origin_id;
    uint64_t destination_id;
    uint8_t payload[MOSAICO_NETWORK_APP_PAYLOAD_SIZE];
} network_wire_message_t;

_Static_assert(sizeof(network_wire_message_t) == MOSAICO_PEER_PAYLOAD_SIZE,
               "network application wire size must match peer payload");

typedef struct {
    bool active;
    uint64_t peer_id;
    uint32_t session_id;
    mosaico_edge_t local_edge;
    mosaico_edge_t peer_edge;
    uint16_t relative_rotation;
} network_neighbor_t;

typedef struct {
    uint64_t origin_id;
    uint32_t message_boot_id;
    uint32_t message_id;
} recent_message_t;

typedef struct {
    bool active;
    uint8_t retries;
    uint32_t last_tx_ms;
    network_wire_message_t wire;
} pending_message_t;

struct mosaico_network {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t session_lock;
    QueueHandle_t event_queue;
    TaskHandle_t task;
    mosaico_network_config_t config;
    mosaico_peer_session_manager_t sessions;
    mosaico_mesh_t mesh;
    network_neighbor_t neighbors[MOSAICO_MESH_EDGE_COUNT];
    recent_message_t recent[MOSAICO_NETWORK_RECENT_COUNT];
    pending_message_t pending[MOSAICO_NETWORK_PENDING_COUNT];
    size_t recent_next;
    uint32_t boot_id;
    uint32_t next_message_id;
    uint32_t last_topology_sync_ms;
    bool running;
};

static uint32_t network_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int edge_index(mosaico_edge_t edge)
{
    return edge >= MOSAICO_EDGE_TOP && edge <= MOSAICO_EDGE_LEFT ?
        (int)edge - (int)MOSAICO_EDGE_TOP : -1;
}

static void fill_snapshot_locked(
    const mosaico_network_handle_t handle,
    mosaico_network_snapshot_t *snapshot)
{
    *snapshot = (mosaico_network_snapshot_t) {
        .device_id = handle->mesh.local_id,
        .root_id = handle->mesh.root_id,
        .topology_id = mosaico_mesh_get_topology_id(&handle->mesh),
        .node_count = (uint8_t)handle->mesh.connected_count,
        .conflict_flags = mosaico_mesh_get_conflicts(&handle->mesh),
    };
}

static void queue_event(
    mosaico_network_handle_t handle,
    mosaico_network_event_t *event)
{
    if (!handle->config.event_cb) {
        return;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    fill_snapshot_locked(handle, &event->snapshot);
    xSemaphoreGive(handle->lock);
    if (xQueueSend(handle->event_queue, event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full: type=%u", (unsigned)event->type);
    }
}

static void dispatch_events(mosaico_network_handle_t handle)
{
    if (!handle->config.event_cb) {
        return;
    }
    mosaico_network_event_t event;
    while (xQueueReceive(handle->event_queue, &event, 0) == pdTRUE) {
        handle->config.event_cb(&event, handle->config.user_ctx);
    }
}

static esp_err_t send_to_neighbor(
    mosaico_peer_message_type_t type,
    const network_neighbor_t *neighbor,
    const void *payload,
    size_t payload_len)
{
    return mosaico_peer_link_send_broadcast_to(
        type, neighbor->peer_id, neighbor->session_id,
        neighbor->local_edge, neighbor->peer_edge,
        neighbor->relative_rotation, payload, payload_len);
}

static size_t copy_neighbors_locked(
    mosaico_network_handle_t handle,
    network_neighbor_t *neighbors,
    uint64_t excluded_peer_id)
{
    size_t count = 0;
    for (size_t edge = 0; edge < MOSAICO_MESH_EDGE_COUNT; ++edge) {
        if (handle->neighbors[edge].active &&
            handle->neighbors[edge].peer_id != excluded_peer_id) {
            neighbors[count++] = handle->neighbors[edge];
        }
    }
    return count;
}

static void flood_mesh_link(
    mosaico_network_handle_t handle,
    const mosaico_mesh_wire_link_t *wire,
    uint64_t excluded_peer_id)
{
    if (!wire || wire->ttl == 0) {
        return;
    }
    network_neighbor_t neighbors[MOSAICO_MESH_EDGE_COUNT] = {0};
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    const size_t count = copy_neighbors_locked(
        handle, neighbors, excluded_peer_id);
    xSemaphoreGive(handle->lock);
    for (size_t i = 0; i < count; ++i) {
        const esp_err_t ret = send_to_neighbor(
            MOSAICO_PEER_MSG_TOPOLOGY_SYNC, &neighbors[i],
            wire, sizeof(*wire));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "topology unicast failed: peer=%012" PRIx64
                     " error=%s", neighbors[i].peer_id,
                     esp_err_to_name(ret));
        }
    }
}

static bool find_neighbor_locked(
    mosaico_network_handle_t handle,
    uint64_t peer_id,
    network_neighbor_t *neighbor)
{
    for (size_t edge = 0; edge < MOSAICO_MESH_EDGE_COUNT; ++edge) {
        if (handle->neighbors[edge].active &&
            handle->neighbors[edge].peer_id == peer_id) {
            if (neighbor) {
                *neighbor = handle->neighbors[edge];
            }
            return true;
        }
    }
    return false;
}

static bool direct_message_matches(
    const network_neighbor_t *neighbor,
    const mosaico_peer_message_t *message)
{
    return neighbor->session_id == message->session_id &&
        neighbor->local_edge == message->peer_edge &&
        neighbor->peer_edge == message->local_edge;
}

static bool remember_message_locked(
    mosaico_network_handle_t handle,
    uint64_t origin_id,
    uint32_t message_boot_id,
    uint32_t message_id)
{
    for (size_t i = 0; i < MOSAICO_NETWORK_RECENT_COUNT; ++i) {
        if (handle->recent[i].origin_id == origin_id &&
            handle->recent[i].message_boot_id == message_boot_id &&
            handle->recent[i].message_id == message_id) {
            return false;
        }
    }
    handle->recent[handle->recent_next] = (recent_message_t) {
        .origin_id = origin_id,
        .message_boot_id = message_boot_id,
        .message_id = message_id,
    };
    handle->recent_next =
        (handle->recent_next + 1U) % MOSAICO_NETWORK_RECENT_COUNT;
    return true;
}

static esp_err_t route_wire_message(
    mosaico_network_handle_t handle,
    const network_wire_message_t *wire,
    uint64_t excluded_peer_id)
{
    if (wire->destination_id == 0) {
        network_neighbor_t neighbors[MOSAICO_MESH_EDGE_COUNT] = {0};
        xSemaphoreTake(handle->lock, portMAX_DELAY);
        const size_t count = copy_neighbors_locked(
            handle, neighbors, excluded_peer_id);
        xSemaphoreGive(handle->lock);
        esp_err_t result = count ? ESP_OK : ESP_ERR_NOT_FOUND;
        for (size_t i = 0; i < count; ++i) {
            const esp_err_t ret = send_to_neighbor(
                MOSAICO_PEER_MSG_APP_DATA, &neighbors[i], wire,
                offsetof(network_wire_message_t, payload) + wire->payload_len);
            if (ret != ESP_OK) {
                result = ret;
            }
        }
        return result;
    }

    uint64_t next_hop_id = 0;
    mosaico_edge_t local_edge = MOSAICO_EDGE_NONE;
    network_neighbor_t neighbor = {0};
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    esp_err_t ret = mosaico_mesh_get_next_hop(
        &handle->mesh, wire->destination_id, &next_hop_id, &local_edge);
    if (ret == ESP_OK) {
        const int index = edge_index(local_edge);
        if (index < 0 || !handle->neighbors[index].active ||
            handle->neighbors[index].peer_id != next_hop_id) {
            ret = ESP_ERR_INVALID_STATE;
        } else {
            neighbor = handle->neighbors[index];
        }
    }
    xSemaphoreGive(handle->lock);
    return ret == ESP_OK ? send_to_neighbor(
        MOSAICO_PEER_MSG_APP_DATA, &neighbor, wire,
        offsetof(network_wire_message_t, payload) + wire->payload_len) : ret;
}

static void receive_app_message(
    mosaico_network_handle_t handle,
    const mosaico_peer_message_t *message)
{
    const size_t header_size = offsetof(network_wire_message_t, payload);
    if (message->payload_len < header_size) {
        ESP_LOGW(TAG, "application frame is too short: length=%u",
                 message->payload_len);
        return;
    }
    network_wire_message_t wire = {0};
    memcpy(&wire, message->payload, message->payload_len);
    if (wire.version != MOSAICO_NETWORK_APP_VERSION ||
        (wire.flags & ~MOSAICO_NETWORK_APP_ACK) != 0 ||
        wire.ttl == 0 || wire.ttl > MOSAICO_NETWORK_APP_TTL ||
        wire.payload_len > MOSAICO_NETWORK_APP_PAYLOAD_SIZE ||
        message->payload_len != header_size + wire.payload_len ||
        wire.service_id == 0 || wire.message_id == 0 ||
        wire.topology_id == 0 || wire.message_boot_id == 0 ||
        wire.origin_id == 0 ||
        ((wire.flags & MOSAICO_NETWORK_APP_ACK) != 0 && wire.payload_len != 0)) {
        ESP_LOGW(TAG, "rejected invalid application frame");
        return;
    }

    bool accepted = false;
    bool deliver = false;
    bool topology_valid = false;
    uint64_t local_id = 0;
    mosaico_network_event_t delivery_event = {0};
    bool delivery_ready = false;
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    const uint32_t topology_id = mosaico_mesh_get_topology_id(&handle->mesh);
    local_id = handle->mesh.local_id;
    topology_valid = wire.topology_id == topology_id &&
        handle->mesh.conflict_flags == 0;
    if ((wire.flags & MOSAICO_NETWORK_APP_ACK) != 0 &&
        wire.destination_id == local_id && topology_valid) {
        for (size_t i = 0; i < MOSAICO_NETWORK_PENDING_COUNT; ++i) {
            pending_message_t *pending = &handle->pending[i];
            if (pending->active &&
                pending->wire.message_id == wire.message_id &&
                pending->wire.message_boot_id == wire.message_boot_id &&
                pending->wire.destination_id == wire.origin_id &&
                pending->wire.service_id == wire.service_id) {
                delivery_event = (mosaico_network_event_t) {
                    .type = MOSAICO_NETWORK_EVENT_MESSAGE_DELIVERED,
                    .data.delivery = {
                        .destination_id = wire.origin_id,
                        .message_id = wire.message_id,
                        .service_id = wire.service_id,
                        .error = ESP_OK,
                    },
                };
                memset(pending, 0, sizeof(*pending));
                delivery_ready = true;
                break;
            }
        }
    } else if (topology_valid) {
        accepted = remember_message_locked(
            handle, wire.origin_id, wire.message_boot_id, wire.message_id);
        deliver =
            (wire.destination_id == 0 || wire.destination_id == local_id);
    }
    xSemaphoreGive(handle->lock);
    if (delivery_ready) {
        queue_event(handle, &delivery_event);
        return;
    }
    if ((wire.flags & MOSAICO_NETWORK_APP_ACK) != 0) {
        if (topology_valid && wire.destination_id != local_id && wire.ttl > 1) {
            wire.ttl--;
            (void)route_wire_message(handle, &wire, message->source_id);
        }
        return;
    }
    if (topology_valid && wire.destination_id == local_id) {
        network_wire_message_t ack = {
            .version = MOSAICO_NETWORK_APP_VERSION,
            .flags = MOSAICO_NETWORK_APP_ACK,
            .ttl = MOSAICO_NETWORK_APP_TTL,
            .service_id = wire.service_id,
            .message_id = wire.message_id,
            .topology_id = wire.topology_id,
            .message_boot_id = wire.message_boot_id,
            .origin_id = local_id,
            .destination_id = wire.origin_id,
        };
        const esp_err_t ack_ret = route_wire_message(handle, &ack, 0);
        if (ack_ret != ESP_OK) {
            ESP_LOGW(TAG, "application acknowledgement failed: destination=%012" PRIx64
                     " error=%s", ack.destination_id, esp_err_to_name(ack_ret));
        }
    }
    if (!accepted) {
        return;
    }
    if (wire.ttl > 1 && wire.destination_id != local_id) {
        wire.ttl--;
        const esp_err_t ret = route_wire_message(
            handle, &wire, message->source_id);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "application forward failed: destination=%012" PRIx64
                     " error=%s", wire.destination_id, esp_err_to_name(ret));
        }
    }
    if (deliver) {
        mosaico_network_event_t event = {
            .type = MOSAICO_NETWORK_EVENT_MESSAGE_RECEIVED,
            .data.message = {
                .source_id = wire.origin_id,
                .destination_id = wire.destination_id,
                .service_id = wire.service_id,
                .payload_len = wire.payload_len,
            },
        };
        memcpy(event.data.message.payload, wire.payload, wire.payload_len);
        queue_event(handle, &event);
    }
}

static void receive_peer_message(
    const uint8_t source_mac[6],
    int8_t rssi,
    const mosaico_peer_message_t *message,
    void *user_ctx)
{
    (void)source_mac;
    mosaico_network_handle_t handle = user_ctx;
    if (!handle->running || message->source_id == handle->mesh.local_id) {
        return;
    }
    if (message->type == MOSAICO_PEER_MSG_DIAGNOSTIC ||
        message->type == MOSAICO_PEER_MSG_GAME_EVENT) {
        if (handle->config.raw_message_cb) {
            handle->config.raw_message_cb(
                message, rssi, handle->config.user_ctx);
        }
        return;
    }
    if (message->type == MOSAICO_PEER_MSG_TOPOLOGY_SYNC ||
        message->type == MOSAICO_PEER_MSG_APP_DATA) {
        network_neighbor_t neighbor = {0};
        xSemaphoreTake(handle->lock, portMAX_DELAY);
        const bool direct = find_neighbor_locked(
            handle, message->source_id, &neighbor);
        xSemaphoreGive(handle->lock);
        if (!direct || message->target_id != handle->mesh.local_id ||
            !direct_message_matches(&neighbor, message)) {
            ESP_LOGW(TAG, "rejected frame from non-neighbor: source=%012" PRIx64,
                     message->source_id);
            return;
        }
    }
    if (message->type == MOSAICO_PEER_MSG_TOPOLOGY_SYNC) {
        if (message->payload_len != sizeof(mosaico_mesh_wire_link_t)) {
            return;
        }
        mosaico_mesh_wire_link_t wire = {0};
        mosaico_mesh_wire_link_t forward = {0};
        memcpy(&wire, message->payload, sizeof(wire));
        bool changed = false;
        xSemaphoreTake(handle->lock, portMAX_DELAY);
        const esp_err_t ret = mosaico_mesh_receive(
            &handle->mesh, &wire, network_now_ms(), &forward, &changed);
        const uint8_t conflicts = handle->mesh.conflict_flags;
        xSemaphoreGive(handle->lock);
        if (ret == ESP_OK) {
            if (changed) {
                mosaico_network_event_t event = {
                    .type = conflicts ? MOSAICO_NETWORK_EVENT_TOPOLOGY_CONFLICT :
                        MOSAICO_NETWORK_EVENT_TOPOLOGY_CHANGED,
                };
                queue_event(handle, &event);
            }
            flood_mesh_link(handle, &forward, message->source_id);
        }
        return;
    }
    if (message->type == MOSAICO_PEER_MSG_APP_DATA) {
        receive_app_message(handle, message);
        return;
    }
    xSemaphoreTake(handle->session_lock, portMAX_DELAY);
    const esp_err_t ret = mosaico_peer_session_receive(
        &handle->sessions, message, network_now_ms());
    xSemaphoreGive(handle->session_lock);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "session frame rejected: source=%012" PRIx64 " error=%s",
                 message->source_id, esp_err_to_name(ret));
    }
}

static void session_event_callback(
    const mosaico_peer_session_event_t *event,
    void *user_ctx)
{
    mosaico_network_handle_t handle = user_ctx;
    if (event->type == MOSAICO_PEER_SESSION_EVENT_SEND) {
        const esp_err_t ret = mosaico_peer_link_send_broadcast_to(
            event->message_type, event->target_id, event->session_id,
            event->local_edge, event->peer_edge, event->relative_rotation,
            NULL, 0);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "session send failed: type=%u error=%s",
                     event->message_type, esp_err_to_name(ret));
        }
        return;
    }

    const int index = edge_index(event->local_edge);
    if (index < 0) {
        return;
    }
    mosaico_mesh_wire_link_t wire = {0};
    bool changed = false;
    uint8_t conflicts = 0;
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    const uint32_t previous_topology_id =
        mosaico_mesh_get_topology_id(&handle->mesh);
    esp_err_t ret = ESP_OK;
    if (event->type == MOSAICO_PEER_SESSION_EVENT_ATTACHED) {
        ret = mosaico_mesh_attach(
            &handle->mesh, event->local_edge, event->peer_id,
            event->peer_edge, network_now_ms(), &wire);
        if (ret == ESP_OK) {
            handle->neighbors[index] = (network_neighbor_t) {
                .active = true,
                .peer_id = event->peer_id,
                .session_id = event->session_id,
                .local_edge = event->local_edge,
                .peer_edge = event->peer_edge,
                .relative_rotation = event->relative_rotation,
            };
        }
    } else {
        ret = mosaico_mesh_detach(
            &handle->mesh, event->local_edge, event->peer_id,
            network_now_ms(), &wire);
        if (ret == ESP_OK || ret == ESP_ERR_NOT_FOUND) {
            memset(&handle->neighbors[index], 0, sizeof(handle->neighbors[index]));
        }
    }
    const uint32_t topology_id = mosaico_mesh_get_topology_id(&handle->mesh);
    changed = topology_id != previous_topology_id;
    conflicts = handle->mesh.conflict_flags;
    xSemaphoreGive(handle->lock);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "neighbor topology update failed: peer=%012" PRIx64
                 " edge=%s error=%s", event->peer_id,
                 mosaico_edge_to_string(event->local_edge), esp_err_to_name(ret));
        return;
    }
    if (ret == ESP_OK) {
        flood_mesh_link(handle, &wire, 0);
    }

    mosaico_network_event_t network_event = {
        .type = event->type == MOSAICO_PEER_SESSION_EVENT_ATTACHED ?
            MOSAICO_NETWORK_EVENT_NEIGHBOR_ATTACHED :
            MOSAICO_NETWORK_EVENT_NEIGHBOR_DETACHED,
        .data.neighbor = {
            .peer_id = event->peer_id,
            .session_id = event->session_id,
            .local_edge = event->local_edge,
            .peer_edge = event->peer_edge,
            .relative_rotation = event->relative_rotation,
        },
    };
    queue_event(handle, &network_event);
    if (changed) {
        network_event = (mosaico_network_event_t) {
            .type = conflicts ? MOSAICO_NETWORK_EVENT_TOPOLOGY_CONFLICT :
                MOSAICO_NETWORK_EVENT_TOPOLOGY_CHANGED,
        };
        queue_event(handle, &network_event);
    }
}

static void network_task(void *arg)
{
    mosaico_network_handle_t handle = arg;
    TickType_t last_wake = xTaskGetTickCount();
    while (handle->running) {
        const uint32_t now_ms = network_now_ms();
        xSemaphoreTake(handle->session_lock, portMAX_DELAY);
        mosaico_peer_session_tick(&handle->sessions, now_ms);
        xSemaphoreGive(handle->session_lock);

        if ((uint32_t)(now_ms - handle->last_topology_sync_ms) >=
            handle->config.topology_sync_interval_ms) {
            mosaico_mesh_wire_link_t wires[MOSAICO_MESH_EDGE_COUNT] = {0};
            size_t wire_count = 0;
            bool expired = false;
            xSemaphoreTake(handle->lock, portMAX_DELAY);
            for (size_t edge = 0; edge < MOSAICO_MESH_EDGE_COUNT; ++edge) {
                if (mosaico_mesh_refresh(
                        &handle->mesh,
                        (mosaico_edge_t)(edge + MOSAICO_EDGE_TOP), now_ms,
                        &wires[wire_count]) == ESP_OK) {
                    wire_count++;
                }
            }
            expired = mosaico_mesh_expire(
                &handle->mesh, now_ms, MOSAICO_MESH_RECORD_STALE_MS);
            handle->last_topology_sync_ms = now_ms;
            xSemaphoreGive(handle->lock);
            for (size_t i = 0; i < wire_count; ++i) {
                flood_mesh_link(handle, &wires[i], 0);
            }
            if (expired) {
                mosaico_network_event_t event = {
                    .type = MOSAICO_NETWORK_EVENT_TOPOLOGY_CHANGED,
                };
                queue_event(handle, &event);
            }
        }

        network_wire_message_t retries[MOSAICO_NETWORK_PENDING_COUNT] = {0};
        mosaico_network_event_t failures[MOSAICO_NETWORK_PENDING_COUNT] = {0};
        size_t retry_count = 0;
        size_t failure_count = 0;
        xSemaphoreTake(handle->lock, portMAX_DELAY);
        const uint32_t topology_id = mosaico_mesh_get_topology_id(&handle->mesh);
        for (size_t i = 0; i < MOSAICO_NETWORK_PENDING_COUNT; ++i) {
            pending_message_t *pending = &handle->pending[i];
            if (!pending->active ||
                (uint32_t)(now_ms - pending->last_tx_ms) <
                    handle->config.app_retry_interval_ms) {
                continue;
            }
            if (pending->wire.topology_id != topology_id ||
                pending->retries >= handle->config.app_retry_count) {
                failures[failure_count++] = (mosaico_network_event_t) {
                    .type = MOSAICO_NETWORK_EVENT_MESSAGE_FAILED,
                    .data.delivery = {
                        .destination_id = pending->wire.destination_id,
                        .message_id = pending->wire.message_id,
                        .service_id = pending->wire.service_id,
                        .error = pending->wire.topology_id != topology_id ?
                            ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT,
                    },
                };
                memset(pending, 0, sizeof(*pending));
                continue;
            }
            pending->retries++;
            pending->last_tx_ms = now_ms;
            retries[retry_count++] = pending->wire;
        }
        xSemaphoreGive(handle->lock);
        for (size_t i = 0; i < retry_count; ++i) {
            const esp_err_t ret = route_wire_message(handle, &retries[i], 0);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "application retry failed: destination=%012" PRIx64
                         " error=%s", retries[i].destination_id,
                         esp_err_to_name(ret));
            }
        }
        for (size_t i = 0; i < failure_count; ++i) {
            queue_event(handle, &failures[i]);
        }
        dispatch_events(handle);
        xTaskDelayUntil(
            &last_wake, pdMS_TO_TICKS(MOSAICO_NETWORK_TASK_PERIOD_MS));
    }
    handle->task = NULL;
    vTaskDelete(NULL);
}

esp_err_t mosaico_network_start(
    const mosaico_network_config_t *config,
    mosaico_network_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(out_handle, ESP_ERR_INVALID_ARG, TAG,
                        "output handle is null");
    const mosaico_network_config_t active = config ? *config :
        (mosaico_network_config_t)MOSAICO_NETWORK_CONFIG_DEFAULT();
    ESP_RETURN_ON_FALSE(
        active.network_id != 0 && active.channel >= 1 && active.channel <= 14 &&
        active.receive_queue_depth > 0 && active.worker_stack_size >= 2048 &&
        active.task_stack_size >= 3072 && active.topology_sync_interval_ms > 0 &&
        active.app_retry_interval_ms > 0 && active.app_retry_count > 0,
        ESP_ERR_INVALID_ARG, TAG, "invalid network configuration");

    mosaico_network_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG,
                        "allocate network context failed");
    handle->config = active;
    handle->lock = xSemaphoreCreateMutex();
    handle->session_lock = xSemaphoreCreateMutex();
    handle->event_queue = xQueueCreate(
        MOSAICO_NETWORK_EVENT_QUEUE_DEPTH, sizeof(mosaico_network_event_t));
    if (!handle->lock || !handle->session_lock || !handle->event_queue) {
        ESP_LOGE(TAG, "create network synchronization failed: error=%s",
                 esp_err_to_name(ESP_ERR_NO_MEM));
        if (handle->lock) {
            vSemaphoreDelete(handle->lock);
        }
        if (handle->session_lock) {
            vSemaphoreDelete(handle->session_lock);
        }
        if (handle->event_queue) {
            vQueueDelete(handle->event_queue);
        }
        free(handle);
        return ESP_ERR_NO_MEM;
    }
    const mosaico_peer_link_config_t link_config = {
        .network_id = active.network_id,
        .manage_wifi = active.manage_wifi,
        .channel = active.channel,
        .receive_queue_depth = active.receive_queue_depth,
        .worker_stack_size = active.worker_stack_size,
        .worker_priority = active.worker_priority,
    };
    esp_err_t ret = mosaico_peer_link_init(
        &link_config, receive_peer_message, handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start peer transport failed: error=%s",
                 esp_err_to_name(ret));
        goto fail;
    }
    const uint64_t device_id = mosaico_peer_link_get_device_id();
    handle->boot_id = mosaico_peer_link_get_boot_id();
    ret = mosaico_mesh_init_with_boot_id(
        &handle->mesh, device_id, mosaico_peer_link_get_boot_id());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "initialize topology failed: error=%s",
                 esp_err_to_name(ret));
        goto fail_transport;
    }
    ret = mosaico_peer_session_init(
        &handle->sessions, device_id, &active.session,
        session_event_callback, handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "initialize sessions failed: error=%s",
                 esp_err_to_name(ret));
        goto fail_transport;
    }
    handle->running = true;
    handle->last_topology_sync_ms = network_now_ms() -
        (handle->boot_id % active.topology_sync_interval_ms);
    if (xTaskCreate(
            network_task, "mosaico_network", active.task_stack_size, handle,
            active.task_priority, &handle->task) != pdPASS) {
        ret = ESP_ERR_NO_MEM;
        handle->running = false;
        ESP_LOGE(TAG, "create network task failed: error=%s",
                 esp_err_to_name(ret));
        goto fail_transport;
    }
    *out_handle = handle;
    ESP_LOGI(TAG, "started: device=%012" PRIx64 " max_nodes=%u",
             device_id, MOSAICO_MESH_MAX_NODES);
    return ESP_OK;

fail_transport:
    (void)mosaico_peer_link_deinit();
fail:
    vQueueDelete(handle->event_queue);
    vSemaphoreDelete(handle->session_lock);
    vSemaphoreDelete(handle->lock);
    free(handle);
    return ret;
}

esp_err_t mosaico_network_stop(mosaico_network_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG,
                        "network handle is null");
    handle->running = false;
    while (handle->task) {
        vTaskDelay(1);
    }
    const esp_err_t ret = mosaico_peer_link_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "stop peer transport failed: error=%s",
                 esp_err_to_name(ret));
        return ret;
    }
    vSemaphoreDelete(handle->session_lock);
    vSemaphoreDelete(handle->lock);
    vQueueDelete(handle->event_queue);
    free(handle);
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

esp_err_t mosaico_network_set_contact(
    mosaico_network_handle_t handle,
    mosaico_edge_t local_edge,
    bool present,
    uint32_t timestamp_ms)
{
    ESP_RETURN_ON_FALSE(handle && handle->running, ESP_ERR_INVALID_STATE, TAG,
                        "network is not running");
    const uint32_t now_ms = timestamp_ms ? timestamp_ms : network_now_ms();
    xSemaphoreTake(handle->session_lock, portMAX_DELAY);
    const esp_err_t ret = present ? mosaico_peer_session_local_contact(
        &handle->sessions, local_edge, 0, now_ms) :
        mosaico_peer_session_local_release(
            &handle->sessions, local_edge, now_ms);
    xSemaphoreGive(handle->session_lock);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "contact update failed: edge=%s present=%d error=%s",
                 mosaico_edge_to_string(local_edge), present,
                 esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t send_application_message(
    mosaico_network_handle_t handle,
    uint64_t destination_id,
    uint16_t service_id,
    const void *payload,
    size_t payload_len)
{
    ESP_RETURN_ON_FALSE(handle && handle->running, ESP_ERR_INVALID_STATE, TAG,
                        "network is not running");
    ESP_RETURN_ON_FALSE(service_id != 0 &&
                        payload_len <= MOSAICO_NETWORK_APP_PAYLOAD_SIZE &&
                        (!payload_len || payload),
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid application message");
    network_wire_message_t wire = {
        .version = MOSAICO_NETWORK_APP_VERSION,
        .ttl = MOSAICO_NETWORK_APP_TTL,
        .payload_len = (uint8_t)payload_len,
        .service_id = service_id,
        .destination_id = destination_id,
    };
    if (payload_len) {
        memcpy(wire.payload, payload, payload_len);
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    if (handle->mesh.conflict_flags != 0) {
        xSemaphoreGive(handle->lock);
        return ESP_ERR_INVALID_STATE;
    }
    wire.origin_id = handle->mesh.local_id;
    wire.message_boot_id = handle->boot_id;
    wire.topology_id = mosaico_mesh_get_topology_id(&handle->mesh);
    wire.message_id = ++handle->next_message_id;
    if (wire.message_id == 0) {
        wire.message_id = ++handle->next_message_id;
    }
    (void)remember_message_locked(
        handle, wire.origin_id, wire.message_boot_id, wire.message_id);
    pending_message_t *pending = NULL;
    if (destination_id != 0) {
        for (size_t i = 0; i < MOSAICO_NETWORK_PENDING_COUNT; ++i) {
            if (!handle->pending[i].active) {
                pending = &handle->pending[i];
                break;
            }
        }
        if (!pending) {
            xSemaphoreGive(handle->lock);
            return ESP_ERR_NO_MEM;
        }
        *pending = (pending_message_t) {
            .active = true,
            .last_tx_ms = network_now_ms(),
            .wire = wire,
        };
    }
    xSemaphoreGive(handle->lock);
    const esp_err_t ret = route_wire_message(handle, &wire, 0);
    if (ret != ESP_OK && destination_id != 0) {
        xSemaphoreTake(handle->lock, portMAX_DELAY);
        for (size_t i = 0; i < MOSAICO_NETWORK_PENDING_COUNT; ++i) {
            if (handle->pending[i].active &&
                handle->pending[i].wire.message_id == wire.message_id) {
                memset(&handle->pending[i], 0, sizeof(handle->pending[i]));
                break;
            }
        }
        xSemaphoreGive(handle->lock);
    }
    return ret;
}

esp_err_t mosaico_network_send(
    mosaico_network_handle_t handle,
    uint64_t destination_id,
    uint16_t service_id,
    const void *payload,
    size_t payload_len)
{
    ESP_RETURN_ON_FALSE(destination_id != 0, ESP_ERR_INVALID_ARG, TAG,
                        "destination is zero");
    return send_application_message(
        handle, destination_id, service_id, payload, payload_len);
}

esp_err_t mosaico_network_broadcast(
    mosaico_network_handle_t handle,
    uint16_t service_id,
    const void *payload,
    size_t payload_len)
{
    return send_application_message(handle, 0, service_id, payload, payload_len);
}

esp_err_t mosaico_network_get_snapshot(
    mosaico_network_handle_t handle,
    mosaico_network_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(handle && snapshot, ESP_ERR_INVALID_ARG, TAG,
                        "invalid snapshot arguments");
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    fill_snapshot_locked(handle, snapshot);
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}

size_t mosaico_network_get_nodes(
    mosaico_network_handle_t handle,
    mosaico_network_node_t *nodes,
    size_t capacity)
{
    if (!handle || !nodes || capacity == 0) {
        return 0;
    }
    size_t count = 0;
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    for (size_t i = 0; i < handle->mesh.node_count && count < capacity; ++i) {
        const mosaico_mesh_node_t *node = &handle->mesh.nodes[i];
        if (node->pose_valid) {
            const mosaico_network_node_t value = {
                .device_id = node->device_id,
                .x = node->x,
                .y = node->y,
                .rotation_degrees = node->rotation_degrees,
            };
            size_t insert = count;
            while (insert > 0 && nodes[insert - 1].device_id > value.device_id) {
                nodes[insert] = nodes[insert - 1];
                insert--;
            }
            nodes[insert] = value;
            count++;
        }
    }
    xSemaphoreGive(handle->lock);
    return count;
}
