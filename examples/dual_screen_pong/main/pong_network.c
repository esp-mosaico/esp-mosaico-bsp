/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include "pong_network.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "pong_docking.h"

static const char *TAG = "pong_network";

#define PONG_NETWORK_ID                 0x504F4E47U
#define PONG_NETWORK_CHANNEL            1U
#define PONG_NETWORK_RX_DEPTH           32U
#define PONG_NETWORK_EVENT_DEPTH        16U
#define PONG_HELLO_PERIOD_MS            500U
#define PONG_PING_PERIOD_MS             500U
#define PONG_CANDIDATE_TIMEOUT_MS       2000U
#define PONG_PROTOCOL_KIND_COUNT        ((unsigned)PONG_MSG_LAYOUT + 1U)

typedef struct {
    int8_t rssi;
    mosaico_peer_message_t message;
} pong_rx_item_t;

typedef struct {
    bool started;
    bool paired;
    bool paused;
    bool local_ready;
    bool peer_ready;
    uint64_t device_id;
    uint32_t boot_id;
    uint64_t candidate_id;
    uint32_t candidate_boot_id;
    uint32_t candidate_last_ms;
    uint64_t peer_id;
    uint32_t peer_boot_id;
    uint32_t session_id;
    uint32_t tx_sequence;
    uint32_t rx_sequence[PONG_PROTOCOL_KIND_COUNT];
    uint16_t rx_sequence_valid;
    uint32_t last_hello_ms;
    uint32_t last_ping_ms;
    uint32_t last_input_ms;
    uint32_t last_snapshot_ms;
    uint32_t last_snapshot_tick;
    uint32_t snapshot_received;
    uint32_t snapshot_expected;
    uint32_t last_receive_ms;
    uint16_t rtt_ms;
    int8_t rssi;
    QueueHandle_t rx_queue;
    QueueHandle_t event_queue;
    SemaphoreHandle_t state_lock;
    SemaphoreHandle_t send_lock;
} pong_network_context_t;

static pong_network_context_t s_network;

static uint32_t network_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool elapsed_at_least(uint32_t now_ms, uint32_t then_ms, uint32_t period_ms)
{
    return (uint32_t)(now_ms - then_ms) >= period_ms;
}

static int16_t quantize(float value, float scale)
{
    const float scaled = value * scale;
    if (scaled >= (float)INT16_MAX) {
        return INT16_MAX;
    }
    if (scaled <= (float)INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)scaled;
}

static float dequantize(int16_t value, float scale)
{
    return (float)value / scale;
}

static void device_id_to_bytes(uint64_t device_id, uint8_t bytes[6])
{
    for (int i = 5; i >= 0; --i) {
        bytes[i] = (uint8_t)device_id;
        device_id >>= 8;
    }
}

static uint32_t make_session_id(uint64_t local_id, uint32_t local_boot,
                                uint64_t peer_id, uint32_t peer_boot)
{
    const uint64_t low_id = local_id < peer_id ? local_id : peer_id;
    const uint64_t high_id = local_id < peer_id ? peer_id : local_id;
    const uint32_t low_boot = local_id < peer_id ? local_boot : peer_boot;
    const uint32_t high_boot = local_id < peer_id ? peer_boot : local_boot;
    uint32_t value = (uint32_t)low_id ^ (uint32_t)(low_id >> 32) ^
                     (uint32_t)high_id ^ (uint32_t)(high_id >> 32) ^
                     low_boot ^ (high_boot * 0x9e3779b9U);
    return value != 0 ? value : 1U;
}

