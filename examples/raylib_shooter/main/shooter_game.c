// SPDX-License-Identifier: Apache-2.0
#include "shooter_game.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

static uint32_t next_random(shooter_game_t *game)
{
    uint32_t x = game->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return game->rng = x ? x : 0x6d2b79f5U;
}

static bool overlaps(const shooter_actor_t *a, float aw, float ah,
                     const shooter_actor_t *b, float bw, float bh)
{
    return a->x < b->x + bw && a->x + aw > b->x &&
           a->y < b->y + bh && a->y + ah > b->y;
}

void shooter_game_reset(shooter_game_t *game, uint32_t seed)
{
    memset(game, 0, sizeof(*game));
    game->phase = SHOOTER_START;
    game->player = (shooter_actor_t){.x=222, .y=420, .active=true};
    game->lives = 3;
    game->wave = 1;
    game->wave_banner = 60;
    game->rng = seed ? seed : 1;
}

void shooter_game_set_pointer(shooter_game_t *game, float x, float y, bool pressed)
{
    if (!pressed) return;
    if (game->phase == SHOOTER_START || game->phase == SHOOTER_GAME_OVER) {
        uint32_t seed = game->rng;
        shooter_game_reset(game, seed);
        game->phase = SHOOTER_PLAYING;
    }
    if (game->phase == SHOOTER_PLAYING) {
        game->player.x = fmaxf(0, fminf(444, x - 18));
        game->player.y = fmaxf(280, fminf(438, y - 18));
    }
}

void shooter_game_move(shooter_game_t *game, float dx, float dy)
{
    if (!game || game->phase != SHOOTER_PLAYING) return;
    game->player.x = fmaxf(0, fminf(444, game->player.x + dx));
    game->player.y = fmaxf(280, fminf(438, game->player.y + dy));
}

void shooter_game_toggle_pause(shooter_game_t *game)
{
    if (game->phase == SHOOTER_PLAYING) game->phase = SHOOTER_PAUSED;
    else if (game->phase == SHOOTER_PAUSED) game->phase = SHOOTER_PLAYING;
}

static void spawn_bullet(shooter_game_t *game)
{
    const bool twin=game->combo>=4;
    const unsigned count=twin?2U:1U;
    const float offsets[2]={9.0f,25.0f};
    for(unsigned shot=0;shot<count;++shot){
        for (size_t i=0; i<SHOOTER_MAX_BULLETS; ++i) if (!game->bullets[i].active) {
            game->bullets[i] = (shooter_actor_t){
                .x=game->player.x+(twin?offsets[shot]:17.0f),
                .y=game->player.y-13, .vy=-7.4f,
                .kind=(uint8_t)(game->combo>=8), .active=true};
            ++game->shots_fired;
            break;
        }
    }
}

static bool spawn_enemy_at(shooter_game_t *game,float x,uint8_t kind)
{
    for (size_t i=0; i<SHOOTER_MAX_ENEMIES; ++i) if (!game->enemies[i].active) {
        uint32_t value=next_random(game);
        const float speed=1.05f+(float)game->wave*0.075f;
        game->enemies[i]=(shooter_actor_t){.x=x, .y=-36,
            .vx=kind==1?((value>>9)&1?1.15f:-1.15f):0,
            .vy=speed+(kind==0?0.45f:(kind==2?-0.18f:0.12f)),
            .kind=kind, .hp=(uint8_t)(kind+1U), .active=true};
        return true;
    }
    return false;
}

static void spawn_wave_enemy(shooter_game_t *game)
{
    uint32_t value=next_random(game);
    uint8_t kind=(uint8_t)((value>>5)%3U);
    float x=14.0f+(float)(value%418U);
    (void)spawn_enemy_at(game,x,kind);
    if(game->wave>=3 && (value&7U)==0U)
        (void)spawn_enemy_at(game,454.0f-x,0);
}

