// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "mosaico_game_2d.h"
#include "mosaico_game_fx.h"
#include "platform_game.h"

#define SKY_HOP_PARTICLE_COUNT 24

typedef struct {
    mosaico_tween_t tween;
    float y;
    bool shown;
} sky_hop_overlay_t;

typedef struct {
    const platform_game_t *game;
    MosaicoAtlas atlas;
    uint16_t best_score;
    float overlay_y;
    const mosaico_particle_t *particles;
    size_t particle_count;
} sky_hop_view_t;

void sky_hop_overlay_sync(sky_hop_overlay_t *overlay, platform_phase_t phase);
void sky_hop_view_spawn_particles(mosaico_particle_pool_t *pool, float x, float y,
                                  Color color, unsigned count);
bool sky_hop_view_render(const sky_hop_view_t *view);
