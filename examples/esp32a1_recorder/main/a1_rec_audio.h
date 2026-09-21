#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
esp_err_t a1_rec_audio_init(void);
esp_err_t a1_rec_audio_read(uint8_t *five_channel_pcm, size_t bytes);
esp_err_t a1_rec_audio_write(uint8_t *stereo_pcm, size_t bytes);
void a1_rec_audio_speaker(bool enabled);
void a1_rec_audio_update_route(bool playing);
esp_err_t a1_rec_audio_set_volume(unsigned volume);
