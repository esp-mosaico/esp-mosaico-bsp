// SPDX-License-Identifier: Apache-2.0
#include "tower_view.h"

#include <stdio.h>
#include "assets_ids.h"
#include "mosaico_game_assets.h"
#include "mosaico_raylib_fast.h"

static const Color C_BG = {5, 10, 20, 255};
static const Color C_GRASS = {12, 30, 35, 255};
static const Color C_GOLD = {255, 193, 61, 255};
static const Color C_CYAN = {42, 224, 231, 255};
static const Color C_RED = {255, 73, 105, 255};
static const Color C_BLUE = {110, 145, 255, 255};

static void draw_sprite(MosaicoAtlas atlas, const char *name, float x, float y,
                        float size, float rotation, Color tint)
{
    const MosaicoSpriteFrame *frame = MosaicoAtlasGetFrame(atlas, mosaico_game_asset_id(name));
    if (!frame) return;
    DrawTexturePro(atlas.texture, frame->source,
        (Rectangle){x, y, size, size}, (Vector2){size * .5f, size * .5f}, rotation, tint);
}

static void draw_sprite_id(MosaicoAtlas atlas, mosaico_asset_id_t id, float x, float y,
                           float size, Color tint)
{
    const MosaicoSpriteFrame *frame = MosaicoAtlasGetFrame(atlas, id);
    if (frame) DrawTexturePro(atlas.texture, frame->source,
        (Rectangle){x, y, size, size}, (Vector2){size * .5f, size * .5f}, 0, tint);
}

static void draw_centered(const char *text, int y, int size, Color color)
{
    DrawText(text, (480 - MeasureText(text, size)) / 2, y, size, color);
}

static void draw_pad(const tower_view_t *view, const tower_slot_t *tower)
{
    int x = tower->x, y = tower->y;
    Color main = tower->type == TOWER_PULSE ? C_CYAN
        : tower->type == TOWER_RAPID ? C_GOLD : C_BLUE;
    draw_sprite(view->atlas, "build_pad", x, y, 54, 0,
                tower->occupied ? (Color){150, 150, 150, 255} : WHITE);
    if (!tower->occupied) {
        Color plus = (view->game->tick / 15U) % 2 ? C_CYAN : (Color){76, 139, 139, 255};
        DrawRectangle(x - 8, y - 2, 16, 4, plus);
        DrawRectangle(x - 2, y - 8, 4, 16, plus);
        return;
    }
    const char *names[] = {"tower_pulse", "tower_rapid", "tower_frost"};
    draw_sprite(view->atlas, names[tower->type], x, y - 4, 58, 0, WHITE);
    DrawRectangle(x - 13, y + 14, 26, 7, (Color){4, 15, 21, 255});
    for (int i = 0; i < 3; ++i) DrawRectangle(x - 10 + i * 7, y + 16, 5, 3,
        i < tower->level ? main : (Color){39, 67, 70, 255});
}

static void draw_enemy(MosaicoAtlas atlas, const tower_enemy_t *enemy)
{
    int x = (int)enemy->x, y = (int)enemy->y;
    const char *names[] = {"enemy_drone", "enemy_brute", "enemy_scout"};
    int size = enemy->kind == 1 ? 48 : enemy->kind == 2 ? 34 : 40;
    draw_sprite(atlas, names[enemy->kind], x, y, size, 0,
                enemy->slow_ticks ? (Color){145, 205, 255, 255} : WHITE);
    int hpw = (int)((enemy->hp / enemy->max_hp) * 28.0f);
    DrawRectangle(x - 15, y - size / 2 - 9, 30, 5, (Color){5, 13, 18, 255});
    DrawRectangle(x - 14, y - size / 2 - 8, hpw, 3,
        enemy->hp / enemy->max_hp > 0.35f ? (Color){73, 236, 135, 255} : C_RED);
}

