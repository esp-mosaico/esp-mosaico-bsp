/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "music_player.h"
#include "a1_audio.h"
#include "nand_littlefs.h"

#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_audio_simple_player.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define NAND_MOUNT_POINT         "/nandflash"
#define MUSIC_DIRECTORY          NAND_MOUNT_POINT "/music"
#define MUSIC_FILE_URI_PREFIX    "file://"
#define MUSIC_MAX_TRACKS         64U
#define MUSIC_URI_SIZE           320U
#define PLAYER_TASK_STACK_SIZE   6144U
#define PLAYER_TASK_PRIORITY     5U
#define PLAYER_INITIAL_VOLUME    50U
#define PLAYER_VOLUME_STEP       5U
#define PLAYER_OUTPUT_BITS       16
#define PLAYER_OUTPUT_CHANNELS   2

typedef enum {
    PLAYER_EVENT_COMMAND,
    PLAYER_EVENT_FINISHED,
    PLAYER_EVENT_ERROR,
} player_event_type_t;

typedef struct {
    player_event_type_t type;
    music_player_command_t command;
} player_event_t;

static const char *TAG = "music_player";
static esp_asp_handle_t s_player;
static QueueHandle_t s_events;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static char *s_track_names[MUSIC_MAX_TRACKS];
static size_t s_track_count;
static size_t s_track_index;
static uint8_t s_volume = PLAYER_INITIAL_VOLUME;
static bool s_playing = false;
/* Only the control task accesses this; an idle decoder cannot be resumed. */
static bool s_track_started;
static atomic_bool s_track_has_pcm;
static music_player_state_callback_t s_state_callback;
static void *s_state_user_data;
static bool s_pcm_output_seen;
static bool s_speaker_open;
static int s_output_rate;

static esp_err_t set_speaker_mute(bool mute)
{
    if (!s_speaker_open) {
        return ESP_OK;
    }
    return a1_audio_set_mute(mute);
}

static esp_err_t open_speaker(int sample_rate)
{
    ESP_RETURN_ON_FALSE(sample_rate >= 8000 && sample_rate <= 96000,
                        ESP_ERR_INVALID_ARG, TAG, "unsupported sample rate: %d", sample_rate);
    if (s_speaker_open && s_output_rate == sample_rate) {
        return ESP_OK;
    }
    if (s_speaker_open) {
        s_speaker_open = false;
        s_output_rate = 0;
    }

    ESP_RETURN_ON_ERROR(a1_audio_open((uint32_t)sample_rate, s_volume), TAG,
                        "open A1 audio output failed");
    s_speaker_open = true;
    s_output_rate = sample_rate;
    ESP_RETURN_ON_ERROR(set_speaker_mute(true), TAG, "mute speaker before PCM failed");
    ESP_LOGI(TAG, "A1 output ready (muted): %d Hz, %d channel(s), %d bit, volume=%u",
             sample_rate, PLAYER_OUTPUT_CHANNELS, PLAYER_OUTPUT_BITS,
             (unsigned)s_volume);
    return ESP_OK;
}

static bool has_mp3_extension(const char *name)
{
    const size_t length = strlen(name);
    return length >= 4 && strcasecmp(name + length - 4, ".mp3") == 0;
}

static int compare_track_names(const void *left, const void *right)
{
    const char *const *left_name = left;
    const char *const *right_name = right;
    return strcasecmp(*left_name, *right_name);
}

