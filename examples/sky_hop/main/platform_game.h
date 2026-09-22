// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PLATFORM_LEVEL_COUNT 4
#define PLATFORM_COIN_COUNT 16
#define PLATFORM_ENEMY_COUNT 8
#define PLATFORM_POINTER_COUNT 2

typedef enum {
    PLATFORM_START,
    PLATFORM_PLAYING,
    PLATFORM_PAUSED,
    PLATFORM_LEVEL_CLEAR,
    PLATFORM_WON,
    PLATFORM_GAME_OVER,
} platform_phase_t;

typedef enum {
    PLATFORM_ACTION_LEFT,
    PLATFORM_ACTION_RIGHT,
    PLATFORM_ACTION_JUMP,
    PLATFORM_ACTION_PAUSE,
    PLATFORM_ACTION_RESTART,
} platform_action_t;

typedef struct {
    float x, y;
    bool collected;
} platform_coin_t;

typedef struct {
    float x, y, left, right, speed;
    bool active;
} platform_enemy_t;

typedef struct {
    bool active;
    int32_t track_id;
    float x, y;
} platform_pointer_t;

typedef struct {
    platform_phase_t phase;
    float player_x, player_y, velocity_x, velocity_y;
    float camera_x;
    float respawn_x;
    bool move_left, move_right, jump_held, grounded, facing_left;
    bool checkpoint_active;
    uint8_t coyote_frames;
    uint8_t jump_buffer;
    uint32_t tick;
    uint32_t phase_tick;
    uint16_t score;
    uint8_t lives;
    uint8_t level;
    uint8_t coin_count;
    uint8_t enemy_count;
    platform_coin_t coins[PLATFORM_COIN_COUNT];
    platform_enemy_t enemies[PLATFORM_ENEMY_COUNT];
    platform_pointer_t pointers[PLATFORM_POINTER_COUNT];
} platform_game_t;

typedef struct { float x, y, width, height; } platform_block_t;
typedef struct { float x, y, width; } platform_spring_t;

void platform_game_reset(platform_game_t *game);
void platform_game_set_action(platform_game_t *game, platform_action_t action,
                              bool pressed);
void platform_game_set_pointer(platform_game_t *game, float x, float y, bool pressed,
                              int32_t track_id);
void platform_game_update(platform_game_t *game);
const platform_block_t *platform_game_blocks(const platform_game_t *game,
                                              size_t *count);
float platform_game_world_width(const platform_game_t *game);
float platform_game_finish_x(const platform_game_t *game);
float platform_game_checkpoint_x(const platform_game_t *game);
const platform_spring_t *platform_game_springs(const platform_game_t *game,
                                               size_t *count);
uint32_t platform_game_state_hash(const platform_game_t *game);
