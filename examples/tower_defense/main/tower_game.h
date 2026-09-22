// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TOWER_MAX_ENEMIES 48
#define TOWER_MAX_PROJECTILES 64
#define TOWER_PAD_COUNT 9
#define TOWER_MAX_PATH_POINTS 16

typedef enum {
    TOWER_START,
    TOWER_PLAYING,
    TOWER_PAUSED,
    TOWER_GAME_OVER,
} tower_phase_t;

typedef enum {
    TOWER_PULSE,
    TOWER_RAPID,
    TOWER_FROST,
    TOWER_TYPE_COUNT,
} tower_type_t;

typedef enum {
    TOWER_FEEDBACK_NONE,
    TOWER_FEEDBACK_BUILT,
    TOWER_FEEDBACK_UPGRADED,
    TOWER_FEEDBACK_REPLACED,
    TOWER_FEEDBACK_MAX_LEVEL,
    TOWER_FEEDBACK_NO_CREDITS,
} tower_feedback_t;

typedef struct {
    float x, y;
    float hp, max_hp;
    float speed;
    uint16_t waypoint;
    uint8_t kind;
    uint8_t slow_ticks;
    bool active;
} tower_enemy_t;

typedef struct {
    float x, y, vx, vy;
    uint16_t target;
    uint8_t damage;
    uint8_t kind;
    uint8_t ttl;
    bool active;
} tower_projectile_t;

typedef struct {
    int16_t x, y;
    uint16_t cooldown;
    uint8_t type;
    uint8_t level;
    bool occupied;
} tower_slot_t;

typedef struct {
    tower_phase_t phase;
    tower_enemy_t enemies[TOWER_MAX_ENEMIES];
    tower_projectile_t projectiles[TOWER_MAX_PROJECTILES];
    tower_slot_t towers[TOWER_PAD_COUNT];
    uint32_t tick, rng, score, kills, shots;
    uint16_t credits, wave, wave_spawned, wave_total;
    uint16_t spawn_cooldown, intermission;
    uint16_t feedback_value;
    uint8_t base_hp, selected_type, feedback, feedback_ticks;
    bool pointer_down;
} tower_game_t;

typedef struct {
    uint8_t path_count;
    float path[TOWER_MAX_PATH_POINTS][2];
    float pads[TOWER_PAD_COUNT][2];
} tower_level_t;

bool tower_game_configure_level(const tower_level_t *level);
void tower_game_reset(tower_game_t *game, uint32_t seed);
void tower_game_set_pointer(tower_game_t *game, float x, float y, bool pressed);
void tower_game_update(tower_game_t *game);
uint32_t tower_game_state_hash(const tower_game_t *game);