static void draw_projectiles(const tower_view_t *view)
{
    for (size_t i = 0; i < TOWER_MAX_PROJECTILES; ++i) {
        const tower_projectile_t *projectile = &view->game->projectiles[i];
        if (!projectile->active) continue;
        const char *names[] = {"projectile_pulse", "projectile_rapid", "projectile_frost"};
        draw_sprite(view->atlas, names[projectile->kind], projectile->x, projectile->y,
                    projectile->kind == TOWER_RAPID ? 14 : 18, 0, WHITE);
    }
}

static void draw_effects(const tower_view_t *view)
{
    for (size_t i = 0; i < view->effect_count; ++i) {
        const tower_effect_t *effect = &view->effects[i];
        if (!effect->active) continue;
        mosaico_asset_id_t frame = MosaicoAnimationFrameAt(
            MOSAICO_ANIMATION_EXPLOSION_FRAMES,
            MOSAICO_ANIMATION_EXPLOSION_FRAME_COUNT,
            MOSAICO_ANIMATION_EXPLOSION_FRAME_TICKS, 12U - effect->ticks, false);
        draw_sprite_id(view->atlas, frame, effect->x, effect->y,
                       54 + (12 - effect->ticks) * 2, WHITE);
    }
}

static void draw_core(const tower_view_t *view)
{
    float pulse = 62 + (float)((view->game->tick / 5U) % 4);
    draw_sprite(view->atlas, "reactor_core", 467, 354, pulse, 0,
                view->game->base_hp > 6 ? WHITE : (Color){255, 110, 110, 255});
}

static void draw_hud(const tower_game_t *game)
{
    DrawRectangle(0, 0, 480, 69, C_BG);
    DrawRectangle(0, 65, 480, 4, (Color){13, 72, 83, 255});
    DrawRectangle(0, 65, 110, 2, C_CYAN);
    DrawRectangle(12, 9, 118, 47, (Color){10, 27, 39, 255});
    DrawRectangleLines(12, 9, 118, 47, (Color){35, 100, 112, 255});
    DrawRectangle(18, 15, 4, 35, C_CYAN);
    DrawText(TextFormat("WAVE %02u", game->wave), 29, 14, 17, RAYWHITE);
    DrawText(TextFormat("CRED %03u", game->credits), 29, 36, 13, C_GOLD);
    DrawRectangle(140, 9, 132, 47, (Color){10, 27, 39, 255});
    DrawRectangleLines(140, 9, 132, 47, (Color){35, 77, 91, 255});
    DrawText("TACTICAL SCORE", 151, 15, 11, (Color){107, 151, 160, 255});
    DrawText(TextFormat("%06lu", (unsigned long)game->score), 151, 34, 17, RAYWHITE);
    DrawRectangle(282, 9, 125, 47, (Color){10, 27, 39, 255});
    DrawRectangleLines(282, 9, 125, 47, (Color){35, 77, 91, 255});
    DrawText(TextFormat("CORE %02u", game->base_hp), 293, 15, 14,
        game->base_hp > 6 ? (Color){102, 233, 139, 255} : C_RED);
    DrawRectangle(293, 39, 102, 7, (Color){28, 48, 53, 255});
    DrawRectangle(295, 41, game->base_hp * 5, 3,
        game->base_hp > 6 ? (Color){78, 228, 134, 255} : C_RED);
    DrawRectangle(418, 9, 50, 47, (Color){13, 36, 47, 255});
    DrawRectangleLines(418, 9, 50, 47, C_CYAN);
    DrawRectangle(432, 21, 7, 22, (Color){181, 246, 245, 255});
    DrawRectangle(447, 21, 7, 22, (Color){181, 246, 245, 255});
}

