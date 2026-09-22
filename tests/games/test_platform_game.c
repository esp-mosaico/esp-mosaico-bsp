#include <assert.h>
#include <stdio.h>
#include "platform_game.h"

int main(void)
{
    platform_game_t game;
    platform_game_reset(&game);
    assert(game.phase == PLATFORM_START && game.lives == 3 && game.level == 0);
    assert(game.coin_count == 12 && game.enemy_count == 4);
    size_t block_count = 0;
    assert(platform_game_blocks(&game, &block_count) != NULL && block_count == 12);
    platform_game_set_pointer(&game, 220, 430, true, 1);
    assert(game.phase == PLATFORM_PLAYING && game.move_right && !game.facing_left);
    for (int i = 0; i < 20; ++i) platform_game_update(&game);
    assert(game.player_x > 42);
    platform_game_set_pointer(&game, 350, 430, true, 2);
    platform_game_update(&game);
    assert(game.velocity_y < 0 && !game.grounded && game.velocity_x > 0);
    platform_game_set_pointer(&game, 40, 430, true, 1);
    assert(game.move_left && game.facing_left && game.jump_held);
    platform_game_set_pointer(&game, 350, 430, false, 2);
    platform_game_set_pointer(&game, 40, 430, false, 1);
    for (int i = 0; i < 40 && !game.grounded; ++i) platform_game_update(&game);
    assert(game.grounded);
    platform_game_set_action(&game, PLATFORM_ACTION_JUMP, true);
    platform_game_set_action(&game, PLATFORM_ACTION_JUMP, false);
    platform_game_update(&game);
    assert(game.velocity_y < 0 && !game.grounded);
    platform_game_set_action(&game, PLATFORM_ACTION_PAUSE, true);
    assert(game.phase == PLATFORM_PAUSED);
    uint32_t paused_tick = game.tick;
    platform_game_update(&game);
    assert(game.tick == paused_tick);
    platform_game_set_action(&game, PLATFORM_ACTION_PAUSE, true);
    assert(game.phase == PLATFORM_PLAYING);

    size_t spring_count = 0;
    const platform_spring_t *springs = platform_game_springs(&game, &spring_count);
    assert(springs != NULL && spring_count == 2);
    platform_game_set_action(&game, PLATFORM_ACTION_LEFT, false);
    platform_game_set_action(&game, PLATFORM_ACTION_RIGHT, false);
    game.player_x = springs[0].x + 4;
    game.player_y = springs[0].y - 36;
    game.velocity_x = game.velocity_y = 0;
    game.grounded = true;
    platform_game_update(&game);
    assert(game.velocity_y < -10 && !game.grounded);

    float checkpoint = platform_game_checkpoint_x(&game);
    game.player_x = checkpoint + 1;
    game.player_y = 354;
    game.velocity_x = game.velocity_y = 0;
    platform_game_update(&game);
    assert(game.checkpoint_active && game.respawn_x == checkpoint);
    uint8_t checkpoint_lives = game.lives;
    game.player_y = 501;
    platform_game_update(&game);
    assert(game.lives + 1 == checkpoint_lives && game.player_x == checkpoint);

    game.player_x = platform_game_finish_x(&game) + 1;
    platform_game_update(&game);
    assert(game.phase == PLATFORM_LEVEL_CLEAR && game.level == 0);
    const uint16_t score = game.score;
    const uint8_t lives = game.lives;
    platform_game_set_action(&game, PLATFORM_ACTION_RESTART, true);
    assert(game.phase == PLATFORM_PLAYING && game.level == 1);
    assert(game.score == score && game.lives == lives);
    assert(game.coin_count == 12 && game.enemy_count == 5);
    assert(platform_game_world_width(&game) == 2400.0f);
    springs = platform_game_springs(&game, &spring_count);
    assert(springs != NULL && spring_count == 3);
    game.player_x = 1092;
    game.player_y = 354;
    game.velocity_x = game.velocity_y = 0;
    game.grounded = true;
    platform_game_update(&game);
    assert(game.velocity_y < -10 && !game.grounded);

    game.player_x = platform_game_finish_x(&game) + 1;
    platform_game_update(&game);
    assert(game.phase == PLATFORM_LEVEL_CLEAR);
    platform_game_set_action(&game, PLATFORM_ACTION_RESTART, true);
    assert(game.phase == PLATFORM_PLAYING && game.level == 2);
    assert(game.coin_count == 13 && game.enemy_count == 6);
    assert(platform_game_world_width(&game) == 2480.0f);

    game.player_x = platform_game_finish_x(&game) + 1;
    platform_game_update(&game);
    assert(game.phase == PLATFORM_LEVEL_CLEAR);
    platform_game_set_action(&game, PLATFORM_ACTION_RESTART, true);
    assert(game.phase == PLATFORM_PLAYING && game.level == 3);
    assert(game.coin_count == 15 && game.enemy_count == 7);
    assert(platform_game_world_width(&game) == 2760.0f);

    game.player_x = platform_game_finish_x(&game) + 1;
    platform_game_update(&game);
    assert(game.phase == PLATFORM_WON && game.level == 3);
    platform_game_set_action(&game, PLATFORM_ACTION_RESTART, true);
    assert(game.phase == PLATFORM_PLAYING && game.level == 0 && game.lives == 3);
    assert(platform_game_state_hash(&game) != 0);
    puts("platform game model: ok");
    return 0;
}