static void emit_particles(shooter_game_t *game,float x,float y,uint8_t color,unsigned count)
{
    for(unsigned made=0;made<count;++made){
        for(size_t i=0;i<SHOOTER_MAX_PARTICLES;++i)if(!game->particles[i].active){
            uint32_t value=next_random(game);
            float angle=(float)(value&255U)*0.024543693f;
            float speed=0.9f+(float)((value>>8)&15U)*0.14f;
            game->particles[i]=(shooter_particle_t){
                .x=x,.y=y,.vx=cosf(angle)*speed,.vy=sinf(angle)*speed,
                .life=(uint8_t)(14U+((value>>16)&15U)),.color=color,.active=true};
            break;
        }
    }
}

void shooter_game_update(shooter_game_t *game)
{
    if (game->phase != SHOOTER_PLAYING) return;
    ++game->tick;
    uint8_t next_wave=(uint8_t)(1U+game->tick/450U);
    if(next_wave>9)next_wave=9;
    if(next_wave!=game->wave){game->wave=next_wave;game->wave_banner=72;}
    if(game->wave_banner)--game->wave_banner;
    if(game->hit_flash)--game->hit_flash;
    if(game->invulnerable)--game->invulnerable;
    if(game->combo_timer){
        if(!--game->combo_timer)game->combo=0;
    }
    if (!game->fire_cooldown) {
        spawn_bullet(game);
        game->fire_cooldown=(uint16_t)(game->combo>=8?6:8);
    }
    else --game->fire_cooldown;
    if (!game->spawn_cooldown) {
        spawn_wave_enemy(game);
        unsigned pace=game->wave<7?(unsigned)(7-game->wave):0U;
        game->spawn_cooldown=(uint16_t)(10U+pace+next_random(game)%13U);
    }
    else --game->spawn_cooldown;
    for (size_t i=0; i<SHOOTER_MAX_BULLETS; ++i) {
        shooter_actor_t *b=&game->bullets[i]; if (!b->active) continue;
        b->y+=b->vy; if (b->y < -12) b->active=false;
    }
    for(size_t i=0;i<SHOOTER_MAX_PARTICLES;++i){
        shooter_particle_t *p=&game->particles[i];if(!p->active)continue;
        p->x+=p->vx;p->y+=p->vy;p->vy+=0.025f;
        if(p->life)--p->life;else p->active=false;
    }
    for (size_t i=0; i<SHOOTER_MAX_ENEMIES; ++i) {
        shooter_actor_t *e=&game->enemies[i]; if (!e->active) continue;
        e->x+=e->vx; e->y+=e->vy;
        if (e->x<0 || e->x>450) e->vx=-e->vx;
        if (e->y>490 || (!game->invulnerable&&overlaps(e,28,28,&game->player,36,36))) {
            e->active=false;
            emit_particles(game,e->x+14,e->y+14,3,12);
            game->combo=0;game->combo_timer=0;game->hit_flash=8;game->invulnerable=45;
            if (game->lives && --game->lives==0) game->phase=SHOOTER_GAME_OVER;
            continue;
        }
        for (size_t j=0; j<SHOOTER_MAX_BULLETS; ++j) {
            shooter_actor_t *b=&game->bullets[j];
            if (b->active && overlaps(e,28,28,b,6,12)) {
                b->active=false;
                if(e->hp)--e->hp;
                emit_particles(game,b->x,e->y+18,e->kind,3);
                if(!e->hp){
                    e->active=false;
                    if(game->combo<99)++game->combo;
                    if(game->combo>game->max_combo)game->max_combo=game->combo;
                    game->combo_timer=90;++game->kills;
                    uint32_t multiplier=1U+(uint32_t)game->combo/4U;
                    game->score+=(uint32_t)(15U+e->kind*20U)*multiplier;
                    emit_particles(game,e->x+14,e->y+14,e->kind,12U+e->kind*4U);
                }
                break;
            }
        }
    }
}

uint32_t shooter_game_state_hash(const shooter_game_t *game)
{
    const uint8_t *bytes=(const uint8_t *)game; uint32_t hash=2166136261U;
    for (size_t i=0; i<sizeof(*game); ++i) hash=(hash^bytes[i])*16777619U;
    return hash;
}
