// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "mosaico_game_2d.h"
#include "mosaico_game_tilemap.h"
#include "tower_game.h"

#define TOWER_EFFECT_COUNT 12

typedef struct {
    float x, y;
    uint8_t ticks;
    bool active;
} tower_effect_t;

typedef struct {
    const tower_game_t *game;
    MosaicoAtlas atlas;
    MosaicoTilemap map;
    const tower_effect_t *effects;
    size_t effect_count;
} tower_view_t;

bool tower_view_apply_map(tower_game_t *game, MosaicoTilemap map);
void tower_view_add_explosion(tower_effect_t *effects, size_t count, float x, float y);
void tower_view_tick_effects(tower_effect_t *effects, size_t count);
void tower_view_render(const tower_view_t *view);