static esp_err_t validate_track_file(const char *name)
{
    char path[MUSIC_URI_SIZE];
    const int length = snprintf(path, sizeof(path), MUSIC_DIRECTORY "/%s", name);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        ESP_LOGW(TAG, "skip MP3: path too long: %s", name);
        return ESP_ERR_INVALID_SIZE;
    }
    struct stat info;
    if (stat(path, &info) != 0) {
        ESP_LOGW(TAG, "MP3 stat failed: %s, errno=%d (%s)", path, errno, strerror(errno));
        return ESP_FAIL;
    }
    if (!S_ISREG(info.st_mode) || info.st_size <= 0) {
        ESP_LOGW(TAG, "skip empty/non-regular MP3: %s, size=%lld", path, (long long)info.st_size);
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        ESP_LOGW(TAG, "MP3 open failed: %s, errno=%d (%s)", path, errno, strerror(errno));
        return ESP_FAIL;
    }
    const int first_byte = fgetc(file);
    const int read_error = ferror(file);
    const int saved_errno = errno;
    fclose(file);
    if (first_byte == EOF) {
        ESP_LOGW(TAG, "MP3 first read failed: %s, size=%lld, ferror=%d errno=%d",
                 path, (long long)info.st_size, read_error, saved_errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t mount_and_scan_music(void)
{
    ESP_RETURN_ON_ERROR(nand_littlefs_mount(NAND_MOUNT_POINT), TAG,
                        "mount NAND LittleFS at %s failed", NAND_MOUNT_POINT);

    DIR *directory = opendir(MUSIC_DIRECTORY);
    ESP_RETURN_ON_FALSE(directory, ESP_ERR_NOT_FOUND, TAG, "open %s failed", MUSIC_DIRECTORY);

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL && s_track_count < MUSIC_MAX_TRACKS) {
        if (!has_mp3_extension(entry->d_name)) {
            continue;
        }
        if (validate_track_file(entry->d_name) != ESP_OK) {
            continue;
        }
        s_track_names[s_track_count] = strdup(entry->d_name);
        if (!s_track_names[s_track_count]) {
            closedir(directory);
            return ESP_ERR_NO_MEM;
        }
        ++s_track_count;
    }
    closedir(directory);

    ESP_RETURN_ON_FALSE(s_track_count > 0, ESP_ERR_NOT_FOUND, TAG,
                        "no MP3 files found in NAND directory %s", MUSIC_DIRECTORY);
    qsort(s_track_names, s_track_count, sizeof(s_track_names[0]), compare_track_names);
    ESP_LOGI(TAG, "found %u MP3 file(s) in %s", (unsigned)s_track_count, MUSIC_DIRECTORY);
    for (size_t index = 0; index < s_track_count; ++index) {
        ESP_LOGI(TAG, "  %02u: %s", (unsigned)(index + 1), s_track_names[index]);
    }
    return ESP_OK;
}

void music_player_get_state(music_player_state_t *state)
{
    if (!state) {
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    state->track_index = s_track_index;
    state->track_count = s_track_count;
    state->title = s_track_count > 0 ? s_track_names[s_track_index] : "No MP3 files";
    state->artist = "MP3 / NAND";
    state->volume = s_volume;
    state->playing = s_playing;
    taskEXIT_CRITICAL(&s_state_lock);
}

static void notify_state_changed(void)
{
    if (!s_state_callback) {
        return;
    }
    music_player_state_t state;
    music_player_get_state(&state);
    s_state_callback(&state, s_state_user_data);
}

static int output_callback(uint8_t *data, int data_size, void *context)
{
    (void)context;
    if (!data || data_size <= 0) {
        return 0;
    }

    const esp_err_t result = a1_audio_write(data, (size_t)data_size);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "A1 write failed: %s", esp_err_to_name(result));
    } else if (!s_pcm_output_seen) {
        s_track_has_pcm = true;
        int peak = 0;
        for (int offset = 0; offset + (int)sizeof(int16_t) <= data_size;
                offset += sizeof(int16_t)) {
            int16_t sample;
            memcpy(&sample, data + offset, sizeof(sample));
            int magnitude = sample < 0 ? -(int)sample : (int)sample;
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
        s_pcm_output_seen = true;
        if (set_speaker_mute(false) == ESP_OK) {
            ESP_LOGI(TAG, "PCM output started: %d bytes, peak=%d -> A1/TDM GPIO52",
                     data_size, peak);
        }
    }
    return 0;
}

static int player_event_callback(esp_asp_event_pkt_t *event, void *context)
{
    (void)context;
    if (!event || !event->payload) {
        return 0;
    }

    if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO &&
            event->payload_size >= sizeof(esp_asp_music_info_t)) {
        esp_asp_music_info_t info;
        memcpy(&info, event->payload, sizeof(info));
        ESP_LOGI(TAG, "stream: %d Hz, %u channel(s), %u bit",
                 info.sample_rate, info.channels, info.bits);

        /*
         * Keep the native MP3 sample rate.  On this target the extra software
         * resampler can turn otherwise valid 44.1 kHz PCM into noise.  Channel
         * count and bit depth are still normalized by the GMF filters.
         */
        if (info.channels != PLAYER_OUTPUT_CHANNELS ||
                info.bits != PLAYER_OUTPUT_BITS) {
            ESP_LOGE(TAG, "unexpected player output format; expected %d ch/%d bit",
                     PLAYER_OUTPUT_CHANNELS, PLAYER_OUTPUT_BITS);
            return 0;
        }
        if (open_speaker(info.sample_rate) != ESP_OK) {
            ESP_LOGE(TAG, "speaker output is unavailable");
        }
        return 0;
    }

    if (event->type != ESP_ASP_EVENT_TYPE_STATE ||
            event->payload_size < sizeof(esp_asp_state_t)) {
        return 0;
    }

    esp_asp_state_t state;
    memcpy(&state, event->payload, sizeof(state));
    ESP_LOGI(TAG, "decoder state: %s", esp_audio_simple_player_state_to_str(state));

    player_event_t message = {0};
    if (state == ESP_ASP_STATE_FINISHED) {
        /* Empty/truncated input must not masquerade as a completed song. */
        message.type = s_track_has_pcm ? PLAYER_EVENT_FINISHED : PLAYER_EVENT_ERROR;
        if (message.type == PLAYER_EVENT_ERROR) {
            ESP_LOGE(TAG, "track ended without PCM; pause instead of auto-advancing");
        }
    } else if (state == ESP_ASP_STATE_ERROR) {
        message.type = PLAYER_EVENT_ERROR;
    } else {
        return 0;
    }
    if (s_events && xQueueSend(s_events, &message, 0) != pdTRUE) {
        ESP_LOGE(TAG, "drop decoder event: queue full");
    }
    return 0;
}

