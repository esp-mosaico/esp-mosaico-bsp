/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file main.c
 * @brief Button-triggered ESP-NOW chat via mosaico_peer_link (no magnets).
 */

#include <stdio.h>
#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mosaico_peer_link.h"

static const char *TAG = "espnow_chat";

#define HELLO_PERIOD_MS   3000
#define APP_SESSION_ID    1U

static uint32_t s_chat_seq;

static void peer_receive_cb(const uint8_t source_mac[6],
                            int8_t rssi,
                            const mosaico_peer_message_t *message,
                            void *user_ctx)
{
    (void)user_ctx;
    if (!message) {
        return;
    }

    if (message->source_id == mosaico_peer_link_get_device_id()) {
        return;
    }

    if (message->type == MOSAICO_PEER_MSG_APP_DATA) {
        char text[MOSAICO_PEER_PAYLOAD_SIZE + 1];
        size_t n = message->payload_len;
        if (n > MOSAICO_PEER_PAYLOAD_SIZE) {
            n = MOSAICO_PEER_PAYLOAD_SIZE;
        }
        memcpy(text, message->payload, n);
        text[n] = '\0';
        ESP_LOGI(TAG,
                 "CHAT from %02x:%02x:%02x:%02x:%02x:%02x rssi=%d: %s",
                 source_mac[0], source_mac[1], source_mac[2],
                 source_mac[3], source_mac[4], source_mac[5],
                 (int)rssi, text);
        (void)bsp_led_set(true);
    } else if (message->type == MOSAICO_PEER_MSG_HELLO) {
        ESP_LOGI(TAG,
                 "HELLO from id=0x%llx rssi=%d",
                 (unsigned long long)message->source_id, (int)rssi);
    }
}

static esp_err_t send_chat(const char *text, size_t text_capacity)
{
    const size_t limit = text_capacity < MOSAICO_PEER_PAYLOAD_SIZE ?
                             text_capacity : MOSAICO_PEER_PAYLOAD_SIZE;
    const size_t len = strnlen(text, limit);
    return mosaico_peer_link_send_broadcast(
        MOSAICO_PEER_MSG_APP_DATA,
        APP_SESSION_ID,
        MOSAICO_EDGE_NONE,
        MOSAICO_EDGE_NONE,
        0,
        text,
        len);
}

static void button_event_cb(void *button_handle, void *user_data)
{
    (void)user_data;
    const button_event_t event = iot_button_get_event(button_handle);
    if (event != BUTTON_SINGLE_CLICK) {
        return;
    }

    char line[64];
    s_chat_seq++;
    snprintf(line, sizeof(line), "hello #%lu from 0x%llx",
             (unsigned long)s_chat_seq,
             (unsigned long long)mosaico_peer_link_get_device_id());
    esp_err_t ret = send_chat(line, sizeof(line));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "TX: %s", line);
    (void)bsp_led_set(true);
    vTaskDelay(pdMS_TO_TICKS(40));
    (void)bsp_led_set(false);
}

static esp_err_t setup_button(void)
{
    button_handle_t buttons[BSP_BUTTON_NUM] = {0};
    int count = 0;
    ESP_RETURN_ON_ERROR(bsp_iot_button_create(buttons, &count, BSP_BUTTON_NUM), TAG,
                        "button create");
    ESP_RETURN_ON_ERROR(
        iot_button_register_cb(buttons[BSP_BUTTON_AI], BUTTON_SINGLE_CLICK, NULL,
                               button_event_cb, NULL),
        TAG, "register click");
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP-NOW chat (no magnetic)");
    ESP_ERROR_CHECK(bsp_led_init());
    ESP_ERROR_CHECK(setup_button());

    mosaico_peer_link_config_t config = MOSAICO_PEER_LINK_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(mosaico_peer_link_init(&config, peer_receive_cb, NULL));
    ESP_LOGI(TAG, "device_id=0x%llx boot_id=%lu — press AI to chat",
             (unsigned long long)mosaico_peer_link_get_device_id(),
             (unsigned long)mosaico_peer_link_get_boot_id());

    while (true) {
        const char hello[] = "ping";
        (void)mosaico_peer_link_send_broadcast(
            MOSAICO_PEER_MSG_HELLO,
            APP_SESSION_ID,
            MOSAICO_EDGE_NONE,
            MOSAICO_EDGE_NONE,
            0,
            hello,
            sizeof(hello));
        vTaskDelay(pdMS_TO_TICKS(HELLO_PERIOD_MS));
    }
}
