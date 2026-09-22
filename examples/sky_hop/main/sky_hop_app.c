// SPDX-License-Identifier: Apache-2.0
#define GSP_BUNDLE_ENABLE_RAW_IDS 1
#include "bundle_gsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "mosaico_game_2d.h"
#include "mosaico_game_fx.h"
#include "mmap_generate_game_assets.h"
#include "mosaico_game_action.h"
#include "mosaico_game_assets.h"
#include "mosaico_game_audio.h"
#include "platform_game.h"
#include "raylib_screen_mirror.h"
#include "sdkconfig.h"
#include "sky_hop_app.h"
#include "sky_hop_save.h"
#include "sky_hop_view.h"

static const char *TAG = "sky_hop";
static platform_game_t s_game;
static MosaicoAtlas s_atlas;
static Sound s_jump, s_coin, s_stomp, s_hurt, s_win;
static Music s_music;
static uint16_t s_best_score;
static mosaico_particle_t s_particles[SKY_HOP_PARTICLE_COUNT];
static mosaico_particle_pool_t s_particle_pool;
static sky_hop_overlay_t s_overlay;

#define EMBEDDED_ASSET(symbol) \
    extern const uint8_t _binary_##symbol##_start[]; \
    extern const uint8_t _binary_##symbol##_end[]
EMBEDDED_ASSET(sky_hop_atlas);
EMBEDDED_ASSET(sky_hop_jump);
EMBEDDED_ASSET(sky_hop_coin);
EMBEDDED_ASSET(sky_hop_stomp);
EMBEDDED_ASSET(sky_hop_hurt);
EMBEDDED_ASSET(sky_hop_win);
EMBEDDED_ASSET(sky_hop_music);

static void register_embedded_asset(const char *name, const uint8_t *start,
                                    const uint8_t *end)
{
    ESP_ERROR_CHECK(mosaico_game_asset_register_memory(name, start, (size_t)(end - start)));
}

static esp_err_t before_display(void)
{
    register_embedded_asset("tower.atlas", _binary_sky_hop_atlas_start, _binary_sky_hop_atlas_end);
    register_embedded_asset("jump.sound", _binary_sky_hop_jump_start, _binary_sky_hop_jump_end);
    register_embedded_asset("coin.sound", _binary_sky_hop_coin_start, _binary_sky_hop_coin_end);
    register_embedded_asset("stomp.sound", _binary_sky_hop_stomp_start, _binary_sky_hop_stomp_end);
    register_embedded_asset("hurt.sound", _binary_sky_hop_hurt_start, _binary_sky_hop_hurt_end);
    register_embedded_asset("win.sound", _binary_sky_hop_win_start, _binary_sky_hop_win_end);
    register_embedded_asset("music.sound", _binary_sky_hop_music_start, _binary_sky_hop_music_end);
    mosaico_asset_store_config_t assets = {.partition_label = "game_assets",
        .max_files = MMAP_GAME_ASSETS_FILES, .checksum = MMAP_GAME_ASSETS_CHECKSUM,
        .mmap_enable = true};
    esp_err_t err = mosaico_game_assets_mount(&assets);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "asset partition unavailable, using embedded assets: %s",
                 esp_err_to_name(err));
    s_atlas = LoadMosaicoAtlas("tower.atlas");
    if (!s_atlas.texture.id) return ESP_ERR_NOT_FOUND;
    static const mosaico_action_zone_t zones[] = {
        {0, 360, 150, 480, MOSAICO_ACTION_LEFT},
        {150, 360, 300, 480, MOSAICO_ACTION_RIGHT},
        {300, 360, 480, 480, MOSAICO_ACTION_JUMP},
    };
    mosaico_action_set_zones(zones, 3);
    return ESP_OK;
}

static esp_err_t on_start(void)
{
    mosaico_game_2d_reset_raster_stats();
    platform_game_reset(&s_game);
    mosaico_particle_pool_init(&s_particle_pool, s_particles, SKY_HOP_PARTICLE_COUNT);
    sky_hop_overlay_sync(&s_overlay, s_game.phase);
    esp_err_t err = sky_hop_save_load(&s_best_score);
    if (err != ESP_OK) ESP_LOGW(TAG, "save load failed: %s", esp_err_to_name(err));
    return ESP_OK;
}

