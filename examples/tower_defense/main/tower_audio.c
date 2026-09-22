// SPDX-License-Identifier: Apache-2.0
#include "tower_audio.h"
#include <stdbool.h>
#include "mosaico_game_audio.h"
static Sound s_cues[TOWER_AUDIO_GAME_OVER+1];
static Music s_music;
static bool s_music_stopped;
static const char *const CUE_PATHS[]={"wave.sound","build.sound","shot.sound",
    "explosion.sound","wave.sound","leak.sound","game_over.sound"};

esp_err_t tower_audio_init(void)
{
    InitAudioDevice();
    for(unsigned i=0;i<=TOWER_AUDIO_GAME_OVER;++i)s_cues[i]=LoadSound(CUE_PATHS[i]);
    s_music=LoadMusicStream("music.sound");
    SetMusicVolume(s_music,.24f);PlayMusicStream(s_music);s_music_stopped=false;
    return ESP_OK;
}

esp_err_t tower_audio_play(tower_audio_cue_t cue)
{
    if(cue>TOWER_AUDIO_GAME_OVER||!s_cues[cue].frameCount)return ESP_ERR_INVALID_STATE;
    PlaySound(s_cues[cue]);return ESP_OK;
}

void tower_audio_set_music(bool paused, bool game_over)
{
    if(game_over){StopMusicStream(s_music);s_music_stopped=true;return;}
    SetMusicVolume(s_music,paused?.07f:.24f);
    if(s_music_stopped&&IsAudioDeviceReady()){PlayMusicStream(s_music);s_music_stopped=false;}
}
