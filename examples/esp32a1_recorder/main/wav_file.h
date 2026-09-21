#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#define WAV_RATE 48000U
#define WAV_FRAME_BYTES 6U
#define WAV_BYTES_PER_SECOND (WAV_RATE * WAV_FRAME_BYTES)
bool wav_write_header(FILE *file, uint32_t bytes);
bool wav_read_header(FILE *file, uint32_t *bytes);
bool wav_read_metadata(FILE *file, uint32_t *bytes);
