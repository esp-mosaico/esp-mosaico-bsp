// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include "host_asset_runtime.h"
#include "mosaico_game_module.h"
#include "mosaico_raylib_fast.h"
#include "tower_game.h"
#include "tower_view.h"

typedef struct {
    tower_game_t game;
    MosaicoAtlas atlas;
    MosaicoTilemap map;
    tower_effect_t effects[TOWER_EFFECT_COUNT];
    bool paused;
} tower_module_state_t;

static tower_view_t view_of(tower_module_state_t *state)
{
    return (tower_view_t){
        .game = &state->game,
        .atlas = state->atlas,
        .map = state->map,
        .effects = state->effects,
        .effect_count = TOWER_EFFECT_COUNT,
    };
}

static int initialize(void *value, const char *asset_root)
{
    tower_module_state_t *state = value;
    mosaico_host_assets_set_root(asset_root);
    state->atlas = LoadMosaicoAtlas("tower.atlas");
    state->map = LoadMosaicoTilemap("level01.map");
    if (!state->atlas.texture.id || !state->map) return -1;
    (void)tower_view_apply_map(&state->game, state->map);
    InitWindow(480, 480, "Circuit Keep");
    SetTargetFPS(30);
    tower_game_reset(&state->game, 0x544f5745U);
    return 0;
}

static void shutdown(void *value)
{
    tower_module_state_t *state = value;
    if (!state) return;
    UnloadMosaicoTilemap(state->map);
    UnloadMosaicoAtlas(state->atlas);
}

static void input(void *value, const mosaico_host_input_v1_t *event)
{
    tower_module_state_t *state = value;
    if (!state || !event) return;
    if (event->type == MOSAICO_HOST_INPUT_POINTER)
        tower_game_set_pointer(&state->game, (float)event->x, (float)event->y, event->pressed);
    else if (event->type == MOSAICO_HOST_INPUT_ACTION)
        tower_game_set_pointer(&state->game, 240, 220, event->pressed);
    else if (event->type == MOSAICO_HOST_INPUT_CONTROL &&
             event->code == MOSAICO_HOST_CONTROL_RESET)
        tower_game_reset(&state->game, 0x544f5745U);
    else if (event->type == MOSAICO_HOST_INPUT_CONTROL &&
             event->code == MOSAICO_HOST_CONTROL_PAUSE)
        state->paused = true;
    else if (event->type == MOSAICO_HOST_INPUT_CONTROL &&
             event->code == MOSAICO_HOST_CONTROL_RESUME)
        state->paused = false;
}

static void update(void *value)
{
    tower_module_state_t *state = value;
    if (!state || state->paused) return;
    uint8_t hp_before = state->game.base_hp;
    bool enemy_before[TOWER_MAX_ENEMIES];
    float enemy_x[TOWER_MAX_ENEMIES], enemy_y[TOWER_MAX_ENEMIES];
    for (size_t i = 0; i < TOWER_MAX_ENEMIES; ++i) {
        enemy_before[i] = state->game.enemies[i].active;
        enemy_x[i] = state->game.enemies[i].x;
        enemy_y[i] = state->game.enemies[i].y;
    }
    tower_game_update(&state->game);
    for (size_t i = 0; i < TOWER_MAX_ENEMIES; ++i) {
        if (enemy_before[i] && !state->game.enemies[i].active &&
                state->game.base_hp == hp_before)
            tower_view_add_explosion(state->effects, TOWER_EFFECT_COUNT,
                                     enemy_x[i], enemy_y[i]);
    }
    tower_view_tick_effects(state->effects, TOWER_EFFECT_COUNT);
}

static int render(void *value)
{
    tower_module_state_t *state = value;
    tower_view_t view = view_of(state);
    tower_view_render(&view);
    return 0;
}

static uint32_t state_hash(const void *value)
{
    return tower_game_state_hash(&((const tower_module_state_t *)value)->game);
}

static int state_json(const void *value, char *output, size_t capacity)
{
    const tower_game_t *game = &((const tower_module_state_t *)value)->game;
    static const char *phases[] = {"start", "playing", "paused", "game_over"};
    const char *phase = (unsigned)game->phase < 4 ? phases[game->phase] : "unknown";
    return snprintf(output, capacity,
        "{\"wave\":%u,\"score\":%lu,\"credits\":%u,\"base_hp\":%u,\"kills\":%u,"
        "\"phase\":\"%s\",\"tick\":%lu,\"state_hash\":\"%08lx\"}",
        game->wave, (unsigned long)game->score, game->credits, game->base_hp,
        game->kills, phase, (unsigned long)game->tick,
        (unsigned long)tower_game_state_hash(game));
}

static const mosaico_game_module_v1_t s_module = {
    .descriptor = {MOSAICO_HOST_GAME_ABI_V1, "tower_defense", "Circuit Keep",
                   480, 480, 30, 1},
    .state_size = sizeof(tower_module_state_t),
    .initialize = initialize, .shutdown = shutdown, .input = input,
    .update = update, .render = render, .state_hash = state_hash,
    .state_json = state_json,
};

const mosaico_game_module_v1_t *mosaico_game_module_v1(void)
{
    return &s_module;
}