static esp_err_t run_current_track(void)
{
    char uri[MUSIC_URI_SIZE];
    size_t index;
    taskENTER_CRITICAL(&s_state_lock);
    index = s_track_index;
    taskEXIT_CRITICAL(&s_state_lock);

    s_track_started = false;
    ESP_RETURN_ON_ERROR(validate_track_file(s_track_names[index]), TAG,
                        "selected MP3 is unavailable; playback paused");
    s_track_has_pcm = false;
    const int length = snprintf(uri, sizeof(uri), MUSIC_FILE_URI_PREFIX MUSIC_DIRECTORY "/%s",
                                s_track_names[index]);
    ESP_RETURN_ON_FALSE(length > 0 && (size_t)length < sizeof(uri), ESP_ERR_INVALID_SIZE,
                        TAG, "MP3 path is too long");
    ESP_LOGI(TAG, "play %u/%u: %s", (unsigned)(index + 1),
             (unsigned)s_track_count, s_track_names[index]);
    s_track_started = esp_audio_simple_player_run(s_player, uri, NULL) == ESP_GMF_ERR_OK;
    return s_track_started ? ESP_OK : ESP_FAIL;
}

static void handle_command(music_player_command_t command)
{
    bool playing;
    uint8_t volume;
    taskENTER_CRITICAL(&s_state_lock);
    playing = s_playing;
    volume = s_volume;
    taskEXIT_CRITICAL(&s_state_lock);

    esp_gmf_err_t result = ESP_GMF_ERR_OK;
    switch (command) {
    case MUSIC_PLAYER_PREVIOUS:
    case MUSIC_PLAYER_NEXT:
        (void)esp_audio_simple_player_stop(s_player);
        /* stop() waits for outstanding output callbacks.  Mute/reset only
         * afterwards so a final old-track buffer cannot unmute the PA. */
        (void)set_speaker_mute(true);
        s_pcm_output_seen = false;
        if (run_current_track() != ESP_OK) {
            result = ESP_GMF_ERR_FAIL;
        }
        break;
    case MUSIC_PLAYER_TOGGLE:
        if (playing) {
            s_pcm_output_seen = false;
            result = s_track_started ? esp_audio_simple_player_resume(s_player)
                                    : (run_current_track() == ESP_OK ? ESP_GMF_ERR_OK : ESP_GMF_ERR_FAIL);
        } else {
            (void)set_speaker_mute(true);
            if (s_track_started) {
                result = esp_audio_simple_player_pause(s_player);
            }
        }
        break;
    case MUSIC_PLAYER_VOLUME_DOWN:
    case MUSIC_PLAYER_VOLUME_UP:
        if (s_speaker_open &&
                a1_audio_set_volume(volume) != ESP_OK) {
            result = ESP_GMF_ERR_FAIL;
        }
        break;
    }
    if (result != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "apply command %d failed: %d", command, result);
        taskENTER_CRITICAL(&s_state_lock);
        s_playing = false;
        taskEXIT_CRITICAL(&s_state_lock);
        notify_state_changed();
    }
}