static esp_err_t after_healthy(void)
{
    InitAudioDevice();
    s_jump = LoadSound("jump.sound"); s_coin = LoadSound("coin.sound");
    s_stomp = LoadSound("stomp.sound"); s_hurt = LoadSound("hurt.sound");
    s_win = LoadSound("win.sound"); s_music = LoadMusicStream("music.sound");
    SetMusicVolume(s_music, .18f);
    PlayMusicStream(s_music);
    return ESP_OK;
}

static void apply_menu_actions(void)
{
    if (!mosaico_action_pressed(MOSAICO_ACTION_RESTART) &&
        !mosaico_action_pressed(MOSAICO_ACTION_PAUSE)) return;
    bool pause_hit = false;
    for (size_t i = 0; i < MOSAICO_CONTACT_CAPACITY; ++i) {
        mosaico_input_contact_t contact = mosaico_action_contact(i);
        if (contact.active && contact.y < 54 && contact.x > 420) pause_hit = true;
    }
    if (s_game.phase == PLATFORM_PAUSED)
        platform_game_set_action(&s_game, PLATFORM_ACTION_PAUSE, true);
    else if (s_game.phase != PLATFORM_PLAYING)
        platform_game_set_action(&s_game, PLATFORM_ACTION_RESTART, true);
    else if (pause_hit || mosaico_action_pressed(MOSAICO_ACTION_PAUSE))
        platform_game_set_action(&s_game, PLATFORM_ACTION_PAUSE, true);
}

static void on_update(void)
{
#if CONFIG_SKY_HOP_BENCHMARK_MODE
    const bool playing = s_game.phase == PLATFORM_PLAYING;
    platform_game_set_action(&s_game, PLATFORM_ACTION_RESTART, !playing);
    platform_game_set_action(&s_game, PLATFORM_ACTION_LEFT,
        playing && ((s_game.tick / 180U) & 1U));
    platform_game_set_action(&s_game, PLATFORM_ACTION_RIGHT,
        playing && !((s_game.tick / 180U) & 1U));
    platform_game_set_action(&s_game, PLATFORM_ACTION_JUMP,
        playing && s_game.tick % 45U == 5U);
#else
    apply_menu_actions();
    platform_game_set_action(&s_game, PLATFORM_ACTION_LEFT,
                             mosaico_action_down(MOSAICO_ACTION_LEFT));
    platform_game_set_action(&s_game, PLATFORM_ACTION_RIGHT,
                             mosaico_action_down(MOSAICO_ACTION_RIGHT));
    platform_game_set_action(&s_game, PLATFORM_ACTION_JUMP,
                             mosaico_action_down(MOSAICO_ACTION_JUMP));
#endif
    bool grounded_before = s_game.grounded;
    uint16_t score_before = s_game.score;
    uint8_t lives_before = s_game.lives;
    platform_phase_t phase_before = s_game.phase;
    bool enemy_before[PLATFORM_ENEMY_COUNT];
    for (size_t i = 0; i < PLATFORM_ENEMY_COUNT; ++i)
        enemy_before[i] = s_game.enemies[i].active;
    platform_game_update(&s_game);
    if (grounded_before && !s_game.grounded) PlaySound(s_jump);
    if (s_game.score > score_before) {
        bool stomp = false;
        for (size_t i = 0; i < s_game.enemy_count; ++i)
            if (enemy_before[i] && !s_game.enemies[i].active) stomp = true;
        PlaySound(stomp ? s_stomp : s_coin);
        sky_hop_view_spawn_particles(&s_particle_pool, s_game.player_x + 14,
            s_game.player_y + 8,
            stomp ? (Color){205, 125, 255, 255} : (Color){255, 220, 70, 255}, 8);
    }
    if (s_game.lives < lives_before) {
        PlaySound(s_hurt);
        sky_hop_view_spawn_particles(&s_particle_pool, s_game.player_x + 14,
            s_game.player_y + 12, (Color){255, 95, 80, 255}, 12);
    }
    if (phase_before != s_game.phase &&
        (s_game.phase == PLATFORM_LEVEL_CLEAR || s_game.phase == PLATFORM_WON))
        PlaySound(s_win);
    if (s_game.score > s_best_score) s_best_score = s_game.score;
    if (phase_before != s_game.phase && (s_game.phase == PLATFORM_LEVEL_CLEAR ||
        s_game.phase == PLATFORM_WON || s_game.phase == PLATFORM_GAME_OVER)) {
        esp_err_t err = sky_hop_save_best_score(s_best_score);
        if (err != ESP_OK) ESP_LOGW(TAG, "save write failed: %s", esp_err_to_name(err));
    }
    mosaico_particle_pool_update(&s_particle_pool);
    sky_hop_overlay_sync(&s_overlay, s_game.phase);
    esp_err_t err = sky_hop_save_flush();
    if (err != ESP_OK) ESP_LOGW(TAG, "save flush failed: %s", esp_err_to_name(err));
}

