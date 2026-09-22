// SPDX-License-Identifier: Apache-2.0
#define GSP_BUNDLE_ENABLE_RAW_IDS 1
#include "bundle_gsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "mmap_generate_game_assets.h"
#include "mosaico_game_2d.h"
#include "mosaico_game_assets.h"
#include "mosaico_game_tilemap.h"
#include "tower_app.h"
#include "tower_audio.h"
#include "tower_game.h"
#include "tower_screen_mirror.h"
#include "tower_view.h"

static const char *TAG = "tower_defense";
static tower_game_t s_game;
static MosaicoAtlas s_atlas;
static MosaicoTilemap s_map;
static tower_effect_t s_effects[TOWER_EFFECT_COUNT];
static bool s_input;

#define EMBEDDED_ASSET(symbol) \
    extern const uint8_t _binary_tower_##symbol##_start[]; \
    extern const uint8_t _binary_tower_##symbol##_end[]
EMBEDDED_ASSET(atlas);
EMBEDDED_ASSET(terrain);
EMBEDDED_ASSET(level);
EMBEDDED_ASSET(build);
EMBEDDED_ASSET(explosion);
EMBEDDED_ASSET(game_over);
EMBEDDED_ASSET(hit);
EMBEDDED_ASSET(leak);
EMBEDDED_ASSET(music);
EMBEDDED_ASSET(shot);
EMBEDDED_ASSET(wave);

static void register_embedded_asset(const char *name, const uint8_t *start,
                                    const uint8_t *end)
{
    ESP_ERROR_CHECK(mosaico_game_asset_register_memory(name, start,
                                                        (size_t)(end - start)));
}

static tower_view_t current_view(void)
{
    return (tower_view_t){
        .game = &s_game,
        .atlas = s_atlas,
        .map = s_map,
        .effects = s_effects,
        .effect_count = TOWER_EFFECT_COUNT,
    };
}

static esp_err_t before_display(void)
{
    register_embedded_asset("tower.atlas", _binary_tower_atlas_start,
                            _binary_tower_atlas_end);
    register_embedded_asset("terrain.atlas", _binary_tower_terrain_start,
                            _binary_tower_terrain_end);
    register_embedded_asset("level01.map", _binary_tower_level_start,
                            _binary_tower_level_end);
#define REGISTER_SOUND(name) \
    register_embedded_asset(#name ".sound", _binary_tower_##name##_start, \
                            _binary_tower_##name##_end)
    REGISTER_SOUND(build);
    REGISTER_SOUND(explosion);
    REGISTER_SOUND(game_over);
    REGISTER_SOUND(hit);
    REGISTER_SOUND(leak);
    REGISTER_SOUND(music);
    REGISTER_SOUND(shot);
    REGISTER_SOUND(wave);
#undef REGISTER_SOUND
    mosaico_asset_store_config_t assets = {.partition_label = "game_assets",
        .max_files = MMAP_GAME_ASSETS_FILES, .checksum = MMAP_GAME_ASSETS_CHECKSUM,
        .mmap_enable = true};
    esp_err_t err = mosaico_game_assets_mount(&assets);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "asset partition unavailable, using embedded assets: %s",
                 esp_err_to_name(err));
    s_atlas = LoadMosaicoAtlas("tower.atlas");
    if (!s_atlas.texture.id) return ESP_ERR_NOT_FOUND;
    s_map = LoadMosaicoTilemap("level01.map");
    if (!s_map) return ESP_ERR_NOT_FOUND;
    if (!tower_view_apply_map(&s_game, s_map))
        ESP_LOGW(TAG, "using built-in level fallback");
    return ESP_OK;
}

static esp_err_t on_start(void)
{
    tower_game_reset(&s_game, 0x544f5745U);
    return ESP_OK;
}

static esp_err_t after_healthy(void)
{
    return tower_audio_init();
}

static void on_event(const mosaico_device_event_t *event)
{
    if (event->type != MOSAICO_DEVICE_EVENT_POINTER) return;
    s_input = true;
    tower_phase_t before = s_game.phase;
    uint16_t credits = s_game.credits;
    tower_game_set_pointer(&s_game, event->x, event->y, event->pressed);
    if (s_game.credits < credits) (void)tower_audio_play(TOWER_AUDIO_BUILD);
    if (before != TOWER_PLAYING && s_game.phase == TOWER_PLAYING)
        (void)tower_audio_play(TOWER_AUDIO_START);
    if (before != s_game.phase)
        tower_audio_set_music(s_game.phase == TOWER_PAUSED, s_game.phase == TOWER_GAME_OVER);
}

static bool idle(void)
{
    bool skip = s_game.phase != TOWER_PLAYING && !s_input;
    s_input = false;
    return skip;
}

static void on_update(void)
{
    uint32_t shots_before = s_game.shots, kills_before = s_game.kills;
    uint16_t wave_before = s_game.wave;
    uint8_t hp_before = s_game.base_hp;
    bool enemy_before[TOWER_MAX_ENEMIES];
    float enemy_x[TOWER_MAX_ENEMIES], enemy_y[TOWER_MAX_ENEMIES];
    for (size_t i = 0; i < TOWER_MAX_ENEMIES; ++i) {
        enemy_before[i] = s_game.enemies[i].active;
        enemy_x[i] = s_game.enemies[i].x;
        enemy_y[i] = s_game.enemies[i].y;
    }
    tower_game_update(&s_game);
    for (size_t i = 0; i < TOWER_MAX_ENEMIES; ++i) {
        if (enemy_before[i] && !s_game.enemies[i].active && s_game.base_hp == hp_before)
            tower_view_add_explosion(s_effects, TOWER_EFFECT_COUNT, enemy_x[i], enemy_y[i]);
    }
    tower_view_tick_effects(s_effects, TOWER_EFFECT_COUNT);
    if (s_game.shots > shots_before) (void)tower_audio_play(TOWER_AUDIO_SHOT);
    if (s_game.kills > kills_before) (void)tower_audio_play(TOWER_AUDIO_KILL);
    if (s_game.wave > wave_before) (void)tower_audio_play(TOWER_AUDIO_WAVE);
    if (s_game.base_hp < hp_before)
        (void)tower_audio_play(s_game.phase == TOWER_GAME_OVER ? TOWER_AUDIO_GAME_OVER
                                                               : TOWER_AUDIO_LEAK);
}

static void on_render(void)
{
    tower_view_t view = current_view();
    tower_view_render(&view);
}

static void on_stats(void)
{
    ESP_LOGI(TAG, "wave=%u enemies=%u/%u state_hash=%08lx", s_game.wave,
             s_game.wave_spawned, s_game.wave_total,
             (unsigned long)tower_game_state_hash(&s_game));
}

static const mosaico_game_app_config_t s_config = {
    .tag = "tower_defense",
    .window_title = "Circuit Keep",
    .canvas_bind = GSP_TOWER_DEFENSE_BIND_GAME_CANVAS,
    .touch_points = 1,
    .target_fps = 30,
    .gsp_bundle = gsp_bundle_config,
    .register_mirror = tower_screen_mirror_register,
    .before_display = before_display,
    .on_start = on_start,
    .after_healthy = after_healthy,
    .on_event = on_event,
    .idle = idle,
    .on_update = on_update,
    .on_render = on_render,
    .on_stats = on_stats,
};

const mosaico_game_app_config_t *tower_app_config(void)
{
    return &s_config;
}
