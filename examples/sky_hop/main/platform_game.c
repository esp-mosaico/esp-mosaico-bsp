// SPDX-License-Identifier: Apache-2.0
#include "platform_game.h"

#include <string.h>

#define PLAYER_W 28.0f
#define PLAYER_H 36.0f
#define FLOOR_Y 390.0f
#define JUMP_VELOCITY (-10.5f)
#define COYOTE_FRAMES 6U
#define JUMP_BUFFER_FRAMES 8U

typedef struct {
    const platform_block_t *blocks;
    size_t block_count;
    const platform_coin_t *coins;
    uint8_t coin_count;
    const platform_enemy_t *enemies;
    uint8_t enemy_count;
    const platform_spring_t *springs;
    uint8_t spring_count;
    float checkpoint_x;
    float start_x;
    float finish_x;
    float world_width;
} platform_level_t;

static const platform_spring_t LEVEL_1_SPRINGS[] = {
    {664, 275, 48}, {1738, 255, 48},
};
static const platform_spring_t LEVEL_2_SPRINGS[] = {
    {818, 230, 48}, {1088, FLOOR_Y, 48}, {1598, 235, 48},
};
static const platform_spring_t LEVEL_3_SPRINGS[] = {
    {608, 245, 48}, {1308, 225, 48}, {2098, 220, 48},
};
static const platform_spring_t LEVEL_4_SPRINGS[] = {
    {558, 235, 48}, {1598, 210, 48}, {2378, 200, 48},
};

static const platform_block_t LEVEL_1_BLOCKS[] = {
    {0, FLOOR_Y, 2160, 90},
    {160, 330, 100, 20}, {310, 280, 90, 20}, {470, 330, 110, 20},
    {640, 275, 95, 20}, {820, 320, 100, 20}, {1000, 250, 110, 20},
    {1180, 305, 95, 20}, {1360, 260, 100, 20}, {1540, 315, 110, 20},
    {1720, 255, 100, 20}, {1900, 300, 120, 20},
};
static const platform_coin_t LEVEL_1_COINS[] = {
    {200, 295, false}, {350, 245, false}, {510, 295, false}, {680, 240, false},
    {860, 285, false}, {1040, 215, false}, {1220, 270, false}, {1400, 225, false},
    {1580, 280, false}, {1760, 220, false}, {1945, 265, false}, {2080, 350, false},
};
static const platform_enemy_t LEVEL_1_ENEMIES[] = {
    {560, 362, 500, 700, 1.1f, true}, {930, 362, 850, 1080, -1.2f, true},
    {1320, 362, 1240, 1480, 1.3f, true}, {1780, 362, 1680, 1960, -1.35f, true},
};

static const platform_block_t LEVEL_2_BLOCKS[] = {
    {0, FLOOR_Y, 380, 90}, {480, FLOOR_Y, 360, 90}, {960, FLOOR_Y, 340, 90},
    {1420, FLOOR_Y, 420, 90}, {1980, FLOOR_Y, 420, 90},
    {140, 325, 100, 20}, {300, 270, 90, 20}, {450, 320, 85, 20},
    {620, 275, 100, 20}, {800, 230, 95, 20}, {980, 300, 90, 20},
    {1160, 250, 105, 20}, {1360, 285, 95, 20}, {1580, 235, 110, 20},
    {1780, 300, 100, 20}, {1980, 245, 105, 20}, {2180, 310, 110, 20},
};
static const platform_coin_t LEVEL_2_COINS[] = {
    {180, 290, false}, {340, 235, false}, {490, 285, false}, {660, 240, false},
    {840, 195, false}, {1020, 265, false}, {1200, 215, false}, {1400, 250, false},
    {1620, 200, false}, {1820, 265, false}, {2020, 210, false}, {2220, 275, false},
};
static const platform_enemy_t LEVEL_2_ENEMIES[] = {
    {250, 362, 210, 360, 1.4f, true}, {680, 362, 540, 820, -1.5f, true},
    {1120, 362, 1000, 1280, 1.6f, true}, {1620, 362, 1480, 1780, -1.7f, true},
    {2100, 362, 2020, 2320, 1.75f, true},
};