static void on_render(void)
{
    sky_hop_view_t view = {
        .game = &s_game,
        .atlas = s_atlas,
        .best_score = s_best_score,
        .overlay_y = s_overlay.y,
        .particles = s_particles,
        .particle_count = SKY_HOP_PARTICLE_COUNT,
    };
    (void)sky_hop_view_render(&view);
}

static void on_stats(void)
{
    mosaico_game_2d_raster_stats_t raster;
    mosaico_game_2d_get_raster_stats(&raster);
    ESP_LOGI(TAG, "state_hash=%08lx", (unsigned long)platform_game_state_hash(&s_game));
    ESP_LOGI(TAG,
        "raster copy=%lu/%lu scale=%lu/%lu binary=%lu/%lu bin_copy=%lu/%lu bin_scale=%lu/%lu tile=%lu/%lu a8=%lu/%lu rotate=%lu/%lu lookup=%lu/%lu",
        (unsigned long)raster.opaque_copy_calls,
        (unsigned long)raster.opaque_copy_pixels,
        (unsigned long)raster.opaque_scale_calls,
        (unsigned long)raster.opaque_scale_pixels,
        (unsigned long)raster.binary_alpha_calls,
        (unsigned long)raster.binary_alpha_pixels,
        (unsigned long)raster.binary_copy_calls,
        (unsigned long)raster.binary_copy_pixels,
        (unsigned long)raster.binary_scale_calls,
        (unsigned long)raster.binary_scale_pixels,
        (unsigned long)raster.tile_row_calls,
        (unsigned long)raster.tile_row_pixels,
        (unsigned long)raster.alpha_calls,
        (unsigned long)raster.alpha_pixels,
        (unsigned long)raster.rotated_calls,
        (unsigned long)raster.rotated_pixels,
        (unsigned long)raster.frame_lookup_hits,
        (unsigned long)raster.frame_lookup_misses);
    mosaico_game_2d_reset_raster_stats();
}

static const mosaico_game_app_config_t s_config = {
    .tag = "sky_hop",
    .window_title = "Sky Hop",
    .canvas_bind = GSP_SKY_HOP_BIND_GAME_CANVAS,
    .touch_points = 2,
    .target_fps = 30,
#ifdef CONFIG_SKY_HOP_DRAWBUF_LINES
    .drawbuf_lines = CONFIG_SKY_HOP_DRAWBUF_LINES,
#endif
#ifdef CONFIG_SKY_HOP_TE_COMPOSE_BUFFERS
    .te_compose_buffers = CONFIG_SKY_HOP_TE_COMPOSE_BUFFERS,
#endif
    .stats_interval = 300,
    .gsp_bundle = gsp_bundle_config,
    .register_mirror = raylib_screen_mirror_register,
    .before_display = before_display,
    .on_start = on_start,
    .after_healthy = after_healthy,
    .on_event = NULL,
    .on_update = on_update,
    .on_render = on_render,
    .on_stats = on_stats,
};

const mosaico_game_app_config_t *sky_hop_app_config(void)
{
    return &s_config;
}
