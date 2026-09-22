// SPDX-License-Identifier: Apache-2.0
#include "sky_hop_view.h"

#include "assets_ids.h"
#include "mosaico_raylib_fast.h"

static void centered(const char *text, int y, int size, Color color)
{
    DrawText(text, (480 - MeasureText(text, size)) / 2, y, size, color);
}

static void draw_sprite(MosaicoAtlas atlas, mosaico_asset_id_t id, float x, float y,
                        float width, float height, bool flip)
{
    const MosaicoSpriteFrame *frame = MosaicoAtlasGetFrame(atlas, id);
    if (!frame) return;
    Rectangle source = frame->source;
    if (flip) source.width = -source.width;
    DrawTexturePro(atlas.texture, source, (Rectangle){x, y, width, height},
                   (Vector2){0, 0}, 0, WHITE);
}

static void draw_button(Rectangle bounds, const char *text, Color color, bool pressed)
{
    Color fill = pressed
        ? (Color){(unsigned char)(color.r / 2), (unsigned char)(color.g / 2),
                  (unsigned char)(color.b / 2), color.a}
        : color;
    DrawRectangle((int)bounds.x, (int)bounds.y, (int)bounds.width, (int)bounds.height, fill);
    if (!text) return;
    int width = MeasureText(text, 20);
    DrawText(text, (int)(bounds.x + (bounds.width - width) / 2),
             (int)(bounds.y + (bounds.height - 20) / 2), 20, RAYWHITE);
}

static void draw_overlay(const platform_game_t *game, float overlay_y)
{
    int panel_y = (int)overlay_y;
    DrawRectangle(42, panel_y, 396, 190, (Color){20, 39, 65, 255});
    const char *title = game->phase == PLATFORM_WON ? "ALL CLEAR!"
        : game->phase == PLATFORM_LEVEL_CLEAR ? "LEVEL CLEAR!"
        : game->phase == PLATFORM_GAME_OVER ? "TRY AGAIN"
        : game->phase == PLATFORM_PAUSED ? "PAUSED" : "SKY HOP";
    centered(title, panel_y + 38, 38, (Color){255, 220, 80, 255});
    const char *subtitle = game->phase == PLATFORM_LEVEL_CLEAR
        ? TextFormat("LEVEL %u COMPLETE", game->level + 1)
        : game->phase == PLATFORM_WON ? "FOUR LEVELS COMPLETE"
        : "AN ORIGINAL PLATFORM ADVENTURE";
    centered(subtitle, panel_y + 93, 16, RAYWHITE);
    centered(game->phase == PLATFORM_PAUSED ? "TOUCH TO RESUME"
             : game->phase == PLATFORM_LEVEL_CLEAR ? "TOUCH FOR NEXT LEVEL"
             : game->phase == PLATFORM_WON ? "TOUCH TO PLAY AGAIN"
             : "TOUCH TO START",
             panel_y + 143, 21, (Color){129, 224, 171, 255});
}

void sky_hop_overlay_sync(sky_hop_overlay_t *overlay, platform_phase_t phase)
{
    if (!overlay) return;
    bool want = phase != PLATFORM_PLAYING;
    if (want && !overlay->shown) {
        overlay->shown = true;
        overlay->y = -190.0f;
        mosaico_tween_start(&overlay->tween, -190.0f, 120.0f, 12, MOSAICO_EASE_OUT);
    } else if (!want && overlay->shown) {
        overlay->shown = false;
        overlay->y = -190.0f;
        overlay->tween.active = false;
    }
    if (overlay->shown && overlay->tween.active)
        overlay->y = mosaico_tween_tick(&overlay->tween);
}