static void post_event(const pong_network_event_t *event)
{
    if (xQueueSend(s_network.event_queue, event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full: kind=%u", (unsigned)event->kind);
    }
}

static void peer_receive_callback(const uint8_t source_mac[6], int8_t rssi,
                                  const mosaico_peer_message_t *message,
                                  void *user_ctx)
{
    (void)source_mac;
    (void)user_ctx;
    const pong_rx_item_t item = {
        .rssi = rssi,
        .message = *message,
    };
    (void)xQueueSend(s_network.rx_queue, &item, 0);
}

static esp_err_t send_wire_message(uint64_t target_id,
                                   mosaico_peer_message_type_t peer_type,
                                   const pong_message_t *message)
{
    uint8_t wire[PONG_PROTOCOL_MAX_WIRE_SIZE];
    size_t wire_len = 0;
    ESP_RETURN_ON_FALSE(pong_protocol_encode(message, wire, sizeof(wire), &wire_len),
                        ESP_ERR_INVALID_ARG, TAG, "encode Pong frame failed");
    xSemaphoreTake(s_network.send_lock, portMAX_DELAY);
    const esp_err_t ret = target_id == 0 ?
        mosaico_peer_link_send_broadcast(peer_type, message->session,
                                         MOSAICO_EDGE_NONE, MOSAICO_EDGE_NONE,
                                         0, wire, wire_len) :
        mosaico_peer_link_send_broadcast_to(peer_type, target_id, message->session,
                                            MOSAICO_EDGE_NONE, MOSAICO_EDGE_NONE,
                                            0, wire, wire_len);
    xSemaphoreGive(s_network.send_lock);
    return ret;
}

static esp_err_t send_protocol_message(pong_message_kind_t kind,
                                       const pong_message_payload_t *payload,
                                       uint64_t target_id, uint32_t session_id,
                                       uint32_t tick)
{
    pong_message_t message = {
        .magic = PONG_PROTOCOL_MAGIC,
        .version = PONG_PROTOCOL_VERSION,
        .kind = kind,
        .payload_len = pong_protocol_payload_size(kind),
        .session = session_id,
        .sequence = ++s_network.tx_sequence,
        .tick = tick,
    };
    if (payload) {
        message.payload = *payload;
    }
    return send_wire_message(target_id,
                             kind == PONG_MSG_HELLO ? MOSAICO_PEER_MSG_HELLO :
                             MOSAICO_PEER_MSG_GAME_EVENT,
                             &message);
}

static esp_err_t send_hello(uint32_t now_ms)
{
    pong_message_payload_t payload = {0};
    device_id_to_bytes(s_network.device_id, payload.hello.device_id);
    payload.hello.capabilities = 1U;
    payload.hello.nonce = s_network.boot_id;
    payload.hello.preferred_role =
        s_network.candidate_id != 0 && s_network.device_id > s_network.candidate_id ?
        PONG_ROLE_RIGHT : PONG_ROLE_LEFT;
    return send_protocol_message(PONG_MSG_HELLO, &payload, 0, 0, now_ms);
}

static esp_err_t send_pair(uint32_t now_ms)
{
    pong_message_payload_t payload = {0};
    payload.pair.role = s_network.device_id < s_network.candidate_id ?
                        PONG_ROLE_LEFT : PONG_ROLE_RIGHT;
    payload.pair.dock_state = PONG_DOCK_WIRELESS;
    payload.pair.nonce = s_network.session_id;
    return send_protocol_message(PONG_MSG_PAIR, &payload, s_network.candidate_id,
                                 s_network.session_id, now_ms);
}

static esp_err_t send_ready(uint32_t now_ms)
{
    pong_message_payload_t payload = {0};
    payload.ready.role = s_network.device_id < s_network.peer_id ?
                         PONG_ROLE_LEFT : PONG_ROLE_RIGHT;
    payload.ready.ready = s_network.local_ready;
    return send_protocol_message(PONG_MSG_READY, &payload, s_network.peer_id,
                                 s_network.session_id, now_ms);
}

static esp_err_t send_ping(uint32_t now_ms, uint32_t echoed_timestamp_ms)
{
    pong_message_payload_t payload = {0};
    payload.ping.timestamp_ms = now_ms;
    payload.ping.echoed_timestamp_ms = echoed_timestamp_ms;
    return send_protocol_message(PONG_MSG_PING, &payload, s_network.peer_id,
                                 s_network.session_id, now_ms);
}

static void clear_pairing(void)
{
    s_network.paired = false;
    s_network.paused = false;
    s_network.peer_ready = false;
    s_network.peer_id = 0;
    s_network.peer_boot_id = 0;
    s_network.session_id = 0;
    s_network.last_snapshot_tick = 0;
    s_network.snapshot_received = 0;
    s_network.snapshot_expected = 0;
    s_network.rx_sequence_valid = 0;
    memset(s_network.rx_sequence, 0, sizeof(s_network.rx_sequence));
}

static void enter_paired(uint64_t peer_id, uint32_t peer_boot_id, uint32_t now_ms)
{
    s_network.paired = true;
    s_network.paused = false;
    s_network.peer_id = peer_id;
    s_network.peer_boot_id = peer_boot_id;
    s_network.last_receive_ms = now_ms;
    s_network.rx_sequence_valid = 0;
    const pong_network_event_t event = {
        .kind = PONG_NETWORK_EVENT_PAIRED,
        .peer_id = peer_id,
        .session_id = s_network.session_id,
    };
    post_event(&event);
    ESP_LOGI(TAG, "paired: peer=%012" PRIx64 " session=%08" PRIx32
             " role=%s host=%d", peer_id, s_network.session_id,
             s_network.device_id < peer_id ? "LEFT" : "RIGHT",
             s_network.device_id < peer_id);
    (void)send_ready(now_ms);
}

static bool frame_sequence_is_fresh(const pong_message_t *message)
{
    const unsigned kind = message->kind;
    if (kind >= PONG_PROTOCOL_KIND_COUNT) {
        return false;
    }
    const uint16_t bit = (uint16_t)(1U << kind);
    if ((s_network.rx_sequence_valid & bit) != 0 &&
        !pong_protocol_sequence_is_newer(message->sequence,
                                         s_network.rx_sequence[kind])) {
        return false;
    }
    s_network.rx_sequence[kind] = message->sequence;
    s_network.rx_sequence_valid |= bit;
    return true;
}

static void decode_input(const pong_message_t *message, pong_input_t *input)
{
    input->axis_x = dequantize(message->payload.input.axis_x_q15, 32767.0f);
    input->axis_y = dequantize(message->payload.input.axis_y_q15, 32767.0f);
    input->buttons = message->payload.input.buttons;
    input->sequence = message->sequence;
    input->sampled_ms = message->payload.input.sampled_ms;
}

static void decode_snapshot(const pong_message_t *message, pong_world_t *world)
{
    const pong_snapshot_payload_t *wire = &message->payload.snapshot;
    memset(world, 0, sizeof(*world));
    world->tick = message->tick;
    world->event_id = wire->event_id;
    world->event = (pong_event_kind_t)wire->event;
    world->phase = (pong_phase_t)wire->phase;
    world->ball.position.x = dequantize(wire->ball_x_q4, 16.0f);
    world->ball.position.y = dequantize(wire->ball_y_q4, 16.0f);
    world->ball.velocity.x = dequantize(wire->ball_vx_q2, 4.0f);
    world->ball.velocity.y = dequantize(wire->ball_vy_q2, 4.0f);
    world->ball.spin = dequantize(wire->ball_spin_q4, 16.0f);
    for (size_t i = 0; i < 2; ++i) {
        world->paddles[i].y = dequantize(wire->paddle_y_q4[i], 16.0f);
        world->paddles[i].velocity =
            dequantize(wire->paddle_velocity_q2[i], 4.0f);
        world->paddles[i].tilt =
            dequantize(wire->paddle_tilt_q12[i], 4096.0f);
        world->score[i] = wire->score[i];
    }
    world->serving_role = wire->serving_role;
    world->countdown_ms = wire->countdown_ms;
}

static void handle_hello(const pong_rx_item_t *item,
                         const pong_message_t *message, uint32_t now_ms)
{
    const uint64_t source_id = item->message.source_id;
    if (message->session != 0 || message->payload.hello.nonce == 0 ||
        message->payload.hello.nonce != item->message.source_boot_id) {
        return;
    }
    uint8_t expected_id[6];
    device_id_to_bytes(source_id, expected_id);
    if (memcmp(expected_id, message->payload.hello.device_id,
               sizeof(expected_id)) != 0) {
        return;
    }
    if (s_network.paired && source_id == s_network.peer_id &&
        item->message.source_boot_id != s_network.peer_boot_id) {
        const pong_network_event_t event = {
            .kind = PONG_NETWORK_EVENT_PEER_REBOOT,
            .peer_id = source_id,
            .session_id = s_network.session_id,
        };
        post_event(&event);
        ESP_LOGW(TAG, "peer rebooted: peer=%012" PRIx64, source_id);
        clear_pairing();
        s_network.candidate_id = 0;
        return;
    }
    if (s_network.paired) {
        return;
    }
    if (s_network.candidate_id == 0) {
        s_network.candidate_id = source_id;
        s_network.candidate_boot_id = item->message.source_boot_id;
        ESP_LOGI(TAG, "discovered candidate: peer=%012" PRIx64, source_id);
    } else if (s_network.candidate_id != source_id) {
        return;
    } else if (s_network.candidate_boot_id != item->message.source_boot_id) {
        s_network.candidate_boot_id = item->message.source_boot_id;
    }
    s_network.candidate_last_ms = now_ms;
    s_network.rssi = item->rssi;
    s_network.session_id = make_session_id(
        s_network.device_id, s_network.boot_id,
        s_network.candidate_id, s_network.candidate_boot_id);
    /* Device B (the greater stable device ID) confirms the candidate. */
    if (s_network.device_id > s_network.candidate_id) {
        (void)send_pair(now_ms);
    }
}

static void handle_pair(const pong_rx_item_t *item,
                        const pong_message_t *message, uint32_t now_ms)
{
    const uint64_t source_id = item->message.source_id;
    if (s_network.candidate_id != source_id ||
        message->session != s_network.session_id ||
        message->payload.pair.nonce != s_network.session_id) {
        return;
    }
    const pong_role_t expected_peer_role =
        source_id < s_network.device_id ? PONG_ROLE_LEFT : PONG_ROLE_RIGHT;
    if (message->payload.pair.role != expected_peer_role) {
        return;
    }
    if (!s_network.paired) {
        enter_paired(source_id, item->message.source_boot_id, now_ms);
    }
    if (s_network.device_id < source_id) {
        (void)send_pair(now_ms);
    }
}

static void handle_paired_message(const pong_rx_item_t *item,
                                  const pong_message_t *message,
                                  uint32_t now_ms)
{
    if (!s_network.paired || item->message.source_id != s_network.peer_id ||
        item->message.source_boot_id != s_network.peer_boot_id ||
        item->message.target_id != s_network.device_id ||
        message->session != s_network.session_id ||
        item->message.session_id != s_network.session_id) {
        return;
    }
    s_network.last_receive_ms = now_ms;
    s_network.rssi = item->rssi;
    if (s_network.paused) {
        s_network.paused = false;
        const pong_network_event_t resumed = {
            .kind = PONG_NETWORK_EVENT_PEER_RESUMED,
            .peer_id = s_network.peer_id,
            .session_id = s_network.session_id,
        };
        post_event(&resumed);
    }
    if (!frame_sequence_is_fresh(message)) {
        return;
    }

    pong_network_event_t event = {
        .peer_id = s_network.peer_id,
        .session_id = s_network.session_id,
    };
    switch ((pong_message_kind_t)message->kind) {
    case PONG_MSG_READY:
        if (message->payload.ready.role ==
            (s_network.device_id < s_network.peer_id ?
             PONG_ROLE_RIGHT : PONG_ROLE_LEFT)) {
            s_network.peer_ready = message->payload.ready.ready != 0;
            event.kind = PONG_NETWORK_EVENT_READY_CHANGED;
            event.data.ready = s_network.peer_ready;
            post_event(&event);
        }
        break;
    case PONG_MSG_INPUT:
        if (s_network.device_id < s_network.peer_id &&
            message->payload.input.role == PONG_ROLE_RIGHT) {
            event.kind = PONG_NETWORK_EVENT_INPUT;
            decode_input(message, &event.data.input);
            post_event(&event);
        }
        break;
    case PONG_MSG_SNAPSHOT:
        if (s_network.device_id > s_network.peer_id) {
            if (s_network.last_snapshot_tick != 0 &&
                pong_protocol_sequence_is_newer(
                    message->tick, s_network.last_snapshot_tick)) {
                const uint32_t tick_gap =
                    message->tick - s_network.last_snapshot_tick;
                s_network.snapshot_expected +=
                    tick_gap > 5U ? (tick_gap + 2U) / 4U : 1U;
            } else {
                s_network.snapshot_expected++;
            }
            s_network.snapshot_received++;
            s_network.last_snapshot_tick = message->tick;
            event.kind = PONG_NETWORK_EVENT_SNAPSHOT;
            decode_snapshot(message, &event.data.snapshot);
            post_event(&event);
        }
        break;
    case PONG_MSG_CONTROL:
        event.kind = PONG_NETWORK_EVENT_CONTROL;
        event.data.control = message->payload.control;
        post_event(&event);
        break;
    case PONG_MSG_LAYOUT:
        if (message->payload.layout.role ==
            (s_network.device_id < s_network.peer_id ?
             PONG_ROLE_RIGHT : PONG_ROLE_LEFT) &&
            message->payload.layout.dock_state <= PONG_DOCK_REVERSED) {
            event.kind = PONG_NETWORK_EVENT_LAYOUT;
            event.data.dock_state =
                (pong_dock_state_t)message->payload.layout.dock_state;
            post_event(&event);
        }
        break;
    case PONG_MSG_PING:
        if (message->payload.ping.echoed_timestamp_ms == 0) {
            (void)send_ping(now_ms, message->payload.ping.timestamp_ms);
        } else {
            const uint32_t rtt = now_ms - message->payload.ping.echoed_timestamp_ms;
            s_network.rtt_ms = rtt > UINT16_MAX ? UINT16_MAX : (uint16_t)rtt;
        }
        break;
    default:
        break;
    }
}

static void process_rx_item(const pong_rx_item_t *item, uint32_t now_ms)
{
    const mosaico_peer_message_t *peer = &item->message;
    if (peer->source_id == 0 || peer->source_id == s_network.device_id ||
        (peer->target_id != 0 && peer->target_id != s_network.device_id)) {
        return;
    }
    if (peer->type >= MOSAICO_PEER_MSG_CONTACT_CLAIM &&
        peer->type <= MOSAICO_PEER_MSG_CONTACT_RELEASE) {
        pong_docking_receive_peer_message(peer, now_ms);
        return;
    }
    if (peer->type != MOSAICO_PEER_MSG_HELLO &&
        peer->type != MOSAICO_PEER_MSG_GAME_EVENT) {
        return;
    }
    pong_message_t message;
    if (!pong_protocol_decode(peer->payload, peer->payload_len, &message)) {
        ESP_LOGD(TAG, "discard invalid Pong frame: peer=%012" PRIx64, peer->source_id);
        return;
    }
    if (message.kind == PONG_MSG_HELLO && peer->type == MOSAICO_PEER_MSG_HELLO &&
        peer->target_id == 0) {
        handle_hello(item, &message, now_ms);
    } else if (message.kind == PONG_MSG_PAIR &&
               peer->type == MOSAICO_PEER_MSG_GAME_EVENT) {
        handle_pair(item, &message, now_ms);
    } else {
        handle_paired_message(item, &message, now_ms);
    }
}

esp_err_t pong_network_start(void)
{
    ESP_RETURN_ON_FALSE(!s_network.started, ESP_ERR_INVALID_STATE, TAG,
                        "network is already started");
    memset(&s_network, 0, sizeof(s_network));
    s_network.rx_queue = xQueueCreate(PONG_NETWORK_RX_DEPTH, sizeof(pong_rx_item_t));
    s_network.event_queue =
        xQueueCreate(PONG_NETWORK_EVENT_DEPTH, sizeof(pong_network_event_t));
    s_network.state_lock = xSemaphoreCreateMutex();
    s_network.send_lock = xSemaphoreCreateMutex();
    if (!s_network.rx_queue || !s_network.event_queue ||
        !s_network.state_lock || !s_network.send_lock) {
        (void)pong_network_stop();
        return ESP_ERR_NO_MEM;
    }

    mosaico_peer_link_config_t config = MOSAICO_PEER_LINK_CONFIG_DEFAULT();
    config.network_id = PONG_NETWORK_ID;
    config.channel = PONG_NETWORK_CHANNEL;
    config.receive_queue_depth = PONG_NETWORK_RX_DEPTH;
    const esp_err_t ret =
        mosaico_peer_link_init(&config, peer_receive_callback, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "initialize peer link failed: %s", esp_err_to_name(ret));
        (void)pong_network_stop();
        return ret;
    }
    s_network.device_id = mosaico_peer_link_get_device_id();
    s_network.boot_id = mosaico_peer_link_get_boot_id();
    s_network.rssi = INT8_MIN;
    s_network.started = true;
    const uint32_t now_ms = network_now_ms();
    s_network.last_hello_ms = now_ms - PONG_HELLO_PERIOD_MS;
    ESP_LOGI(TAG, "started: device=%012" PRIx64 " boot=%08" PRIx32,
             s_network.device_id, s_network.boot_id);
    return ESP_OK;
}

