// SPDX-License-Identifier: Apache-2.0
#define GSP_BUNDLE_ENABLE_RAW_IDS 1
#include "bundle_gsp.h"
#include <math.h>
#include "esp_log.h"
#include "game_audio.h"
#include "mosaico_game_2d.h"
#include "mosaico_game_assets.h"
#include "raylib_screen_mirror.h"
#include "shooter_app.h"
#include "shooter_game.h"
#include "shooter_view.h"

static const char *TAG = "raylib_shooter";
static shooter_game_t s_game;
static MosaicoAtlas s_atlas;
static bool s_input;
static float s_imu_x, s_imu_y;

extern const uint8_t _binary_shooter_atlas_start[];
extern const uint8_t _binary_shooter_atlas_end[];

static esp_err_t before_display(void)
{
    esp_err_t err=mosaico_game_asset_register_memory(
        "shooter.atlas",_binary_shooter_atlas_start,
        (size_t)(_binary_shooter_atlas_end-_binary_shooter_atlas_start));
    if(err!=ESP_OK)return err;
    s_atlas=LoadMosaicoAtlas("shooter.atlas");
    return s_atlas.texture.id?ESP_OK:ESP_ERR_NOT_FOUND;
}

static esp_err_t on_start(void)
{
    shooter_game_reset(&s_game, 0x4d4f5341U);
    return ESP_OK;
}

static esp_err_t after_healthy(void)
{
    return game_audio_init();
}

static void on_event(const mosaico_device_event_t *event)
{
    if (event->type == MOSAICO_DEVICE_EVENT_IMU) {
        const float x = event->x / 1000.0f;
        const float y = event->y / 1000.0f;
        s_imu_x += (x - s_imu_x) * 0.22f;
        s_imu_y += (y - s_imu_y) * 0.22f;
        return;
    }
    if (event->type != MOSAICO_DEVICE_EVENT_POINTER) return;
    s_input = true;
    shooter_phase_t before = s_game.phase;
    shooter_game_set_pointer(&s_game, (float)event->x, (float)event->y, event->pressed);
    if (before != SHOOTER_PLAYING && s_game.phase == SHOOTER_PLAYING)
        (void)game_audio_play(GAME_AUDIO_START);
}

static bool idle(void)
{
    bool skip = s_game.phase != SHOOTER_PLAYING && !s_input;
    s_input = false;
    return skip;
}

static void on_update(void)
{
    uint32_t old_score = s_game.score, old_shots = s_game.shots_fired;
    uint8_t old_lives = s_game.lives;
    shooter_phase_t old_phase = s_game.phase;
    const float x = fabsf(s_imu_x) > 0.10f ? s_imu_x : 0.0f;
    const float y = fabsf(s_imu_y) > 0.10f ? s_imu_y : 0.0f;
    shooter_game_move(&s_game, x * 10.0f, y * 10.0f);
    shooter_game_update(&s_game);
    if (s_game.shots_fired > old_shots) (void)game_audio_play(GAME_AUDIO_SHOT);
    if (s_game.score > old_score) (void)game_audio_play(GAME_AUDIO_DESTROY);
    if (s_game.lives < old_lives)
        (void)game_audio_play(s_game.phase == SHOOTER_GAME_OVER ? GAME_AUDIO_GAME_OVER
                                                               : GAME_AUDIO_HIT);
    else if (old_phase != SHOOTER_GAME_OVER && s_game.phase == SHOOTER_GAME_OVER)
        (void)game_audio_play(GAME_AUDIO_GAME_OVER);
}

static void on_render(void)
{
    shooter_view_render(&s_game,s_atlas);
}

static void on_stats(void)
{
    ESP_LOGI(TAG, "state_hash=%08lx", (unsigned long)shooter_game_state_hash(&s_game));
}

static const mosaico_game_app_config_t s_config = {
    .tag = "raylib_shooter",
    .window_title = "Mosaico Strike",
    .canvas_bind = GSP_RAYLIB_SHOOTER_BIND_GAME_CANVAS,
    .touch_points = 1,
    .enable_imu = true,
    .imu_sample_ms = 20,
    .target_fps = 30,
    .gsp_bundle = gsp_bundle_config,
    .register_mirror = raylib_screen_mirror_register,
    .before_display = before_display,
    .on_start = on_start,
    .after_healthy = after_healthy,
    .on_event = on_event,
    .idle = idle,
    .on_update = on_update,
    .on_render = on_render,
    .on_stats = on_stats,
};

const mosaico_game_app_config_t *shooter_app_config(void)
{
    return &s_config;
}