static void player_control_task(void *arg)
{
    (void)arg;
    /* Wait for a user command; do not start decoding or playback on boot. */

    player_event_t event;
    while (xQueueReceive(s_events, &event, portMAX_DELAY) == pdTRUE) {
        if (event.type == PLAYER_EVENT_COMMAND) {
            handle_command(event.command);
            continue;
        }
        if (event.type == PLAYER_EVENT_ERROR) {
            s_track_started = false;
            (void)set_speaker_mute(true);
            taskENTER_CRITICAL(&s_state_lock);
            s_playing = false;
            taskEXIT_CRITICAL(&s_state_lock);
            notify_state_changed();
            continue;
        }

        (void)set_speaker_mute(true);
        s_pcm_output_seen = false;
        taskENTER_CRITICAL(&s_state_lock);
        s_track_index = (s_track_index + 1U) % s_track_count;
        s_playing = true;
        taskEXIT_CRITICAL(&s_state_lock);
        notify_state_changed();
        if (run_current_track() != ESP_OK) {
            ESP_LOGE(TAG, "start next track failed");
            taskENTER_CRITICAL(&s_state_lock);
            s_playing = false;
            taskEXIT_CRITICAL(&s_state_lock);
            notify_state_changed();
        }
    }
}

esp_err_t music_player_start(music_player_state_callback_t callback, void *user_data)
{
    ESP_RETURN_ON_FALSE(!s_player, ESP_ERR_INVALID_STATE, TAG, "player already started");
    ESP_RETURN_ON_ERROR(a1_audio_init(), TAG, "A1 audio subboard unavailable");
    ESP_RETURN_ON_ERROR(mount_and_scan_music(), TAG, "load MP3 playlist failed");
    ESP_LOGI(TAG, "A1 detected and initialized; output stays muted until decoded PCM is ready");

    s_events = xQueueCreate(16, sizeof(player_event_t));
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "create player event queue failed");

    esp_asp_cfg_t player_config = {
        .in = {0},
        .out = {
            .cb = output_callback,
            .user_ctx = NULL,
        },
        .task_prio = PLAYER_TASK_PRIORITY,
        .task_stack = PLAYER_TASK_STACK_SIZE,
        .task_core = 0,
        .task_stack_in_ext = false,
    };
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_new(&player_config, &s_player) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "create MP3 decoder failed");
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_set_event(s_player, player_event_callback, NULL) ==
                            ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "set decoder callback failed");

    s_state_callback = callback;
    s_state_user_data = user_data;
    if (xTaskCreate(player_control_task, "mp3_control", PLAYER_TASK_STACK_SIZE, NULL,
                    PLAYER_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    notify_state_changed();
    return ESP_OK;
}

esp_err_t music_player_send(music_player_command_t command)
{
    ESP_RETURN_ON_FALSE(s_events && s_player, ESP_ERR_INVALID_STATE, TAG,
                        "player is not started");
    ESP_RETURN_ON_FALSE(command >= MUSIC_PLAYER_PREVIOUS && command <= MUSIC_PLAYER_VOLUME_UP,
                        ESP_ERR_INVALID_ARG, TAG, "invalid command");

    taskENTER_CRITICAL(&s_state_lock);
    switch (command) {
    case MUSIC_PLAYER_PREVIOUS:
        s_track_index = (s_track_index + s_track_count - 1U) % s_track_count;
        s_playing = true;
        break;
    case MUSIC_PLAYER_TOGGLE:
        s_playing = !s_playing;
        break;
    case MUSIC_PLAYER_NEXT:
        s_track_index = (s_track_index + 1U) % s_track_count;
        s_playing = true;
        break;
    case MUSIC_PLAYER_VOLUME_DOWN:
        s_volume = s_volume >= PLAYER_VOLUME_STEP ? s_volume - PLAYER_VOLUME_STEP : 0;
        break;
    case MUSIC_PLAYER_VOLUME_UP:
        s_volume = s_volume <= 100U - PLAYER_VOLUME_STEP ? s_volume + PLAYER_VOLUME_STEP : 100;
        break;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    const player_event_t event = {
        .type = PLAYER_EVENT_COMMAND,
        .command = command,
    };
    ESP_RETURN_ON_FALSE(xQueueSend(s_events, &event, 0) == pdTRUE, ESP_ERR_TIMEOUT,
                        TAG, "player command queue is full");
    notify_state_changed();
    return ESP_OK;
}