esp_err_t pong_network_stop(void)
{
    const bool link_started = s_network.started;
    s_network.started = false;
    esp_err_t ret = ESP_OK;
    if (link_started) {
        ret = mosaico_peer_link_deinit();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "deinitialize peer link failed: %s", esp_err_to_name(ret));
        }
    }
    if (s_network.event_queue) {
        vQueueDelete(s_network.event_queue);
    }
    if (s_network.rx_queue) {
        vQueueDelete(s_network.rx_queue);
    }
    if (s_network.state_lock) {
        vSemaphoreDelete(s_network.state_lock);
    }
    if (s_network.send_lock) {
        vSemaphoreDelete(s_network.send_lock);
    }
    memset(&s_network, 0, sizeof(s_network));
    return ret;
}

void pong_network_tick(uint32_t now_ms)
{
    if (!s_network.started) {
        return;
    }
    pong_rx_item_t item;
    while (xQueueReceive(s_network.rx_queue, &item, 0) == pdTRUE) {
        const bool contact_frame =
            item.message.type >= MOSAICO_PEER_MSG_CONTACT_CLAIM &&
            item.message.type <= MOSAICO_PEER_MSG_CONTACT_RELEASE;
        if (contact_frame) {
            /* Docking may query network status from its session callback. */
            process_rx_item(&item, now_ms);
        } else {
            xSemaphoreTake(s_network.state_lock, portMAX_DELAY);
            process_rx_item(&item, now_ms);
            xSemaphoreGive(s_network.state_lock);
        }
    }

    xSemaphoreTake(s_network.state_lock, portMAX_DELAY);
    if (elapsed_at_least(now_ms, s_network.last_hello_ms, PONG_HELLO_PERIOD_MS)) {
        s_network.last_hello_ms = now_ms;
        (void)send_hello(now_ms);
    }
    if (!s_network.paired && s_network.candidate_id != 0 &&
        elapsed_at_least(now_ms, s_network.candidate_last_ms,
                         PONG_CANDIDATE_TIMEOUT_MS)) {
        ESP_LOGI(TAG, "candidate expired: peer=%012" PRIx64,
                 s_network.candidate_id);
        s_network.candidate_id = 0;
        s_network.candidate_boot_id = 0;
        s_network.session_id = 0;
    }
    if (s_network.paired &&
        elapsed_at_least(now_ms, s_network.last_ping_ms, PONG_PING_PERIOD_MS)) {
        s_network.last_ping_ms = now_ms;
        (void)send_ping(now_ms, 0);
    }
    if (s_network.paired && !s_network.paused &&
        elapsed_at_least(now_ms, s_network.last_receive_ms,
                         PONG_NETWORK_PAUSE_TIMEOUT_MS)) {
        s_network.paused = true;
        const pong_network_event_t paused = {
            .kind = PONG_NETWORK_EVENT_PEER_PAUSED,
            .peer_id = s_network.peer_id,
            .session_id = s_network.session_id,
        };
        post_event(&paused);
    }
    if (s_network.paired &&
        elapsed_at_least(now_ms, s_network.last_receive_ms,
                         PONG_NETWORK_GRACE_TIMEOUT_MS)) {
        const pong_network_event_t lost = {
            .kind = PONG_NETWORK_EVENT_PEER_LOST,
            .peer_id = s_network.peer_id,
            .session_id = s_network.session_id,
        };
        post_event(&lost);
        ESP_LOGW(TAG, "peer grace expired: peer=%012" PRIx64, s_network.peer_id);
        clear_pairing();
        s_network.candidate_id = 0;
    }
    xSemaphoreGive(s_network.state_lock);
}

