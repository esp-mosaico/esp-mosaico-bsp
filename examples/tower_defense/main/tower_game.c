// SPDX-License-Identifier: Apache-2.0
#include "tower_game.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

typedef struct { float x, y; } point_t;

static point_t s_path[TOWER_MAX_PATH_POINTS] = {
    {-18, 104}, {126, 104}, {126, 190}, {354, 190},
    {354, 278}, {94, 278}, {94, 360}, {498, 360},
};

static uint8_t s_path_count = 8;
static point_t s_pads[TOWER_PAD_COUNT] = {
    {58, 154}, {195, 130}, {286, 130}, {414, 218}, {286, 322},
    {202, 242}, {42, 322}, {176, 356}, {410, 318},
};

bool tower_game_configure_level(const tower_level_t *level)
{
    if (!level || level->path_count < 2 ||
        level->path_count > TOWER_MAX_PATH_POINTS) return false;
    s_path_count = level->path_count;
    for (uint8_t i = 0; i < s_path_count; ++i)
        s_path[i] = (point_t){level->path[i][0], level->path[i][1]};
    for (uint8_t i = 0; i < TOWER_PAD_COUNT; ++i)
        s_pads[i] = (point_t){level->pads[i][0], level->pads[i][1]};
    return true;
}

static const uint16_t COST[TOWER_TYPE_COUNT] = {70, 95, 120};
static const uint16_t UPGRADE_COST[TOWER_TYPE_COUNT] = {65, 80, 95};
static const float RANGE[TOWER_TYPE_COUNT] = {96.0f, 78.0f, 112.0f};
static const uint16_t FIRE_DELAY[TOWER_TYPE_COUNT] = {23, 8, 31};
static const uint8_t DAMAGE[TOWER_TYPE_COUNT] = {12, 5, 8};

static uint32_t next_random(tower_game_t *game)
{
    uint32_t x = game->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return game->rng = x ? x : 0x6d2b79f5U;
}

static float distance_sq(float ax, float ay, float bx, float by)
{
    float dx = ax - bx, dy = ay - by;
    return dx*dx + dy*dy;
}

void tower_game_reset(tower_game_t *game, uint32_t seed)
{
    memset(game, 0, sizeof(*game));
    game->phase = TOWER_START;
    game->rng = seed ? seed : 1;
    game->credits = 190;
    game->base_hp = 20;
    game->selected_type = TOWER_PULSE;
    game->wave = 1;
    game->wave_total = 9;
    game->intermission = 45;
    for (size_t i = 0; i < TOWER_PAD_COUNT; ++i) {
        game->towers[i].x = (int16_t)s_pads[i].x;
        game->towers[i].y = (int16_t)s_pads[i].y;
    }
}