static void draw_shop_card(const tower_game_t *game, int type, int x, const char *name,
                           const char *role, const char *cost, Color color)
{
    bool selected = game->selected_type == type;
    DrawRectangle(x + 3, 405, 143, 67, (Color){2, 8, 14, 255});
    DrawRectangle(x, 402, 146, 67, selected ? (Color){19, 47, 56, 255} : (Color){10, 25, 32, 255});
    DrawRectangleLines(x, 402, 146, 67, selected ? color : (Color){39, 72, 78, 255});
    DrawRectangle(x, 402, selected ? 45 : 18, 3, color);
    DrawRectangle(x + 8, 411, 35, 42, (Color){5, 16, 24, 255});
    DrawRectangleLines(x + 8, 411, 35, 42, (Color){44, 81, 88, 255});
    DrawRectangle(x + 13, 419, 25, 25, color);
    if (type == TOWER_PULSE) DrawRectangle(x + 23, 413, 5, 24, RAYWHITE);
    else if (type == TOWER_RAPID) {
        DrawRectangle(x + 17, 413, 5, 24, RAYWHITE);
        DrawRectangle(x + 29, 413, 5, 24, RAYWHITE);
    } else DrawTriangle((Vector2){x + 25, 411}, (Vector2){x + 14, 442},
                        (Vector2){x + 36, 442}, RAYWHITE);
    DrawText(name, x + 50, 411, 14, RAYWHITE);
    DrawText(role, x + 50, 431, 10, (Color){101, 148, 155, 255});
    DrawText(cost, x + 96, 449, 13, C_GOLD);
    if (selected) {
        DrawRectangle(x + 7, 460, 62, 2, color);
        DrawText("READY", x + 13, 449, 10, color);
    }
}

static void draw_shop(const tower_game_t *game)
{
    DrawRectangle(0, 392, 480, 88, (Color){3, 10, 16, 255});
    DrawRectangle(0, 392, 480, 2, (Color){31, 106, 112, 255});
    draw_shop_card(game, TOWER_PULSE, 8, "PULSE", "BALANCED", "$70", C_CYAN);
    draw_shop_card(game, TOWER_RAPID, 164, "RAPID", "FIRE RATE", "$95", C_GOLD);
    draw_shop_card(game, TOWER_FROST, 320, "FROST", "SLOW FIELD", "$120", C_BLUE);
}

static void draw_feedback(const tower_game_t *game)
{
    if (!game->feedback_ticks) return;
    const char *message = NULL;
    Color color = C_CYAN;
    switch ((tower_feedback_t)game->feedback) {
    case TOWER_FEEDBACK_BUILT: message = TextFormat("TOWER BUILT  -$%u", game->feedback_value); break;
    case TOWER_FEEDBACK_UPGRADED: message = TextFormat("UPGRADED  -$%u", game->feedback_value); break;
    case TOWER_FEEDBACK_REPLACED: message = TextFormat("TOWER CHANGED  -$%u", game->feedback_value); break;
    case TOWER_FEEDBACK_MAX_LEVEL: message = "MAX LEVEL"; color = C_GOLD; break;
    case TOWER_FEEDBACK_NO_CREDITS: message = TextFormat("NEED $%u", game->feedback_value); color = C_RED; break;
    default: return;
    }
    int width = MeasureText(message, 12) + 20;
    DrawRectangle((480 - width) / 2, 372, width, 20, (Color){3, 12, 18, 235});
    DrawRectangleLines((480 - width) / 2, 372, width, 20, color);
    draw_centered(message, 376, 12, color);
}

