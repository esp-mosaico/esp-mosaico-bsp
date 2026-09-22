// SPDX-License-Identifier: Apache-2.0
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include "host_asset_runtime.h"
#include "mosaico_game_2d.h"
#include "mosaico_game_module.h"
#include "mosaico_raylib_fast.h"
#include "shooter_game.h"
#include "shooter_view.h"

typedef struct {
    shooter_game_t game;
    MosaicoAtlas atlas;
    bool paused, left, right;
    float imu_x, imu_y;
} shooter_module_state_t;

static int initialize(void *value,const char *asset_root)
{
    shooter_module_state_t *state=value;
    mosaico_host_assets_set_root(asset_root);
    state->atlas=LoadMosaicoAtlas("shooter.atlas");
    if(!state->atlas.texture.id)return -1;
    shooter_game_reset(&state->game,0x4d4f5341U);
    InitWindow(480,480,"Mosaico Strike");
    SetTargetFPS(30);
    return 0;
}

static void shutdown(void *value)
{
    shooter_module_state_t *state=value;
    if(state)UnloadMosaicoAtlas(state->atlas);
}

static void input(void *value,const mosaico_host_input_v1_t *event)
{
    shooter_module_state_t *state=value;
    if(!state||!event)return;
    if(event->type==MOSAICO_HOST_INPUT_POINTER)
        shooter_game_set_pointer(&state->game,(float)event->x,(float)event->y,event->pressed);
    else if(event->type==MOSAICO_HOST_INPUT_ACTION){
        if(event->code==0)state->left=event->pressed;
        else if(event->code==1)state->right=event->pressed;
        else if(event->pressed&&event->code==3)shooter_game_toggle_pause(&state->game);
        else if(event->pressed&&event->code==4)
            shooter_game_set_pointer(&state->game,240,420,true);
    }else if(event->type==MOSAICO_HOST_INPUT_IMU){
        state->imu_x=event->value_x;
        state->imu_y=event->value_y;
    }else if(event->type==MOSAICO_HOST_INPUT_CONTROL){
        if(event->code==MOSAICO_HOST_CONTROL_PAUSE)state->paused=true;
        else if(event->code==MOSAICO_HOST_CONTROL_RESUME)state->paused=false;
        else if(event->code==MOSAICO_HOST_CONTROL_RESET)
            shooter_game_reset(&state->game,0x4d4f5341U);
    }
}

static void update(void *value)
{
    shooter_module_state_t *state=value;
    if(!state||state->paused)return;
    if(state->game.phase==SHOOTER_PLAYING&&state->left!=state->right){
        float direction=state->right?7.0f:-7.0f;
        shooter_game_set_pointer(&state->game,state->game.player.x+18.0f+direction,
                                 state->game.player.y+18.0f,true);
    }
    float ix=fabsf(state->imu_x)>0.10f?state->imu_x:0.0f;
    float iy=fabsf(state->imu_y)>0.10f?state->imu_y:0.0f;
    shooter_game_move(&state->game,ix*10.0f,iy*10.0f);
    shooter_game_update(&state->game);
}

static int render(void *value)
{
    shooter_module_state_t *state=value;
    shooter_view_render(&state->game,state->atlas);
    return 0;
}

static uint32_t state_hash(const void *value)
{ return shooter_game_state_hash(&((const shooter_module_state_t *)value)->game); }

static int state_json(const void *value,char *output,size_t capacity)
{
    const shooter_module_state_t *state=value;
    static const char *phases[]={"start","playing","paused","game_over"};
    const shooter_game_t *game=&state->game;
    const char *phase=(unsigned)game->phase<4?phases[game->phase]:"unknown";
    return snprintf(output,capacity,
        "{\"score\":%lu,\"lives\":%u,\"phase\":\"%s\",\"wave\":%u,"
        "\"combo\":%u,\"kills\":%u,\"player_x\":%d,\"player_y\":%d,"
        "\"tick\":%lu,\"state_hash\":\"%08lx\"}",
        (unsigned long)game->score,game->lives,phase,game->wave,game->combo,
        game->kills,(int)game->player.x,(int)game->player.y,(unsigned long)game->tick,
        (unsigned long)shooter_game_state_hash(game));
}

static const mosaico_game_module_v1_t s_module={
    .descriptor={MOSAICO_HOST_GAME_ABI_V1,"raylib_shooter","Mosaico Strike",480,480,30,1},
    .state_size=sizeof(shooter_module_state_t),
    .initialize=initialize,.shutdown=shutdown,.input=input,.update=update,.render=render,
    .state_hash=state_hash,.state_json=state_json,
};

const mosaico_game_module_v1_t *mosaico_game_module_v1(void)
{ return &s_module; }
