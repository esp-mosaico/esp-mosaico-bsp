// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SHOOTER_MAX_BULLETS 64
#define SHOOTER_MAX_ENEMIES 32
#define SHOOTER_MAX_PARTICLES 72

typedef enum { SHOOTER_START, SHOOTER_PLAYING, SHOOTER_PAUSED,
               SHOOTER_GAME_OVER } shooter_phase_t;
typedef struct {
    float x, y, vx, vy;
    uint8_t kind, hp;
    bool active;
} shooter_actor_t;
typedef struct {
    float x, y, vx, vy;
    uint8_t life, color;
    bool active;
} shooter_particle_t;
typedef struct {
    shooter_phase_t phase;
    shooter_actor_t player;
    shooter_actor_t bullets[SHOOTER_MAX_BULLETS];
    shooter_actor_t enemies[SHOOTER_MAX_ENEMIES];
    shooter_particle_t particles[SHOOTER_MAX_PARTICLES];
    uint32_t score, tick, rng, shots_fired;
    uint16_t fire_cooldown, spawn_cooldown, combo_timer;
    uint16_t kills;
    uint8_t lives, combo, max_combo, wave;
    uint8_t invulnerable, hit_flash, wave_banner;
} shooter_game_t;

void shooter_game_reset(shooter_game_t *game, uint32_t seed);
void shooter_game_set_pointer(shooter_game_t *game, float x, float y, bool pressed);
void shooter_game_move(shooter_game_t *game, float dx, float dy);
void shooter_game_toggle_pause(shooter_game_t *game);
void shooter_game_update(shooter_game_t *game);
uint32_t shooter_game_state_hash(const shooter_game_t *game);