static bool point_in(float x, float y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static uint16_t tower_investment(const tower_slot_t *tower)
{
    uint16_t value = COST[tower->type];
    for (uint8_t level = 1; level < tower->level; ++level)
        value += UPGRADE_COST[tower->type] * level;
    return value;
}

static void feedback(tower_game_t *game, tower_feedback_t value, uint16_t amount)
{
    game->feedback = (uint8_t)value;
    game->feedback_value = amount;
    game->feedback_ticks = 45;
}

static void handle_tap(tower_game_t *game, float x, float y)
{
    if (game->phase == TOWER_START || game->phase == TOWER_GAME_OVER) {
        uint32_t seed = game->rng;
        tower_game_reset(game, seed);
        game->phase = TOWER_PLAYING;
        return;
    }
    if (point_in(x, y, 418, 12, 50, 40)) {
        game->phase = game->phase == TOWER_PAUSED ? TOWER_PLAYING : TOWER_PAUSED;
        return;
    }
    if (game->phase != TOWER_PLAYING) return;
    for (uint8_t type = 0; type < TOWER_TYPE_COUNT; ++type) {
        if (point_in(x, y, 12 + type*154, 407, 146, 61)) {
            game->selected_type = type;
            return;
        }
    }
    for (size_t i = 0; i < TOWER_PAD_COUNT; ++i) {
        tower_slot_t *tower = &game->towers[i];
        if (distance_sq(x, y, tower->x, tower->y) > 24.0f*24.0f) continue;
        if (!tower->occupied) {
            uint16_t cost = COST[game->selected_type];
            if (game->credits >= cost) {
                game->credits -= cost;
                tower->occupied = true;
                tower->type = game->selected_type;
                tower->level = 1;
                feedback(game, TOWER_FEEDBACK_BUILT, cost);
            } else feedback(game, TOWER_FEEDBACK_NO_CREDITS, cost);
        } else if (tower->type != game->selected_type) {
            uint16_t cost = COST[game->selected_type];
            uint16_t refund = tower_investment(tower) / 2U;
            if (refund > cost) refund = cost;
            if ((uint32_t)game->credits + refund >= cost) {
                game->credits = (uint16_t)(game->credits + refund - cost);
                tower->type = game->selected_type;
                tower->level = 1;
                tower->cooldown = 0;
                feedback(game, TOWER_FEEDBACK_REPLACED,
                         cost > refund ? cost - refund : 0);
            } else feedback(game, TOWER_FEEDBACK_NO_CREDITS,
                            cost > refund ? cost - refund : 0);
        } else if (tower->level < 3) {
            uint16_t cost = UPGRADE_COST[tower->type] * tower->level;
            if (game->credits >= cost) {
                game->credits -= cost;
                ++tower->level;
                feedback(game, TOWER_FEEDBACK_UPGRADED, cost);
            } else feedback(game, TOWER_FEEDBACK_NO_CREDITS, cost);
        } else feedback(game, TOWER_FEEDBACK_MAX_LEVEL, 0);
        return;
    }
}

void tower_game_set_pointer(tower_game_t *game, float x, float y, bool pressed)
{
    if (pressed && !game->pointer_down) handle_tap(game, x, y);
    game->pointer_down = pressed;
}

static void spawn_enemy(tower_game_t *game)
{
    for (size_t i = 0; i < TOWER_MAX_ENEMIES; ++i) {
        tower_enemy_t *enemy = &game->enemies[i];
        if (enemy->active) continue;
        uint32_t value = next_random(game);
        uint8_t kind = game->wave < 2 ? 0 : (uint8_t)(value % 3U);
        float hp = 22.0f + game->wave*5.0f;
        float speed = 1.15f + game->wave*0.025f;
        if (kind == 1) { hp *= 2.1f; speed *= 0.62f; }
        if (kind == 2) { hp *= 0.68f; speed *= 1.65f; }
        *enemy = (tower_enemy_t){.x=s_path[0].x, .y=s_path[0].y,
            .hp=hp, .max_hp=hp, .speed=speed, .waypoint=1,
            .kind=kind, .active=true};
        ++game->wave_spawned;
        return;
    }
}

static int choose_target(const tower_game_t *game, const tower_slot_t *tower)
{
    float range = RANGE[tower->type] + (tower->level-1)*12.0f;
    float best_progress = -1.0f;
    int best = -1;
    for (size_t i = 0; i < TOWER_MAX_ENEMIES; ++i) {
        const tower_enemy_t *enemy = &game->enemies[i];
        if (!enemy->active || distance_sq(tower->x, tower->y, enemy->x, enemy->y) > range*range)
            continue;
        float progress = enemy->waypoint*1000.0f -
            distance_sq(enemy->x, enemy->y, s_path[enemy->waypoint].x, s_path[enemy->waypoint].y);
        if (progress > best_progress) { best_progress = progress; best = (int)i; }
    }
    return best;
}

static void fire_projectile(tower_game_t *game, const tower_slot_t *tower, int target)
{
    tower_enemy_t *enemy = &game->enemies[target];
    for (size_t i = 0; i < TOWER_MAX_PROJECTILES; ++i) {
        tower_projectile_t *p = &game->projectiles[i];
        if (p->active) continue;
        float dx = enemy->x-tower->x, dy = enemy->y-tower->y;
        float length = sqrtf(dx*dx+dy*dy);
        if (length < 1.0f) length = 1.0f;
        float speed = tower->type == TOWER_RAPID ? 8.5f : 6.2f;
        *p = (tower_projectile_t){.x=tower->x, .y=tower->y,
            .vx=dx/length*speed, .vy=dy/length*speed, .target=(uint16_t)target,
            .damage=(uint8_t)(DAMAGE[tower->type] + (tower->level-1)*4),
            .kind=tower->type, .ttl=30, .active=true};
        ++game->shots;
        return;
    }
}

static void update_towers(tower_game_t *game)
{
    for (size_t i = 0; i < TOWER_PAD_COUNT; ++i) {
        tower_slot_t *tower = &game->towers[i];
        if (!tower->occupied) continue;
        if (tower->cooldown) { --tower->cooldown; continue; }
        int target = choose_target(game, tower);
        if (target >= 0) {
            fire_projectile(game, tower, target);
            uint16_t delay = FIRE_DELAY[tower->type];
            tower->cooldown = delay > (tower->level-1)*2 ? delay-(tower->level-1)*2 : 2;
        }
    }
}

static void destroy_enemy(tower_game_t *game, tower_enemy_t *enemy)
{
    enemy->active = false;
    game->credits += (uint16_t)(8 + enemy->kind*5 + game->wave);
    game->score += (uint32_t)(25 + enemy->kind*20) * game->wave;
    ++game->kills;
}

static void update_projectiles(tower_game_t *game)
{
    for (size_t i = 0; i < TOWER_MAX_PROJECTILES; ++i) {
        tower_projectile_t *p = &game->projectiles[i];
        if (!p->active) continue;
        if (!p->ttl-- || p->target >= TOWER_MAX_ENEMIES || !game->enemies[p->target].active) {
            p->active = false;
            continue;
        }
        tower_enemy_t *enemy = &game->enemies[p->target];
        float dx = enemy->x-p->x, dy = enemy->y-p->y;
        float d2 = dx*dx+dy*dy;
        if (d2 < 10.0f*10.0f) {
            enemy->hp -= p->damage;
            if (p->kind == TOWER_FROST) enemy->slow_ticks = 42;
            p->active = false;
            if (enemy->hp <= 0) destroy_enemy(game, enemy);
            continue;
        }
        float length = sqrtf(d2);
        float speed = p->kind == TOWER_RAPID ? 8.5f : 6.2f;
        p->vx=dx/length*speed; p->vy=dy/length*speed;
        p->x += p->vx; p->y += p->vy;
    }
}

static bool any_enemy(const tower_game_t *game)
{
    for (size_t i = 0; i < TOWER_MAX_ENEMIES; ++i)
        if (game->enemies[i].active) return true;
    return false;
}

static void update_enemies(tower_game_t *game)
{
    for (size_t i = 0; i < TOWER_MAX_ENEMIES; ++i) {
        tower_enemy_t *enemy = &game->enemies[i];
        if (!enemy->active) continue;
        point_t target = s_path[enemy->waypoint];
        float dx=target.x-enemy->x, dy=target.y-enemy->y;
        float length=sqrtf(dx*dx+dy*dy);
        float speed=enemy->speed*(enemy->slow_ticks ? 0.52f : 1.0f);
        if (enemy->slow_ticks) --enemy->slow_ticks;
        if (length <= speed+0.1f) {
            enemy->x=target.x; enemy->y=target.y;
            if (++enemy->waypoint >= s_path_count) {
                enemy->active=false;
                uint8_t damage=(uint8_t)(enemy->kind==1 ? 3 : 1);
                game->base_hp = game->base_hp > damage ? game->base_hp-damage : 0;
                if (!game->base_hp) game->phase=TOWER_GAME_OVER;
                continue;
            }
        } else {
            enemy->x += dx/length*speed;
            enemy->y += dy/length*speed;
        }
    }
}

void tower_game_update(tower_game_t *game)
{
    if (game->phase != TOWER_PLAYING) return;
    ++game->tick;
    if (game->feedback_ticks && !--game->feedback_ticks) {
        game->feedback = TOWER_FEEDBACK_NONE;
        game->feedback_value = 0;
    }
    if (game->intermission) --game->intermission;
    else if (game->wave_spawned < game->wave_total) {
        if (game->spawn_cooldown) --game->spawn_cooldown;
        else { spawn_enemy(game); game->spawn_cooldown=20; }
    }
    update_enemies(game);
    update_towers(game);
    update_projectiles(game);
    if (game->wave_spawned >= game->wave_total && !any_enemy(game)) {
        ++game->wave;
        game->credits += (uint16_t)(35 + game->wave*4);
        game->wave_spawned=0;
        game->wave_total=(uint16_t)(7 + game->wave*3);
        game->intermission=75;
    }
}

uint32_t tower_game_state_hash(const tower_game_t *game)
{
    const uint8_t *bytes=(const uint8_t *)game;
    uint32_t hash=2166136261U;
    for(size_t i=0;i<sizeof(*game);++i) hash=(hash^bytes[i])*16777619U;
    return hash;
}