static void draw_overlay(const char *title, const char *line, const char *action, Color accent)
{
    DrawRectangle(29, 125, 422, 204, (Color){1, 7, 13, 210});
    DrawRectangle(35, 119, 410, 202, (Color){5, 16, 27, 246});
    DrawRectangleLines(35, 119, 410, 202, (Color){31, 81, 92, 255});
    DrawRectangle(35, 119, 410, 5, accent);
    DrawRectangle(35, 119, 36, 12, accent);
    DrawRectangle(409, 119, 36, 12, accent);
    DrawText("MOSAICO // DEFENSE PROTOCOL", 52, 140, 11, (Color){94, 142, 153, 255});
    draw_centered(title, 166, 32, RAYWHITE);
    DrawRectangle(103, 207, 274, 2, (Color){30, 78, 88, 255});
    draw_centered(line, 224, 16, (Color){171, 211, 211, 255});
    DrawRectangle(107, 268, 266, 43, (Color){2, 11, 18, 255});
    DrawRectangle(111, 264, 258, 43, accent);
    DrawRectangle(118, 271, 244, 29, (Color){8, 27, 34, 255});
    draw_centered(action, 279, 15, RAYWHITE);
}

bool tower_view_apply_map(tower_game_t *game, MosaicoTilemap map)
{
    (void)game;
    const Vector2 *points = NULL;
    size_t count = MosaicoTilemapPathPoints(map, &points);
    tower_level_t level = {.path_count = (uint8_t)count};
    for (size_t i = 0; i < count && i < TOWER_MAX_PATH_POINTS; ++i) {
        level.path[i][0] = points[i].x;
        level.path[i][1] = points[i].y;
    }
    for (unsigned i = 0; i < TOWER_PAD_COUNT; ++i) {
        char name[12];
        snprintf(name, sizeof(name), "pad_%u", i);
        MosaicoMapObject object = {0};
        if (MosaicoTilemapFindObject(map, mosaico_game_asset_id(name), &object)) {
            level.pads[i][0] = object.x;
            level.pads[i][1] = object.y;
        }
    }
    return tower_game_configure_level(&level);
}

void tower_view_add_explosion(tower_effect_t *effects, size_t count, float x, float y)
{
    for (size_t i = 0; i < count; ++i) {
        if (effects[i].active) continue;
        effects[i] = (tower_effect_t){.x = x, .y = y, .ticks = 12, .active = true};
        return;
    }
}

void tower_view_tick_effects(tower_effect_t *effects, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (effects[i].active && !--effects[i].ticks) effects[i].active = false;
    }
}

void tower_view_render(const tower_view_t *view)
{
    if (!view || !view->game) return;
    const tower_game_t *game = view->game;
    BeginDrawing();
    if (!MosaicoFastFrameAvailable()) {
        EndDrawing();
        return;
    }
    ClearBackground(C_GRASS);
    DrawMosaicoTilemapLayer(view->map, 0, (Rectangle){0, 0, 480, 320});
    draw_core(view);
    for (size_t i = 0; i < TOWER_PAD_COUNT; ++i) draw_pad(view, &game->towers[i]);
    for (size_t i = 0; i < TOWER_MAX_ENEMIES; ++i)
        if (game->enemies[i].active) draw_enemy(view->atlas, &game->enemies[i]);
    draw_projectiles(view);
    draw_effects(view);
    draw_hud(game);
    draw_shop(game);
    if (game->phase == TOWER_START)
        draw_overlay("CIRCUIT KEEP", "BUILD TOWERS // DEFEND THE CORE",
                     "TOUCH TO START", C_CYAN);
    else if (game->phase == TOWER_PAUSED)
        draw_overlay("PAUSED", "TACTICAL GRID ON HOLD",
                     "TOUCH PAUSE TO RESUME", C_GOLD);
    else if (game->phase == TOWER_GAME_OVER)
        draw_overlay("CORE LOST",
                     TextFormat("WAVE %u // SCORE %05lu", game->wave,
                                (unsigned long)game->score),
                     "TOUCH TO RETRY", C_RED);
    else if (game->intermission) {
        DrawRectangle(164, 374, 152, 18, (Color){4, 15, 22, 255});
        DrawRectangleLines(164, 374, 152, 18, (Color){77, 103, 100, 255});
        draw_centered(TextFormat("WAVE %u // T-%u", game->wave,
            (game->intermission + 29) / 30), 378, 11, C_GOLD);
    }
    draw_feedback(game);
    EndDrawing();
}