static const platform_block_t LEVEL_3_BLOCKS[] = {
    {0, FLOOR_Y, 280, 90}, {380, FLOOR_Y, 260, 90}, {760, FLOOR_Y, 250, 90},
    {1140, FLOOR_Y, 260, 90}, {1540, FLOOR_Y, 280, 90}, {1960, FLOOR_Y, 520, 90},
    {110, 315, 80, 20}, {250, 260, 80, 20}, {430, 305, 80, 20},
    {590, 245, 85, 20}, {760, 300, 80, 20}, {930, 230, 85, 20},
    {1110, 285, 80, 20}, {1290, 225, 85, 20}, {1470, 280, 80, 20},
    {1660, 230, 90, 20}, {1860, 275, 85, 20}, {2080, 220, 95, 20},
    {2280, 265, 100, 20},
};
static const platform_coin_t LEVEL_3_COINS[] = {
    {145, 280, false}, {285, 225, false}, {465, 270, false}, {625, 210, false},
    {795, 265, false}, {965, 195, false}, {1145, 250, false}, {1325, 190, false},
    {1505, 245, false}, {1695, 195, false}, {1895, 240, false}, {2115, 185, false},
    {2320, 230, false},
};
static const platform_enemy_t LEVEL_3_ENEMIES[] = {
    {200, 362, 170, 260, 1.7f, true}, {520, 362, 420, 620, -1.8f, true},
    {900, 362, 800, 990, 1.9f, true}, {1280, 362, 1180, 1380, -2.0f, true},
    {1700, 362, 1600, 1800, 2.05f, true}, {2140, 362, 2020, 2360, -2.1f, true},
};

