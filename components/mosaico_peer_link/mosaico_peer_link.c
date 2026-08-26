/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_peer_link.h"

#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "mosaico_peer";

#define MOSAICO_PEER_MAGIC   0x4D4F5341U
#define MOSAICO_PEER_VERSION 5U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t network_id;
    uint8_t version;
    uint8_t type;
    uint8_t local_edge;
    uint8_t peer_edge;
    uint64_t source_id;
    uint32_t source_boot_id;
    uint64_t target_id;
    uint32_t session_id;
    uint32_t sequence;
    uint16_t relative_rotation;
    uint8_t payload_len;
    uint8_t payload[MOSAICO_PEER_PAYLOAD_SIZE];
} peer_wire_message_t;

typedef struct {
    uint8_t source_mac[6];
    int8_t rssi;
    mosaico_peer_message_t message;
} peer_rx_item_t;

typedef struct {
    QueueHandle_t receive_queue;
    QueueHandle_t diagnostic_queue;
    SemaphoreHandle_t send_lock;
    TaskHandle_t worker_task;
    mosaico_peer_receive_cb_t receive_cb;
    void *user_ctx;
    uint64_t device_id;
    uint32_t boot_id;
    uint32_t network_id;
    uint32_t sequence;
    bool running;
    bool wifi_owned;
    bool netif_owned;
    esp_netif_t *sta_netif;
} peer_link_context_t;

static peer_link_context_t s_link;
static const uint8_t s_broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static uint64_t device_id_from_mac(const uint8_t mac[6])
{
    uint64_t id = 0;
    for (int i = 0; i < 6; ++i) {
        id = (id << 8) | mac[i];
    }
    return id;
}

static void mac_from_device_id(uint64_t device_id, uint8_t mac[6])
{
    for (int i = 5; i >= 0; --i) {
        mac[i] = (uint8_t)device_id;
        device_id >>= 8;
    }
}

static esp_err_t ensure_unicast_peer(uint64_t device_id, uint8_t mac[6])
{
    mac_from_device_id(device_id, mac);
    if (esp_now_is_peer_exist(mac)) {
        return ESP_OK;
    }
    esp_now_peer_info_t peer = {
        .channel = 0,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, mac, sizeof(peer.peer_addr));
    const esp_err_t ret = esp_now_add_peer(&peer);
    return ret == ESP_ERR_ESPNOW_EXIST ? ESP_OK : ret;
}

static bool decode_message(
    const uint8_t *data,
    int data_len,
    mosaico_peer_message_t *message)
{
    if (!data || !message || data_len != sizeof(peer_wire_message_t)) {
        return false;
    }
    peer_wire_message_t wire;
    memcpy(&wire, data, sizeof(wire));
    if (wire.magic != MOSAICO_PEER_MAGIC ||
        wire.network_id != s_link.network_id ||
        wire.version != MOSAICO_PEER_VERSION ||
        wire.type < MOSAICO_PEER_MSG_HELLO ||
        wire.type > MOSAICO_PEER_MSG_DIAGNOSTIC ||
        wire.source_boot_id == 0 ||
        wire.payload_len > MOSAICO_PEER_PAYLOAD_SIZE) {
        return false;
    }
    *message = (mosaico_peer_message_t) {
        .type = (mosaico_peer_message_type_t)wire.type,
        .source_id = wire.source_id,
        .source_boot_id = wire.source_boot_id,
        .target_id = wire.target_id,
        .session_id = wire.session_id,
        .sequence = wire.sequence,
        .local_edge = (mosaico_edge_t)wire.local_edge,
        .peer_edge = (mosaico_edge_t)wire.peer_edge,
        .relative_rotation = wire.relative_rotation,
        .payload_len = wire.payload_len,
    };
    memcpy(message->payload, wire.payload, wire.payload_len);
    return true;
}

static void receive_callback(
    const esp_now_recv_info_t *info,
    const uint8_t *data,
    int data_len)
{
    if (!s_link.running || !info || !info->src_addr) {
        return;
    }
    peer_rx_item_t item = {0};
    if (!decode_message(data, data_len, &item.message)) {
        ESP_LOGD(TAG, "ignored invalid frame: length=%d", data_len);
        return;
    }
    if (device_id_from_mac(info->src_addr) != item.message.source_id) {
        ESP_LOGW(TAG, "ignored frame with mismatched source identity");
        return;
    }
    memcpy(item.source_mac, info->src_addr, sizeof(item.source_mac));
    item.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : INT8_MIN;
    QueueHandle_t queue = item.message.type == MOSAICO_PEER_MSG_DIAGNOSTIC ?
                          s_link.diagnostic_queue : s_link.receive_queue;
    if (xQueueSend(queue, &item, 0) != pdTRUE) {
        if (item.message.type == MOSAICO_PEER_MSG_DIAGNOSTIC) {
            ESP_LOGD(TAG, "diagnostic queue full: source=%012llx sequence=%lu",
                     (unsigned long long)item.message.source_id,
                     (unsigned long)item.message.sequence);
        } else {
            ESP_LOGW(TAG, "receive queue full: source=%012llx sequence=%lu",
                     (unsigned long long)item.message.source_id,
                     (unsigned long)item.message.sequence);
        }
    } else if (s_link.worker_task) {
        xTaskNotifyGive(s_link.worker_task);
    }
}

