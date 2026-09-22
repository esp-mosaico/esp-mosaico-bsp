// SPDX-License-Identifier: Apache-2.0
#include <stddef.h>
#include "mosaico_game.h"
#include "mosaico_raylib_fast.h"
#include "shooter_view.h"
#include "assets_ids.h"

static const Color C_CYAN={48,226,255,255};
static const Color C_PINK={255,60,166,255};
static const Color C_GOLD={255,190,62,255};
static const Color C_MINT={70,242,174,255};

static void draw_centered(const char *text,int y,int size,Color color)
{
    DrawText(text,(MOSAICO_GAME_WIDTH-MeasureText(text,size))/2,y,size,color);
}

static void draw_background(const shooter_game_t *game)
{
    static const Color sky[]={
        {2,5,24,255},{3,6,27,255},{4,7,30,255},{5,7,32,255},
        {7,8,34,255},{8,8,35,255},{9,8,36,255},{10,8,37,255}
    };
    for(int band=0;band<8;++band)DrawRectangle(0,band*60,480,60,sky[band]);
    DrawRectangle(0,104,184,72,(Color){12,17,51,255});
    DrawRectangle(296,218,184,92,(Color){17,10,47,255});
    DrawRectangle(0,390,480,90,(Color){4,8,25,255});
    for(int i=0;i<68;++i){
        int depth=i%3+1;
        int x=(i*101+(i%7)*23)%480;
        int y=(i*53+(int)(game->tick*(unsigned)depth))%430;
        Color color=depth==3?(Color){196,240,255,255}:
                    (depth==2?(Color){93,150,210,255}:(Color){46,76,133,255});
        if(depth==3&&i%4==0)DrawRectangle(x,y,2,3,color);
        else DrawPixel(x,y,color);
    }
    for(int i=0;i<8;++i){
        int x=i*72-(int)(game->tick%72U);
        DrawRectangle(x,450,30,2,(Color){25,54,91,255});
        DrawRectangle(x+12,462,48,1,(Color){16,34,67,255});
    }
}

static void draw_sprite(MosaicoAtlas atlas,mosaico_asset_id_t id,
                        float x,float y,float width,float height)
{
    const MosaicoSpriteFrame *frame=MosaicoAtlasGetFrame(atlas,id);
    if(!frame)return;
    DrawTexturePro(atlas.texture,frame->source,(Rectangle){x,y,width,height},
                   (Vector2){0,0},0,WHITE);
}

static void draw_ship(const shooter_game_t *game,MosaicoAtlas atlas,float x,float y)
{
    if(game->invulnerable&&(game->tick&2U))return;
    DrawEllipse((int)x+18,(int)y+35,25,12,(Color){7,43,76,255});
    draw_sprite(atlas,MOSAICO_ASSET_ID_PLAYER_SHIP,x-9,y-8,54,58);
}

static void draw_enemy(MosaicoAtlas atlas,const shooter_actor_t *enemy)
{
    static const mosaico_asset_id_t ids[]={
        MOSAICO_ASSET_ID_ENEMY_SCOUT,
        MOSAICO_ASSET_ID_ENEMY_ASSAULT,
        MOSAICO_ASSET_ID_ENEMY_TANK
    };
    static const Color colors[]={
        {255,105,62,255},{255,54,173,255},{72,238,170,255}
    };
    float size=enemy->kind==0?43.0f:(enemy->kind==1?49.0f:55.0f);
    DrawEllipse((int)enemy->x+14,(int)enemy->y+17,size*.43f,size*.31f,
                (Color){12,10,36,255});
    draw_sprite(atlas,ids[enemy->kind],enemy->x+(28.0f-size)*.5f,
                enemy->y+(28.0f-size)*.5f,size,size);
    if(enemy->kind>0&&enemy->hp){
        DrawRectangle((int)enemy->x+3,(int)enemy->y-8,24,2,(Color){32,26,55,255});
        DrawRectangle((int)enemy->x+3,(int)enemy->y-8,8*(int)enemy->hp,2,
                      colors[enemy->kind]);
    }
}

static void draw_projectiles(const shooter_game_t *game)
{
    for(size_t i=0;i<SHOOTER_MAX_BULLETS;++i)if(game->bullets[i].active){
        const shooter_actor_t *b=&game->bullets[i];
        int x=(int)b->x,y=(int)b->y;
        Color core=b->kind?C_GOLD:(Color){202,252,255,255};
        DrawRectangle(x-2,y+7,10,12,(Color){9,55,88,255});
        DrawRectangle(x,y,6,16,C_CYAN);
        DrawRectangle(x+2,y-3,2,18,core);
    }
}

static void draw_particles(const shooter_game_t *game)
{
    static const Color colors[]={
        {255,114,50,255},{255,62,174,255},{70,242,174,255},{255,194,62,255}
    };
    for(size_t i=0;i<SHOOTER_MAX_PARTICLES;++i)if(game->particles[i].active){
        const shooter_particle_t *p=&game->particles[i];
        int size=p->life>19?3:(p->life>9?2:1);
        DrawRectangle((int)p->x,(int)p->y,size,size,colors[p->color&3U]);
    }
}

