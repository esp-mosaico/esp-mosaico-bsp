// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include "host_asset_runtime.h"
#include "mosaico_game_module.h"
#include "mosaico_raylib_fast.h"
#include "platform_game.h"
#include "sky_hop_view.h"

typedef struct {
    platform_game_t game;
    MosaicoAtlas atlas;
    mosaico_particle_t particles[SKY_HOP_PARTICLE_COUNT];
    mosaico_particle_pool_t pool;
    sky_hop_overlay_t overlay;
    uint16_t best_score;
    bool paused;
} sky_hop_module_state_t;

static sky_hop_view_t view_of(sky_hop_module_state_t *state)
{
    return (sky_hop_view_t){
        .game = &state->game,
        .atlas = state->atlas,
        .best_score = state->best_score,
        .overlay_y = state->overlay.y,
        .particles = state->particles,
        .particle_count = SKY_HOP_PARTICLE_COUNT,
    };
}

static int initialize(void *value, const char *asset_root)
{
    sky_hop_module_state_t *state = value;
    mosaico_host_assets_set_root(asset_root);
    state->atlas = LoadMosaicoAtlas("tower.atlas");
    if (!state->atlas.texture.id) return -1;
    InitWindow(480, 480, "Sky Hop");
    SetTargetFPS(30);
    platform_game_reset(&state->game);
    mosaico_particle_pool_init(&state->pool, state->particles, SKY_HOP_PARTICLE_COUNT);
    sky_hop_overlay_sync(&state->overlay, state->game.phase);
    return 0;
}

static void shutdown(void *value)
{
    sky_hop_module_state_t *state = value;
    if (state) UnloadMosaicoAtlas(state->atlas);
}

static void input(void *value, const mosaico_host_input_v1_t *event)
{
    sky_hop_module_state_t *state = value;
    if (!state || !event) return;
    if (event->type == MOSAICO_HOST_INPUT_ACTION && event->code >= 0 && event->code <= 4)
        platform_game_set_action(&state->game, (platform_action_t)event->code, event->pressed);
    else if (event->type == MOSAICO_HOST_INPUT_POINTER)
        platform_game_set_pointer(&state->game, (float)event->x, (float)event->y,
                                 event->pressed, event->track_id);
    else if (event->type == MOSAICO_HOST_INPUT_CONTROL &&
             event->code == MOSAICO_HOST_CONTROL_RESET)
        platform_game_reset(&state->game);
    else if (event->type == MOSAICO_HOST_INPUT_CONTROL &&
             event->code == MOSAICO_HOST_CONTROL_PAUSE)
        state->paused = true;
    else if (event->type == MOSAICO_HOST_INPUT_CONTROL &&
             event->code == MOSAICO_HOST_CONTROL_RESUME)
        state->paused = false;
}

static void update(void *value)
{
    sky_hop_module_state_t *state = value;
    if (!state || state->paused) return;
    uint16_t score_before = state->game.score;
    uint8_t lives_before = state->game.lives;
    bool enemy_before[PLATFORM_ENEMY_COUNT];
    for (size_t i = 0; i < PLATFORM_ENEMY_COUNT; ++i)
        enemy_before[i] = state->game.enemies[i].active;
    platform_game_update(&state->game);
    if (state->game.score > score_before) {
        bool stomp = false;
        for (size_t i = 0; i < state->game.enemy_count; ++i)
            if (enemy_before[i] && !state->game.enemies[i].active) stomp = true;
        sky_hop_view_spawn_particles(&state->pool, state->game.player_x + 14,
            state->game.player_y + 8,
            stomp ? (Color){205, 125, 255, 255} : (Color){255, 220, 70, 255}, 8);
    }
    if (state->game.lives < lives_before)
        sky_hop_view_spawn_particles(&state->pool, state->game.player_x + 14,
            state->game.player_y + 12, (Color){255, 95, 80, 255}, 12);
    if (state->game.score > state->best_score) state->best_score = state->game.score;
    mosaico_particle_pool_update(&state->pool);
    sky_hop_overlay_sync(&state->overlay, state->game.phase);
}

static int render(void *value)
{
    sky_hop_module_state_t *state = value;
    sky_hop_view_t view = view_of(state);
    return sky_hop_view_render(&view) ? 0 : -1;
}

static uint32_t state_hash(const void *value)
{
    return platform_game_state_hash(&((const sky_hop_module_state_t *)value)->game);
}

static int state_json(const void *value, char *output, size_t capacity)
{
    const sky_hop_module_state_t *state = value;
    static const char *phases[] = {
        "start", "playing", "paused", "level_clear", "won", "game_over"};
    const platform_game_t *game = &state->game;
    const char *phase = (unsigned)game->phase < 6 ? phases[game->phase] : "unknown";
    return snprintf(output, capacity,
        "{\"level\":%u,\"levels\":%u,\"score\":%u,\"lives\":%u,\"phase\":\"%s\","
        "\"tick\":%lu,\"state_hash\":\"%08lx\"}",
        game->level + 1, PLATFORM_LEVEL_COUNT, game->score, game->lives, phase,
        (unsigned long)game->tick, (unsigned long)platform_game_state_hash(game));
}

static const mosaico_game_module_v1_t s_module = {
    .descriptor = {MOSAICO_HOST_GAME_ABI_V1, "sky_hop", "Sky Hop", 480, 480, 30, 2},
    .state_size = sizeof(sky_hop_module_state_t),
    .initialize = initialize, .shutdown = shutdown, .input = input,
    .update = update, .render = render, .state_hash = state_hash,
    .state_json = state_json,
};

const mosaico_game_module_v1_t *mosaico_game_module_v1(void)
{
    return &s_module;
}