esp_err_t pong_network_set_ready(bool ready)
{
    ESP_RETURN_ON_FALSE(s_network.started, ESP_ERR_INVALID_STATE, TAG,
                        "network is not started");
    xSemaphoreTake(s_network.state_lock, portMAX_DELAY);
    s_network.local_ready = ready;
    const esp_err_t ret = s_network.paired ? send_ready(network_now_ms()) : ESP_OK;
    xSemaphoreGive(s_network.state_lock);
    return ret;
}

esp_err_t pong_network_send_input(const pong_input_t *input)
{
    ESP_RETURN_ON_FALSE(input, ESP_ERR_INVALID_ARG, TAG, "input is null");
    ESP_RETURN_ON_FALSE(s_network.started, ESP_ERR_INVALID_STATE, TAG,
                        "network is not started");
    const uint32_t now_ms = network_now_ms();
    xSemaphoreTake(s_network.state_lock, portMAX_DELAY);
    if (!s_network.paired || s_network.device_id < s_network.peer_id) {
        xSemaphoreGive(s_network.state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!elapsed_at_least(now_ms, s_network.last_input_ms,
                          PONG_NETWORK_INPUT_PERIOD_MS)) {
        xSemaphoreGive(s_network.state_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    pong_message_payload_t payload = {0};
    payload.input.role = PONG_ROLE_RIGHT;
    payload.input.buttons = input->buttons;
    payload.input.axis_x_q15 = quantize(input->axis_x, 32767.0f);
    payload.input.axis_y_q15 = quantize(input->axis_y, 32767.0f);
    payload.input.paddle_tilt_q12 = quantize(input->axis_x, 4096.0f);
    payload.input.sampled_ms = input->sampled_ms;
    s_network.last_input_ms = now_ms;
    const esp_err_t ret = send_protocol_message(
        PONG_MSG_INPUT, &payload, s_network.peer_id, s_network.session_id, now_ms);
    xSemaphoreGive(s_network.state_lock);
    return ret;
}

esp_err_t pong_network_send_snapshot(const pong_world_t *world)
{
    ESP_RETURN_ON_FALSE(world, ESP_ERR_INVALID_ARG, TAG, "world is null");
    ESP_RETURN_ON_FALSE(s_network.started, ESP_ERR_INVALID_STATE, TAG,
                        "network is not started");
    const uint32_t now_ms = network_now_ms();
    xSemaphoreTake(s_network.state_lock, portMAX_DELAY);
    if (!s_network.paired || s_network.device_id > s_network.peer_id) {
        xSemaphoreGive(s_network.state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!elapsed_at_least(now_ms, s_network.last_snapshot_ms,
                          PONG_NETWORK_SNAPSHOT_PERIOD_MS)) {
        xSemaphoreGive(s_network.state_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    pong_message_payload_t payload = {0};
    pong_snapshot_payload_t *wire = &payload.snapshot;
    wire->ball_x_q4 = quantize(world->ball.position.x, 16.0f);
    wire->ball_y_q4 = quantize(world->ball.position.y, 16.0f);
    wire->ball_vx_q2 = quantize(world->ball.velocity.x, 4.0f);
    wire->ball_vy_q2 = quantize(world->ball.velocity.y, 4.0f);
    wire->ball_spin_q4 = quantize(world->ball.spin, 16.0f);
    for (size_t i = 0; i < 2; ++i) {
        wire->paddle_y_q4[i] = quantize(world->paddles[i].y, 16.0f);
        wire->paddle_velocity_q2[i] =
            quantize(world->paddles[i].velocity, 4.0f);
        wire->paddle_tilt_q12[i] =
            quantize(world->paddles[i].tilt, 4096.0f);
        wire->score[i] = world->score[i];
    }
    wire->event_id = world->event_id;
    wire->countdown_ms = world->countdown_ms;
    wire->serving_role = world->serving_role;
    wire->phase = world->phase;
    wire->event = world->event;
    s_network.last_snapshot_ms = now_ms;
    const esp_err_t ret = send_protocol_message(
        PONG_MSG_SNAPSHOT, &payload, s_network.peer_id,
        s_network.session_id, world->tick);
    xSemaphoreGive(s_network.state_lock);
    return ret;
}

esp_err_t pong_network_send_control(pong_control_action_t action,
                                    uint32_t argument)
{
    ESP_RETURN_ON_FALSE(action >= PONG_CONTROL_PAUSE &&
                        action <= PONG_CONTROL_EMOTE,
                        ESP_ERR_INVALID_ARG, TAG, "invalid control action");
    ESP_RETURN_ON_FALSE(s_network.started, ESP_ERR_INVALID_STATE, TAG,
                        "network is not started");
    xSemaphoreTake(s_network.state_lock, portMAX_DELAY);
    if (!s_network.paired) {
        xSemaphoreGive(s_network.state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    pong_message_payload_t payload = {0};
    payload.control.action = action;
    payload.control.argument = argument;
    const esp_err_t ret = send_protocol_message(
        PONG_MSG_CONTROL, &payload, s_network.peer_id,
        s_network.session_id, network_now_ms());
    xSemaphoreGive(s_network.state_lock);
    return ret;
}

esp_err_t pong_network_send_layout(pong_dock_state_t state)
{
    ESP_RETURN_ON_FALSE(state >= PONG_DOCK_UNAVAILABLE &&
                        state <= PONG_DOCK_REVERSED,
                        ESP_ERR_INVALID_ARG, TAG, "invalid layout state");
    ESP_RETURN_ON_FALSE(s_network.started, ESP_ERR_INVALID_STATE, TAG,
                        "network is not started");
    xSemaphoreTake(s_network.state_lock, portMAX_DELAY);
    if (!s_network.paired) {
        xSemaphoreGive(s_network.state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const pong_role_t role = s_network.device_id < s_network.peer_id ?
                             PONG_ROLE_LEFT : PONG_ROLE_RIGHT;
    pong_message_payload_t payload = {0};
    payload.layout.role = (uint8_t)role;
    payload.layout.dock_state = (uint8_t)state;
    payload.layout.origin_x = role == PONG_ROLE_LEFT ?
                              0 : (int16_t)PONG_VIEWPORT_WIDTH;
    const esp_err_t ret = send_protocol_message(
        PONG_MSG_LAYOUT, &payload, s_network.peer_id,
        s_network.session_id, network_now_ms());
    xSemaphoreGive(s_network.state_lock);
    return ret;
}

bool pong_network_poll_event(pong_network_event_t *event)
{
    return event && s_network.event_queue &&
           xQueueReceive(s_network.event_queue, event, 0) == pdTRUE;
}

esp_err_t pong_network_get_status(pong_network_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is null");
    ESP_RETURN_ON_FALSE(s_network.started, ESP_ERR_INVALID_STATE, TAG,
                        "network is not started");
    xSemaphoreTake(s_network.state_lock, portMAX_DELAY);
    *status = (pong_network_status_t) {
        .started = s_network.started,
        .paired = s_network.paired,
        .paused = s_network.paused,
        .local_ready = s_network.local_ready,
        .peer_ready = s_network.peer_ready,
        .is_host = s_network.paired && s_network.device_id < s_network.peer_id,
        .local_role = s_network.paired && s_network.device_id > s_network.peer_id ?
                      PONG_ROLE_RIGHT : PONG_ROLE_LEFT,
        .device_id = s_network.device_id,
        .boot_id = s_network.boot_id,
        .peer_id = s_network.peer_id,
        .peer_boot_id = s_network.peer_boot_id,
        .session_id = s_network.session_id,
        .rtt_ms = s_network.rtt_ms,
        .rssi = s_network.rssi,
        .packet_loss_percent =
            s_network.snapshot_expected > s_network.snapshot_received ?
            (uint8_t)(((s_network.snapshot_expected -
                        s_network.snapshot_received) * 100U) /
                      s_network.snapshot_expected) : 0,
        .last_receive_ms = s_network.last_receive_ms,
    };
    xSemaphoreGive(s_network.state_lock);
    return ESP_OK;
}

esp_err_t pong_network_send_peer_message(
    mosaico_peer_message_type_t type,
    uint64_t target_id,
    uint32_t session_id,
    mosaico_edge_t local_edge,
    mosaico_edge_t peer_edge,
    uint16_t relative_rotation)
{
    ESP_RETURN_ON_FALSE(s_network.started && s_network.send_lock,
                        ESP_ERR_INVALID_STATE, TAG, "network is not started");
    ESP_RETURN_ON_FALSE(type >= MOSAICO_PEER_MSG_CONTACT_CLAIM &&
                        type <= MOSAICO_PEER_MSG_CONTACT_RELEASE &&
                        session_id != 0,
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid docking session frame");
    xSemaphoreTake(s_network.send_lock, portMAX_DELAY);
    const esp_err_t ret = mosaico_peer_link_send_broadcast_to(
        type, target_id, session_id, local_edge, peer_edge,
        relative_rotation, NULL, 0);
    xSemaphoreGive(s_network.send_lock);
    return ret;
}
