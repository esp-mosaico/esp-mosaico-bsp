/* SPDX-License-Identifier: CC0-1.0 */

#include "diagnostic_stream.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mosaico_peer_link.h"

static const char *TAG = "mag_diag";

#define DIAG_QUEUE_DEPTH          16U
#define DIAG_RECORD_SIZE          192U
#define DIAG_FRAGMENT_HEADER_SIZE 5U
#define DIAG_FRAGMENT_DATA_SIZE   \
    (MOSAICO_PEER_PAYLOAD_SIZE - DIAG_FRAGMENT_HEADER_SIZE)
#define DIAG_MAX_FRAGMENTS        8U
#define DIAG_REASSEMBLY_SLOTS     16U
#define DIAG_REASSEMBLY_STALE_MS  2000U
#define DIAG_BROADCAST_INTERVAL_MS 10U

typedef struct {
    uint16_t length;
    char data[DIAG_RECORD_SIZE];
} diagnostic_record_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t record_id;
    uint8_t fragment_index;
    uint8_t fragment_count;
    uint8_t data_len;
    uint8_t data[DIAG_FRAGMENT_DATA_SIZE];
} diagnostic_fragment_t;

_Static_assert(sizeof(diagnostic_fragment_t) == MOSAICO_PEER_PAYLOAD_SIZE,
               "diagnostic fragment must fill one peer payload");

typedef struct {
    uint64_t source_id;
    uint32_t updated_ms;
    uint16_t received_mask;
    uint16_t length;
    uint8_t record_id;
    uint8_t fragment_count;
    char data[DIAG_RECORD_SIZE];
} diagnostic_reassembly_t;

static QueueHandle_t s_queue;
static diagnostic_reassembly_t s_reassembly[DIAG_REASSEMBLY_SLOTS];
static uint8_t s_record_id;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void sender_task(void *arg)
{
    (void)arg;
    diagnostic_record_t record;
    while (true) {
        if (xQueueReceive(s_queue, &record, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const uint8_t record_id = ++s_record_id;
        const uint8_t count = (record.length + DIAG_FRAGMENT_DATA_SIZE - 1U) /
                              DIAG_FRAGMENT_DATA_SIZE;
        for (uint8_t index = 0; index < count; ++index) {
            const size_t offset = index * DIAG_FRAGMENT_DATA_SIZE;
            const size_t remaining = record.length - offset;
            diagnostic_fragment_t fragment = {
                .version = 1,
                .record_id = record_id,
                .fragment_index = index,
                .fragment_count = count,
                .data_len = remaining < DIAG_FRAGMENT_DATA_SIZE ?
                            remaining : DIAG_FRAGMENT_DATA_SIZE,
            };
            memcpy(fragment.data, record.data + offset, fragment.data_len);
            const esp_err_t ret = mosaico_peer_link_send_broadcast(
                MOSAICO_PEER_MSG_DIAGNOSTIC, 0, MOSAICO_EDGE_NONE,
                MOSAICO_EDGE_NONE, 0, &fragment, sizeof(fragment));
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "broadcast diagnostic failed: id=%u part=%u/%u error=%s",
                         record_id, index + 1U, count, esp_err_to_name(ret));
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(DIAG_BROADCAST_INTERVAL_MS));
        }
    }
}

static diagnostic_reassembly_t *find_reassembly(
    uint64_t source_id,
    uint8_t record_id,
    uint8_t fragment_count)
{
    diagnostic_reassembly_t *available = NULL;
    diagnostic_reassembly_t *oldest = &s_reassembly[0];
    const uint32_t current_ms = now_ms();
    for (size_t i = 0; i < DIAG_REASSEMBLY_SLOTS; ++i) {
        diagnostic_reassembly_t *slot = &s_reassembly[i];
        if (slot->source_id == source_id && slot->record_id == record_id) {
            return slot->fragment_count == fragment_count ? slot : NULL;
        }
        if (!slot->source_id || current_ms - slot->updated_ms > DIAG_REASSEMBLY_STALE_MS) {
            available = slot;
        }
        if (slot->updated_ms < oldest->updated_ms) {
            oldest = slot;
        }
    }
    diagnostic_reassembly_t *slot = available ? available : oldest;
    memset(slot, 0, sizeof(*slot));
    slot->source_id = source_id;
    slot->record_id = record_id;
    slot->fragment_count = fragment_count;
    slot->updated_ms = current_ms;
    return slot;
}

esp_err_t diagnostic_stream_start(void)
{
    ESP_RETURN_ON_FALSE(!s_queue, ESP_ERR_INVALID_STATE, TAG,
                        "diagnostic stream is already running");
    s_queue = xQueueCreate(DIAG_QUEUE_DEPTH, sizeof(diagnostic_record_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG,
                        "create diagnostic queue failed");
    if (xTaskCreate(sender_task, "mag_diag_now", 4096, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ESP-NOW diagnostics started: relay_slots=%u queue=%u",
             DIAG_REASSEMBLY_SLOTS, DIAG_QUEUE_DEPTH);
    return ESP_OK;
}

void diagnostic_stream_publish(const char *record)
{
    if (!record) {
        return;
    }
    ESP_LOGI(TAG, "%s", record);
    if (!s_queue) {
        return;
    }
    diagnostic_record_t queued = {0};
    const size_t length = strnlen(record, sizeof(queued.data));
    if (length == 0 || length >= sizeof(queued.data)) {
        ESP_LOGW(TAG, "drop invalid diagnostic record: length=%u", (unsigned)length);
        return;
    }
    memcpy(queued.data, record, length);
    queued.length = (uint16_t)length;
    if (xQueueSend(s_queue, &queued, 0) != pdTRUE) {
        ESP_LOGW(TAG, "diagnostic queue full");
    }
}

void diagnostic_stream_receive(
    uint64_t source_id,
    int8_t rssi,
    const void *payload,
    size_t payload_len)
{
    if (!payload || payload_len != sizeof(diagnostic_fragment_t) || !source_id) {
        return;
    }
    diagnostic_fragment_t fragment;
    memcpy(&fragment, payload, sizeof(fragment));
    if (fragment.version != 1 || fragment.fragment_count == 0 ||
        fragment.fragment_count > DIAG_MAX_FRAGMENTS ||
        fragment.fragment_index >= fragment.fragment_count ||
        fragment.data_len == 0 || fragment.data_len > DIAG_FRAGMENT_DATA_SIZE) {
        return;
    }
    const size_t offset = fragment.fragment_index * DIAG_FRAGMENT_DATA_SIZE;
    if (offset + fragment.data_len >= DIAG_RECORD_SIZE) {
        return;
    }
    diagnostic_reassembly_t *slot = find_reassembly(
        source_id, fragment.record_id, fragment.fragment_count);
    if (!slot) {
        return;
    }
    memcpy(slot->data + offset, fragment.data, fragment.data_len);
    slot->received_mask |= (uint16_t)1U << fragment.fragment_index;
    const size_t end = offset + fragment.data_len;
    if (end > slot->length) {
        slot->length = end;
    }
    slot->updated_ms = now_ms();
    const uint16_t complete_mask = ((uint16_t)1U << fragment.fragment_count) - 1U;
    if (slot->received_mask == complete_mask) {
        slot->data[slot->length] = '\0';
        ESP_LOGI(TAG, "relay source=%012" PRIx64 " rssi=%d %s",
                 source_id, rssi, slot->data);
        memset(slot, 0, sizeof(*slot));
    }
}