void sky_hop_view_spawn_particles(mosaico_particle_pool_t *pool, float x, float y,
                                  Color color, unsigned count)
{
    static const float vx[] = {-2.4f, -1.6f, -.8f, .8f, 1.6f, 2.4f};
    for (unsigned i = 0; i < count; ++i) {
        uint32_t packed = (uint32_t)color.r | ((uint32_t)color.g << 8) |
            ((uint32_t)color.b << 16) | ((uint32_t)color.a << 24);
        (void)mosaico_particle_spawn(pool, (mosaico_particle_t){
            x, y, vx[i % 6], -2.8f - (float)(i % 3), .22f, packed,
            (uint16_t)(18 + i % 8), true});
    }
}

bool sky_hop_view_render(const sky_hop_view_t *view)
{
    if (!view || !view->game) return false;
    const platform_game_t *game = view->game;
    const int camera = (int)game->camera_x;
    Camera2D world_camera = {.offset={0, 0}, .target={game->camera_x, 0},
                             .rotation=0, .zoom=1};
    BeginDrawing();
    if (!MosaicoFastFrameAvailable()) {
        EndDrawing();
        return false;
    }
    static const Color sky[]={{92,190,236,255},{48,66,112,255},
        {142,208,239,255},{91,31,54,255}};
    static const Color haze[]={{170,224,245,255},{103,119,151,255},
        {215,242,250,255},{220,91,62,255}};
    ClearBackground(sky[game->level]);
    DrawRectangle(0,300,480,180,haze[game->level]);
    if(game->level==1){
        for(int i=0;i<7;++i){int x=(i*83+(int)game->tick*3)%520-20;
            DrawLine(x,80,x-18,118,(Color){151,205,235,255});}
    }else if(game->level==2){
        for(int i=0;i<18;++i){int x=(i*73-camera/2)%500,y=78+(i*37)%190;
            DrawRectangle(x,y,3,3,RAYWHITE);}
    }else if(game->level==3){
        for(int i=0;i<6;++i){int x=i*96-(camera/4)%96;
            DrawTriangle((Vector2){x,300},(Vector2){x+48,215},
                         (Vector2){x+96,300},(Color){75,28,48,255});}
    }
    for (int i = 0; i < 8; ++i) {
        int x = i * 210 - (camera / 3) % 210;
        DrawRectangle(x, 155 + (i % 2) * 28, 105, 18, (Color){235, 248, 250, 255});
        DrawRectangle(x + 20, 143 + (i % 2) * 28, 62, 24, (Color){235, 248, 250, 255});
    }

    BeginMode2D(world_camera);
    size_t block_count = 0;
    const platform_block_t *blocks = platform_game_blocks(game, &block_count);
    for (size_t i = 0; i < block_count; ++i) {
        int x = (int)blocks[i].x;
        if (x + (int)blocks[i].width < camera || x >= camera + 480) continue;
        for (int tile_x = x; tile_x < x + (int)blocks[i].width; tile_x += 48)
            draw_sprite(view->atlas, MOSAICO_ASSET_ID_TERRAIN_NATIVE, tile_x,
                        blocks[i].y - 2, 50, 50, false);
        if (blocks[i].y >= 390)
            DrawRectangle(x, (int)blocks[i].y + 48, (int)blocks[i].width, 42,
                          (Color){111, 73, 45, 255});
    }
    size_t spring_count=0;
    const platform_spring_t *springs=platform_game_springs(game,&spring_count);
    for(size_t i=0;i<spring_count;++i){
        DrawRectangle((int)springs[i].x,(int)springs[i].y-8,(int)springs[i].width,8,
                      (Color){238,73,105,255});
        for(int x=(int)springs[i].x+6;x<(int)(springs[i].x+springs[i].width);x+=12)
            DrawLine(x,(int)springs[i].y-7,x+6,(int)springs[i].y-1,
                     (Color){255,232,92,255});
    }
    float checkpoint=platform_game_checkpoint_x(game);
    DrawRectangle((int)checkpoint,302,5,88,game->checkpoint_active?
                  (Color){48,220,144,255}:(Color){98,116,136,255});
    DrawTriangle((Vector2){checkpoint+5,304},(Vector2){checkpoint+42,318},
                 (Vector2){checkpoint+5,334},game->checkpoint_active?
                 (Color){72,244,169,255}:(Color){139,156,170,255});
    for (size_t i = 0; i < game->coin_count; ++i) if (!game->coins[i].collected) {
        int x = (int)game->coins[i].x, y = (int)game->coins[i].y;
        unsigned pulse_index = (unsigned)((game->tick / 5 + i) % 3);
        float pulse = 25.0f + (float)pulse_index * 2.0f;
        static const mosaico_asset_id_t coin_frames[] = {
            MOSAICO_ASSET_ID_COIN_25, MOSAICO_ASSET_ID_COIN_27,
            MOSAICO_ASSET_ID_COIN_29};
        draw_sprite(view->atlas, coin_frames[pulse_index], x - pulse / 2, y - pulse / 2,
                    pulse, pulse, false);
    }
    for (size_t i = 0; i < game->enemy_count; ++i) if (game->enemies[i].active) {
        draw_sprite(view->atlas, MOSAICO_ASSET_ID_ENEMY_BEETLE_NATIVE,
                    game->enemies[i].x - 7, game->enemies[i].y - 12, 44, 44,
                    game->enemies[i].speed < 0);
    }
    mosaico_asset_id_t hero = !game->grounded ? MOSAICO_ASSET_ID_HERO_JUMP_NATIVE
        : (game->move_left || game->move_right)
        ? ((game->tick / MOSAICO_ANIMATION_HERO_RUN_FRAME_TICKS) & 1U
           ? MOSAICO_ASSET_ID_HERO_RUN_NATIVE : MOSAICO_ASSET_ID_HERO_IDLE_NATIVE)
        : MOSAICO_ASSET_ID_HERO_IDLE_NATIVE;
    draw_sprite(view->atlas, hero, game->player_x - 14, game->player_y - 22,
                58, 64, game->facing_left);
    const float finish_x = platform_game_finish_x(game);
    if (finish_x - camera < 500)
        draw_sprite(view->atlas, MOSAICO_ASSET_ID_FINISH_FLAG_NATIVE, finish_x - 12, 292,
                    70, 98, false);
    for (size_t i = 0; i < view->particle_count; ++i) {
        const mosaico_particle_t *particle = &view->particles[i];
        if (!particle->active) continue;
        const uint32_t packed = particle->color;
        const Color color = {(uint8_t)packed, (uint8_t)(packed >> 8),
                             (uint8_t)(packed >> 16), (uint8_t)(packed >> 24)};
        DrawCircle((int)particle->x, (int)particle->y, 2 + (particle->life % 3), color);
    }
    EndMode2D();

    DrawRectangle(0, 0, 480, 42, (Color){22, 42, 68, 230});
    DrawText(TextFormat("SCORE %04u", game->score), 14, 11, 20, RAYWHITE);
    DrawText(TextFormat("L%u/%u", game->level + 1, PLATFORM_LEVEL_COUNT),
             181, 11, 18, (Color){129, 224, 171, 255});
    DrawText(TextFormat("BEST %04u", view->best_score), 250, 13, 14,
             (Color){255, 220, 80, 255});
    DrawText(TextFormat("LIFE %u", game->lives), 365, 11, 20, RAYWHITE);
    DrawRectangle(438, 4, 36, 32, (Color){52, 77, 104, 255});
    DrawRectangle(449, 11, 4, 18, RAYWHITE);
    DrawRectangle(459, 11, 4, 18, RAYWHITE);
    draw_button((Rectangle){8, 408, 138, 64}, "LEFT", (Color){25, 43, 65, 210},
                game->move_left);
    draw_button((Rectangle){154, 408, 138, 64}, "RIGHT", (Color){25, 43, 65, 210},
                game->move_right);
    draw_button((Rectangle){300, 408, 172, 64}, "JUMP", (Color){226, 95, 63, 230},
                game->jump_held);
    if (game->phase != PLATFORM_PLAYING)
        draw_overlay(game, view->overlay_y);
    EndDrawing();
    return true;
}