static void receive_worker(void *arg)
{
    (void)arg;
    peer_rx_item_t item;
    while (s_link.running) {
        while (xQueueReceive(s_link.receive_queue, &item, 0) == pdTRUE) {
            s_link.receive_cb(
                item.source_mac, item.rssi, &item.message, s_link.user_ctx);
        }
        if (xQueueReceive(s_link.diagnostic_queue, &item, 0) == pdTRUE) {
            s_link.receive_cb(
                item.source_mac, item.rssi, &item.message, s_link.user_ctx);
        }
        if (uxQueueMessagesWaiting(s_link.receive_queue) == 0 &&
            uxQueueMessagesWaiting(s_link.diagnostic_queue) == 0) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
    s_link.worker_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_wifi(uint8_t channel)
{
    esp_err_t ret = nvs_flash_init();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    s_link.sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!s_link.sta_netif) {
        s_link.sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_link.sta_netif) {
            return ESP_ERR_NO_MEM;
        }
        s_link.netif_owned = true;
    }

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_config);
    if (ret == ESP_OK) {
        s_link.wifi_owned = true;
    } else if (ret != ESP_ERR_WIFI_INIT_STATE) {
        return ret;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "use volatile Wi-Fi configuration failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set Wi-Fi station mode failed");
    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STOPPED) {
        return ret;
    }
    return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

static esp_err_t verify_existing_wifi(uint8_t channel)
{
    uint8_t current_channel = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    const esp_err_t ret = esp_wifi_get_channel(&current_channel, &second);
    if (ret != ESP_OK) {
        return ret;
    }
    return current_channel == channel ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t mosaico_peer_link_init(
    const mosaico_peer_link_config_t *config,
    mosaico_peer_receive_cb_t receive_cb,
    void *user_ctx)
{
    ESP_RETURN_ON_FALSE(receive_cb, ESP_ERR_INVALID_ARG, TAG, "receive callback is null");
    ESP_RETURN_ON_FALSE(!s_link.running, ESP_ERR_INVALID_STATE, TAG, "peer link is already running");
    const mosaico_peer_link_config_t active =
        config ? *config : (mosaico_peer_link_config_t)MOSAICO_PEER_LINK_CONFIG_DEFAULT();
    ESP_RETURN_ON_FALSE(active.channel >= 1 && active.channel <= 14 &&
                        active.network_id != 0 &&
                        active.receive_queue_depth > 0 &&
                        active.worker_stack_size >= 2048,
                        ESP_ERR_INVALID_ARG, TAG, "invalid peer link configuration");

    memset(&s_link, 0, sizeof(s_link));
    s_link.receive_cb = receive_cb;
    s_link.user_ctx = user_ctx;
    s_link.network_id = active.network_id;
    esp_err_t ret = ESP_OK;
    s_link.receive_queue = xQueueCreate(active.receive_queue_depth, sizeof(peer_rx_item_t));
    s_link.diagnostic_queue = xQueueCreate(
        active.receive_queue_depth, sizeof(peer_rx_item_t));
    s_link.send_lock = xSemaphoreCreateMutex();
    if (!s_link.receive_queue || !s_link.diagnostic_queue || !s_link.send_lock) {
        ESP_LOGE(TAG, "create receive queues failed");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    ret = active.manage_wifi ? start_wifi(active.channel) :
        verify_existing_wifi(active.channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "prepare Wi-Fi failed: managed=%d channel=%u error=%s",
                 active.manage_wifi, active.channel, esp_err_to_name(ret));
        goto fail;
    }
    ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "initialize ESP-NOW failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    ret = esp_now_register_recv_cb(receive_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register ESP-NOW receive callback failed: %s", esp_err_to_name(ret));
        esp_now_deinit();
        goto fail;
    }

    const esp_now_peer_info_t broadcast_peer = {
        /* Channel 0 follows the fixed active STA channel. */
        .channel = 0,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    esp_now_peer_info_t peer = broadcast_peer;
    memcpy(peer.peer_addr, s_broadcast_mac, sizeof(peer.peer_addr));
    ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "add broadcast peer failed: %s", esp_err_to_name(ret));
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        goto fail;
    }

    uint8_t mac[6];
    ret = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read station MAC failed: %s", esp_err_to_name(ret));
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        goto fail;
    }
    s_link.device_id = device_id_from_mac(mac);
    do {
        s_link.boot_id = esp_random();
    } while (s_link.boot_id == 0);
    s_link.running = true;
    if (xTaskCreate(receive_worker, "mosaico_peer", active.worker_stack_size, NULL,
                    active.worker_priority, &s_link.worker_task) != pdPASS) {
        ret = ESP_ERR_NO_MEM;
        s_link.running = false;
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        goto fail;
    }
    ESP_LOGI(TAG, "started: device=%012llx boot=%08lx network=%08lx channel=%u",
             (unsigned long long)s_link.device_id,
             (unsigned long)s_link.boot_id,
             (unsigned long)active.network_id, active.channel);
    return ESP_OK;

