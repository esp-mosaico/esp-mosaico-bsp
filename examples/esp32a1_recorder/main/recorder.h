#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#define REC_MAX_FILES 128
typedef struct { char name[32]; uint32_t seconds; } recording_t;
typedef enum { REC_STARTING, REC_IDLE, REC_RECORDING, REC_SAVING, REC_BUFFERING, REC_PLAYING, REC_ERROR } recorder_mode_t;
typedef struct {
    recorder_mode_t mode;
    char message[96];
    uint32_t seconds, revision;
    unsigned count, volume;
    recording_t files[REC_MAX_FILES];
} recorder_state_t;
esp_err_t recorder_start(void);
void recorder_snapshot(recorder_state_t *state);
bool recorder_toggle(void);
bool recorder_stop(void);
bool recorder_play(const char *name);
bool recorder_volume(bool increase);
/* Call only after the UI has obtained confirmation for this exact filename. */
bool recorder_delete(const char *name);
esp_err_t recorder_ui_start(void);
esp_err_t recorder_key_start(void);