static void draw_hud(const shooter_game_t *game)
{
    DrawRectangle(12,12,456,52,(Color){9,15,43,255});
    DrawRectangle(12,12,92,3,C_CYAN);
    DrawRectangle(376,61,92,3,C_PINK);
    DrawText("SCORE",25,20,12,(Color){94,151,193,255});
    DrawText(TextFormat("%07lu",(unsigned long)game->score),25,36,19,RAYWHITE);
    DrawText(TextFormat("SECTOR %02u",game->wave),200,20,13,(Color){105,169,206,255});
    if(game->combo){
        Color color=game->combo>=8?C_GOLD:C_PINK;
        DrawText(TextFormat("x%02u",game->combo),213,39,19,color);
    }else DrawText("LINK --",204,40,12,(Color){48,70,103,255});
    DrawText("HULL",390,20,12,(Color){117,150,184,255});
    for(unsigned i=0;i<3;++i){
        Color color=i<game->lives?C_MINT:(Color){40,35,67,255};
        int x=389+(int)i*20;
        DrawTriangle((Vector2){x+7,38},(Vector2){x,51},(Vector2){x+14,51},color);
    }
}

static void draw_corner_frame(Color accent)
{
    DrawRectangle(28,139,424,210,(Color){5,10,31,255});
    DrawRectangle(28,139,424,2,(Color){22,73,105,255});
    DrawRectangle(28,347,424,2,(Color){55,22,78,255});
    DrawRectangle(28,139,22,4,accent);DrawRectangle(28,139,4,22,accent);
    DrawRectangle(430,345,22,4,accent);DrawRectangle(448,327,4,22,accent);
}

static void draw_start_card(void)
{
    draw_corner_frame(C_CYAN);
    draw_centered("MOSAICO",164,20,(Color){97,160,202,255});
    draw_centered("STRIKE",190,52,RAYWHITE);
    DrawRectangle(107,250,266,2,(Color){38,86,121,255});
    draw_centered("ARCADE DEFENSE // S-31",266,15,C_CYAN);
    DrawRectangleRounded((Rectangle){126,302,228,42},0.18f,4,C_CYAN);
    draw_centered("TOUCH TO DEPLOY",314,17,(Color){3,22,35,255});
    draw_centered("DRAG TO STEER  /  AUTO FIRE",379,15,(Color){91,139,180,255});
}

static void draw_status_card(const shooter_game_t *game,const char *title,Color accent)
{
    DrawRectangle(0,0,480,480,(Color){3,6,22,255});
    draw_corner_frame(accent);
    draw_centered(title,181,39,RAYWHITE);
    if(game->phase==SHOOTER_GAME_OVER){
        draw_centered(TextFormat("SCORE %07lu",(unsigned long)game->score),242,19,accent);
        draw_centered(TextFormat("%u KILLS  /  MAX LINK x%u",game->kills,game->max_combo),274,14,
                      (Color){126,165,197,255});
        DrawRectangleRounded((Rectangle){126,302,228,42},0.18f,4,accent);
        draw_centered("TOUCH TO RETRY",314,17,RAYWHITE);
    }else{
        draw_centered("COMBAT SYSTEMS STANDBY",246,15,(Color){132,162,194,255});
        DrawRectangleRounded((Rectangle){126,302,228,42},0.18f,4,accent);
        draw_centered("TOUCH TO RESUME",314,17,(Color){28,18,5,255});
    }
}

void shooter_view_render(const shooter_game_t *game,MosaicoAtlas atlas)
{
    if(!game)return;
    BeginDrawing();
    draw_background(game);
    if(game->phase!=SHOOTER_START)draw_ship(game,atlas,game->player.x,game->player.y);
    draw_projectiles(game);
    for(size_t i=0;i<SHOOTER_MAX_ENEMIES;++i)if(game->enemies[i].active)
        draw_enemy(atlas,&game->enemies[i]);
    draw_particles(game);
    draw_hud(game);
    if(game->phase==SHOOTER_START)draw_start_card();
    else if(game->phase==SHOOTER_PAUSED)draw_status_card(game,"PAUSED",C_GOLD);
    else if(game->phase==SHOOTER_GAME_OVER)draw_status_card(game,"MISSION LOST",C_PINK);
    else if(game->wave_banner){
        int width=game->wave_banner>54?250:(int)game->wave_banner*4;
        DrawRectangle((480-width)/2,82,width,28,(Color){8,27,52,255});
        draw_centered(TextFormat("SECTOR %02u // ENGAGED",game->wave),89,14,C_CYAN);
    }
    if(game->hit_flash){
        Color color=game->hit_flash&1U?C_PINK:C_GOLD;
        DrawRectangle(0,0,480,5,color);DrawRectangle(0,475,480,5,color);
        DrawRectangle(0,0,5,480,color);DrawRectangle(475,0,5,480,color);
    }
    EndDrawing();
}
