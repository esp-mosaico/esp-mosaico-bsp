/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp_mosaico.h"
#include "esp_log.h"
#include "music_player.h"
#include "music_ui.h"
#include "touch_keys.h"

static const char *TAG = "esp32a1_player";
static atomic_bool s_player_ready;

static void on_player_state(const music_player_state_t *state, void *user_data)
{
    (void)user_data;
    music_ui_update(state);
}

static void on_touch_key(uint8_t channel, void *user_data)
{
    (void)user_data;
    if (!s_player_ready) {
        ESP_LOGW(TAG, "TK%u ignored: audio player is unavailable", channel);
        return;
    }

    music_player_command_t command;
    switch (channel) {
    case 5:
        command = MUSIC_PLAYER_VOLUME_DOWN;
        break;
    case 6:
        command = MUSIC_PLAYER_PREVIOUS;
        break;
    case 7:
        command = MUSIC_PLAYER_TOGGLE;
        break;
    case 8:
        command = MUSIC_PLAYER_NEXT;
        break;
    case 9:
        command = MUSIC_PLAYER_VOLUME_UP;
        break;
    default:
        return;
    }

    const esp_err_t ret = music_player_send(command);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TK%u command failed: %s", channel, esp_err_to_name(ret));
        return;
    }
}

static void on_screen_command(music_player_command_t command, void *user_data)
{
    (void)user_data;
    if (!s_player_ready) {
        ESP_LOGW(TAG, "screen command ignored: audio player is unavailable");
        return;
    }
    const esp_err_t ret = music_player_send(command);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "screen command %d failed: %s", command,
                 esp_err_to_name(ret));
    }
}

static void load_resources(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(80)); /* Allow the first loading frame to render. */
    music_ui_loading_stage("Initializing A1 / loading NAND music...");
    const esp_err_t player_ret = music_player_start(on_player_state, NULL);
    if (player_ret != ESP_OK) {
        ESP_LOGE(TAG, "audio player unavailable: %s", esp_err_to_name(player_ret));
        music_ui_set_error(player_ret == ESP_ERR_NOT_FOUND ? "A1 / MP3 MISSING" : "AUDIO OFFLINE");
        vTaskDelete(NULL);
        return;
    }
    music_ui_loading_stage("Initializing touch keys...");
    const esp_err_t touch_ret = touch_keys_start(on_touch_key, NULL);
    if (touch_ret != ESP_OK) {
        ESP_LOGE(TAG, "Si12T keys unavailable: %s", esp_err_to_name(touch_ret));
        music_ui_set_error("TOUCH OFFLINE");
        vTaskDelete(NULL);
        return;
    }
    s_player_ready = true;
    music_ui_finish_loading();
    ESP_LOGI(TAG, "ready: screen touch and Si12T controls enabled; "
                  "TK5 volume-, TK6 previous, TK7 play/pause, TK8 next, TK9 volume+");
    vTaskDelete(NULL);
}

void app_main(void)
{
    bsp_board_variant_t variant;
    ESP_ERROR_CHECK(bsp_board_variant_get(&variant));
    if(variant!=BSP_BOARD_VARIANT_V1_2) {
        ESP_LOGE(TAG,"this example requires ESP-Mosaico V1.2 hardware");return;
    }
    ESP_ERROR_CHECK(music_ui_start(on_screen_command,NULL));
    if(xTaskCreate(load_resources,"mp3_load",8192,NULL,2,NULL)!=pdPASS)
        music_ui_set_error("LOADER TASK FAILED");
}