fail:
    if (s_link.wifi_owned) {
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
    }
    if (s_link.netif_owned) {
        esp_netif_destroy_default_wifi(s_link.sta_netif);
    }
    if (s_link.diagnostic_queue) {
        vQueueDelete(s_link.diagnostic_queue);
    }
    if (s_link.receive_queue) {
        vQueueDelete(s_link.receive_queue);
    }
    if (s_link.send_lock) {
        vSemaphoreDelete(s_link.send_lock);
    }
    memset(&s_link, 0, sizeof(s_link));
    return ret;
}

esp_err_t mosaico_peer_link_deinit(void)
{
    if (!s_link.running) {
        return ESP_OK;
    }
    s_link.running = false;
    xTaskNotifyGive(s_link.worker_task);
    ESP_RETURN_ON_ERROR(esp_now_unregister_recv_cb(), TAG, "unregister receive callback failed");
    ESP_RETURN_ON_ERROR(esp_now_deinit(), TAG, "deinitialize ESP-NOW failed");
    if (s_link.wifi_owned) {
        ESP_RETURN_ON_ERROR(esp_wifi_stop(), TAG, "stop Wi-Fi failed");
        ESP_RETURN_ON_ERROR(esp_wifi_deinit(), TAG, "deinitialize Wi-Fi failed");
    }
    if (s_link.netif_owned) {
        esp_netif_destroy_default_wifi(s_link.sta_netif);
    }
    while (s_link.worker_task) {
        vTaskDelay(1);
    }
    vQueueDelete(s_link.diagnostic_queue);
    vQueueDelete(s_link.receive_queue);
    vSemaphoreDelete(s_link.send_lock);
    memset(&s_link, 0, sizeof(s_link));
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

uint64_t mosaico_peer_link_get_device_id(void)
{
    return s_link.device_id;
}

uint32_t mosaico_peer_link_get_boot_id(void)
{
    return s_link.boot_id;
}

esp_err_t mosaico_peer_link_send_broadcast(
    mosaico_peer_message_type_t type,
    uint32_t session_id,
    mosaico_edge_t local_edge,
    mosaico_edge_t peer_edge,
    uint16_t relative_rotation,
    const void *payload,
    size_t payload_len)
{
    return mosaico_peer_link_send_broadcast_to(
        type, 0, session_id, local_edge, peer_edge, relative_rotation,
        payload, payload_len);
}

esp_err_t mosaico_peer_link_send_broadcast_to(
    mosaico_peer_message_type_t type,
    uint64_t target_id,
    uint32_t session_id,
    mosaico_edge_t local_edge,
    mosaico_edge_t peer_edge,
    uint16_t relative_rotation,
    const void *payload,
    size_t payload_len)
{
    ESP_RETURN_ON_FALSE(s_link.running, ESP_ERR_INVALID_STATE, TAG, "peer link is not running");
    ESP_RETURN_ON_FALSE(type >= MOSAICO_PEER_MSG_HELLO &&
                        type <= MOSAICO_PEER_MSG_DIAGNOSTIC &&
                        payload_len <= MOSAICO_PEER_PAYLOAD_SIZE &&
                        (!payload_len || payload) &&
                        relative_rotation <= 270 &&
                        relative_rotation % 90 == 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid broadcast arguments");
    xSemaphoreTake(s_link.send_lock, portMAX_DELAY);
    peer_wire_message_t wire = {
        .magic = MOSAICO_PEER_MAGIC,
        .network_id = s_link.network_id,
        .version = MOSAICO_PEER_VERSION,
        .type = (uint8_t)type,
        .local_edge = (uint8_t)local_edge,
        .peer_edge = (uint8_t)peer_edge,
        .source_id = s_link.device_id,
        .source_boot_id = s_link.boot_id,
        .target_id = target_id,
        .session_id = session_id,
        .sequence = ++s_link.sequence,
        .relative_rotation = relative_rotation,
        .payload_len = payload_len,
    };
    if (payload_len) {
        memcpy(wire.payload, payload, payload_len);
    }
    uint8_t target_mac[6];
    const uint8_t *destination_mac = s_broadcast_mac;
    esp_err_t ret = ESP_OK;
    if (target_id != 0) {
        ret = ensure_unicast_peer(target_id, target_mac);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "prepare unicast peer failed: target=%012llx error=%s",
                     (unsigned long long)target_id, esp_err_to_name(ret));
            xSemaphoreGive(s_link.send_lock);
            return ret;
        }
        destination_mac = target_mac;
    }
    ret = esp_now_send(destination_mac, (const uint8_t *)&wire, sizeof(wire));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send peer frame failed: type=%u target=%012llx sequence=%lu error=%s",
                 type, (unsigned long long)target_id,
                 (unsigned long)wire.sequence, esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "peer frame queued: type=%u target=%012llx sequence=%lu",
                 type, (unsigned long long)target_id,
                 (unsigned long)wire.sequence);
    }
    xSemaphoreGive(s_link.send_lock);
    return ret;
}