static const platform_block_t LEVEL_4_BLOCKS[] = {
    {0, FLOOR_Y, 240, 90}, {360, FLOOR_Y, 220, 90}, {720, FLOOR_Y, 200, 90},
    {1080, FLOOR_Y, 220, 90}, {1460, FLOOR_Y, 200, 90}, {1820, FLOOR_Y, 240, 90},
    {2220, FLOOR_Y, 540, 90},
    {90, 320, 75, 20}, {220, 265, 80, 20}, {400, 300, 75, 20},
    {540, 235, 85, 20}, {700, 280, 75, 20}, {860, 220, 85, 20},
    {1040, 270, 80, 20}, {1220, 215, 85, 20}, {1400, 265, 80, 20},
    {1580, 210, 90, 20}, {1760, 255, 80, 20}, {1960, 205, 90, 20},
    {2160, 250, 85, 20}, {2360, 200, 100, 20}, {2540, 290, 110, 20},
};
static const platform_coin_t LEVEL_4_COINS[] = {
    {120, 285, false}, {250, 230, false}, {430, 265, false}, {575, 200, false},
    {735, 245, false}, {895, 185, false}, {1075, 235, false}, {1255, 180, false},
    {1435, 230, false}, {1615, 175, false}, {1795, 220, false}, {1995, 170, false},
    {2195, 215, false}, {2395, 165, false}, {2580, 255, false},
};
static const platform_enemy_t LEVEL_4_ENEMIES[] = {
    {180, 362, 140, 230, 1.85f, true}, {480, 362, 400, 560, -1.95f, true},
    {840, 362, 760, 900, 2.05f, true}, {1220, 362, 1140, 1280, -2.1f, true},
    {1600, 362, 1520, 1640, 2.15f, true}, {1960, 362, 1880, 2040, -2.2f, true},
    {2360, 362, 2260, 2520, 2.25f, true},
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define LEVEL(blocks_, coins_, enemies_, springs_, checkpoint_, finish_, width_) { \
    blocks_, ARRAY_LEN(blocks_), coins_, ARRAY_LEN(coins_), \
    enemies_, ARRAY_LEN(enemies_), springs_, ARRAY_LEN(springs_), checkpoint_, \
    42.0f, finish_, width_ }
static const platform_level_t LEVELS[PLATFORM_LEVEL_COUNT] = {
    LEVEL(LEVEL_1_BLOCKS, LEVEL_1_COINS, LEVEL_1_ENEMIES, LEVEL_1_SPRINGS, 1080, 2080, 2160),
    LEVEL(LEVEL_2_BLOCKS, LEVEL_2_COINS, LEVEL_2_ENEMIES, LEVEL_2_SPRINGS, 1160, 2320, 2400),
    LEVEL(LEVEL_3_BLOCKS, LEVEL_3_COINS, LEVEL_3_ENEMIES, LEVEL_3_SPRINGS, 1200, 2400, 2480),
    LEVEL(LEVEL_4_BLOCKS, LEVEL_4_COINS, LEVEL_4_ENEMIES, LEVEL_4_SPRINGS, 1500, 2680, 2760),
};

static bool overlap(float ax, float ay, float aw, float ah,
                    float bx, float by, float bw, float bh)
{
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

static const platform_level_t *current_level(const platform_game_t *game)
{
    return &LEVELS[game->level < PLATFORM_LEVEL_COUNT ? game->level : 0];
}

const platform_block_t *platform_game_blocks(const platform_game_t *game,
                                              size_t *count)
{
    const platform_level_t *level = current_level(game);
    if (count) *count = level->block_count;
    return level->blocks;
}

float platform_game_world_width(const platform_game_t *game)
{
    return current_level(game)->world_width;
}

float platform_game_finish_x(const platform_game_t *game)
{
    return current_level(game)->finish_x;
}

float platform_game_checkpoint_x(const platform_game_t *game)
{ return current_level(game)->checkpoint_x; }

const platform_spring_t *platform_game_springs(const platform_game_t *game,size_t *count)
{
    const platform_level_t *level=current_level(game);
    if(count)*count=level->spring_count;
    return level->springs;
}

static void load_level(platform_game_t *game, uint8_t level_index)
{
    const platform_level_t *level = &LEVELS[level_index];
    game->level = level_index;
    game->player_x = level->start_x;
    game->player_y = FLOOR_Y - PLAYER_H;
    game->velocity_x = game->velocity_y = 0;
    game->camera_x = 0;
    game->respawn_x = level->start_x;
    game->checkpoint_active = false;
    game->move_left = game->move_right = game->jump_held = game->facing_left = false;
    game->grounded = true;
    game->coyote_frames = COYOTE_FRAMES;
    game->jump_buffer = 0;
    memset(game->pointers, 0, sizeof(game->pointers));
    game->coin_count = level->coin_count;
    game->enemy_count = level->enemy_count;
    memset(game->coins, 0, sizeof(game->coins));
    memset(game->enemies, 0, sizeof(game->enemies));
    memcpy(game->coins, level->coins, level->coin_count * sizeof(level->coins[0]));
    memcpy(game->enemies, level->enemies,
           level->enemy_count * sizeof(level->enemies[0]));
}

void platform_game_reset(platform_game_t *game)
{
    memset(game, 0, sizeof(*game));
    game->phase = PLATFORM_START;
    game->lives = 3;
    load_level(game, 0);
}

static void set_phase(platform_game_t *game, platform_phase_t phase)
{
    game->phase = phase;
    game->phase_tick = 0;
}

void platform_game_set_action(platform_game_t *game, platform_action_t action,
                              bool pressed)
{
    if (action == PLATFORM_ACTION_RESTART && pressed) {
        if (game->phase == PLATFORM_LEVEL_CLEAR) {
            load_level(game, game->level + 1);
        } else if (game->phase == PLATFORM_START) {
            load_level(game, game->level);
        } else {
            platform_game_reset(game);
        }
        set_phase(game, PLATFORM_PLAYING);
        return;
    }
    if (action == PLATFORM_ACTION_PAUSE && pressed) {
        if (game->phase == PLATFORM_PLAYING) {
            game->move_left = game->move_right = game->jump_held = false;
            set_phase(game, PLATFORM_PAUSED);
        }
        else if (game->phase == PLATFORM_PAUSED) set_phase(game, PLATFORM_PLAYING);
        return;
    }
    if (game->phase != PLATFORM_PLAYING) return;
    if (action == PLATFORM_ACTION_LEFT) {
        game->move_left = pressed;
        if (pressed) game->facing_left = true;
    }
    if (action == PLATFORM_ACTION_RIGHT) {
        game->move_right = pressed;
        if (pressed) game->facing_left = false;
    }
    if (action == PLATFORM_ACTION_JUMP) {
        if (pressed && !game->jump_held)
            game->jump_buffer = JUMP_BUFFER_FRAMES;
        game->jump_held = pressed;
    }
}

static void apply_jump(platform_game_t *game)
{
    if (!game->jump_buffer) return;
    if (game->coyote_frames) {
        game->velocity_y = JUMP_VELOCITY;
        game->grounded = false;
        game->coyote_frames = 0;
        game->jump_buffer = 0;
        return;
    }
    --game->jump_buffer;
}

static void sync_pointer_actions(platform_game_t *game)
{
    bool left = false, right = false, jump = false;
    for (size_t i = 0; i < PLATFORM_POINTER_COUNT; ++i) {
        const platform_pointer_t *pointer = &game->pointers[i];
        if (!pointer->active || pointer->y < 360.0f) continue;
        if (pointer->x < 150.0f) left = true;
        else if (pointer->x < 300.0f) right = true;
        else jump = true;
    }
    platform_game_set_action(game, PLATFORM_ACTION_LEFT, left);
    platform_game_set_action(game, PLATFORM_ACTION_RIGHT, right);
    platform_game_set_action(game, PLATFORM_ACTION_JUMP, jump);
}

void platform_game_set_pointer(platform_game_t *game, float x, float y, bool pressed,
                              int32_t track_id)
{
    if (pressed && game->phase == PLATFORM_PAUSED) {
        platform_game_set_action(game, PLATFORM_ACTION_PAUSE, true);
        return;
    }
    if (pressed && game->phase != PLATFORM_PLAYING) {
        platform_game_set_action(game, PLATFORM_ACTION_RESTART, true);
        if (game->phase != PLATFORM_PLAYING) return;
    }
    if (pressed && y < 54 && x > 420) {
        platform_game_set_action(game, PLATFORM_ACTION_PAUSE, true);
        return;
    }
    platform_pointer_t *slot = NULL;
    for (size_t i = 0; i < PLATFORM_POINTER_COUNT; ++i) {
        if (game->pointers[i].active && game->pointers[i].track_id == track_id) {
            slot = &game->pointers[i];
            break;
        }
    }
    if (!slot && pressed) {
        for (size_t i = 0; i < PLATFORM_POINTER_COUNT; ++i) {
            if (!game->pointers[i].active) {
                slot = &game->pointers[i];
                break;
            }
        }
    }
    if (!slot) return;
    slot->track_id = track_id;
    slot->x = x;
    slot->y = y;
    slot->active = pressed;
    if (game->phase == PLATFORM_PLAYING) sync_pointer_actions(game);
}

static void lose_life(platform_game_t *game)
{
    if (game->lives) --game->lives;
    if (!game->lives) {
        set_phase(game, PLATFORM_GAME_OVER);
        return;
    }
    game->player_x = game->respawn_x;
    game->player_y = FLOOR_Y - PLAYER_H;
    game->velocity_x = game->velocity_y = 0;
    game->camera_x = 0;
    game->grounded = true;
    game->coyote_frames = COYOTE_FRAMES;
    game->jump_buffer = 0;
}

void platform_game_update(platform_game_t *game)
{
    ++game->phase_tick;
    if (game->phase != PLATFORM_PLAYING) return;
    const platform_level_t *level = current_level(game);
    ++game->tick;
    if (game->move_left) game->facing_left = true;
    else if (game->move_right) game->facing_left = false;
    apply_jump(game);
    float desired = game->move_left ? -4.0f : game->move_right ? 4.0f : 0.0f;
    game->velocity_x += (desired - game->velocity_x) * (desired ? 0.35f : 0.55f);

    float next_x = game->player_x + game->velocity_x;
    if (next_x < 0) next_x = 0;
    if (next_x > level->world_width - PLAYER_W) next_x = level->world_width - PLAYER_W;
    for (size_t i = 0; i < level->block_count; ++i) {
        const platform_block_t *b = &level->blocks[i];
        if (!overlap(next_x, game->player_y, PLAYER_W, PLAYER_H,
                     b->x, b->y, b->width, b->height)) continue;
        if (game->velocity_x > 0) next_x = b->x - PLAYER_W;
        else if (game->velocity_x < 0) next_x = b->x + b->width;
        game->velocity_x = 0;
    }
    game->player_x = next_x;

    float old_bottom = game->player_y + PLAYER_H;
    game->velocity_y += 0.62f;
    if (game->velocity_y > 13) game->velocity_y = 13;
    float next_y = game->player_y + game->velocity_y;
    game->grounded = false;
    for (size_t i = 0; i < level->block_count; ++i) {
        const platform_block_t *b = &level->blocks[i];
        if (!overlap(game->player_x, next_y, PLAYER_W, PLAYER_H,
                     b->x, b->y, b->width, b->height)) continue;
        if (game->velocity_y >= 0 && old_bottom <= b->y + 2) {
            next_y = b->y - PLAYER_H;
            game->velocity_y = 0;
            game->grounded = true;
            game->coyote_frames = COYOTE_FRAMES;
        } else if (game->velocity_y < 0 && game->player_y >= b->y + b->height - 2) {
            next_y = b->y + b->height;
            game->velocity_y = 0;
        }
    }
    game->player_y = next_y;
    if(game->grounded){
        for(size_t i=0;i<level->spring_count;++i){
            const platform_spring_t *spring=&level->springs[i];
            if(game->player_x+PLAYER_W>spring->x&&game->player_x<spring->x+spring->width&&
               game->player_y+PLAYER_H>=spring->y-1&&game->player_y+PLAYER_H<=spring->y+2){
                game->velocity_y=-14.0f;game->grounded=false;game->coyote_frames=0;
                break;
            }
        }
    }
    if (game->grounded) game->coyote_frames = COYOTE_FRAMES;
    else if (game->coyote_frames) --game->coyote_frames;
    apply_jump(game);

    for (size_t i = 0; i < game->coin_count; ++i) {
        platform_coin_t *coin = &game->coins[i];
        if (!coin->collected && overlap(game->player_x, game->player_y,
                PLAYER_W, PLAYER_H, coin->x - 8, coin->y - 8, 16, 16)) {
            coin->collected = true;
            game->score += 100;
        }
    }
    if(!game->checkpoint_active&&game->player_x>=level->checkpoint_x){
        game->checkpoint_active=true;
        game->respawn_x=level->checkpoint_x;
        game->score+=200;
    }
    for (size_t i = 0; i < game->enemy_count; ++i) {
        platform_enemy_t *enemy = &game->enemies[i];
        if (!enemy->active) continue;
        enemy->x += enemy->speed;
        if (enemy->x < enemy->left || enemy->x > enemy->right) enemy->speed = -enemy->speed;
        if (!overlap(game->player_x, game->player_y, PLAYER_W, PLAYER_H,
                     enemy->x, enemy->y, 28, 28)) continue;
        if (game->velocity_y > 0 && game->player_y + PLAYER_H < enemy->y + 16) {
            enemy->active = false;
            game->velocity_y = -7.5f;
            game->score += 250;
        } else {
            lose_life(game);
            return;
        }
    }
    if (game->player_y > 500) lose_life(game);
    if (game->player_x > level->finish_x)
        set_phase(game, game->level + 1 < PLATFORM_LEVEL_COUNT ?
                  PLATFORM_LEVEL_CLEAR : PLATFORM_WON);
    float target_camera = game->player_x - 190;
    if (target_camera < 0) target_camera = 0;
    if (target_camera > level->world_width - 480) target_camera = level->world_width - 480;
    game->camera_x += (target_camera - game->camera_x) * 0.15f;
}

uint32_t platform_game_state_hash(const platform_game_t *game)
{
    const uint8_t *bytes = (const uint8_t *)game;
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < sizeof(*game); ++i) hash = (hash ^ bytes[i]) * 16777619U;
    return hash;
}
