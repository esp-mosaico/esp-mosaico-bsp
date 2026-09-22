// SPDX-License-Identifier: Apache-2.0
#include "sky_hop_save.h"

#include "nvs.h"
#include "esp_timer.h"
#include "mosaico_game_save.h"

#define SKY_HOP_SAVE_VERSION 1U
static mosaico_save_t s_save;
static bool s_ready;
static uint64_t now_ms(void){return (uint64_t)esp_timer_get_time()/1000U;}
static esp_err_t ensure_ready(void){if(s_ready)return ESP_OK;mosaico_save_config_t c={"sky_hop","state",SKY_HOP_SAVE_VERSION,sizeof(uint16_t),750,NULL};esp_err_t e=mosaico_save_init(&s_save,&c);s_ready=e==ESP_OK;return e;}

esp_err_t sky_hop_save_load(uint16_t *best_score)
{
    if (!best_score) return ESP_ERR_INVALID_ARG;
    *best_score = 0;
    esp_err_t packed=ensure_ready();
    if(packed!=ESP_OK)return packed;
    packed=mosaico_save_load(&s_save,best_score,NULL);
    if(packed==ESP_OK&&*best_score)return ESP_OK;
    if(packed!=ESP_OK&&packed!=ESP_ERR_INVALID_CRC)return packed;
    nvs_handle_t handle;
    esp_err_t error = nvs_open("sky_hop", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (error != ESP_OK) return error;
    uint8_t version = 0;
    error = nvs_get_u8(handle, "version", &version);
    if (error == ESP_OK && version == SKY_HOP_SAVE_VERSION)
        error = nvs_get_u16(handle, "best", best_score);
    nvs_close(handle);
    error=error == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : error;
    if(error==ESP_OK&&*best_score){(void)mosaico_save_request(&s_save,best_score,now_ms());(void)mosaico_save_flush(&s_save,now_ms(),true);}
    return error;
}

esp_err_t sky_hop_save_best_score(uint16_t best_score)
{
    esp_err_t error=ensure_ready();
    return error==ESP_OK?mosaico_save_request(&s_save,&best_score,now_ms()):error;
}

esp_err_t sky_hop_save_flush(void){return s_ready?mosaico_save_flush(&s_save,now_ms(),false):ESP_OK;}
