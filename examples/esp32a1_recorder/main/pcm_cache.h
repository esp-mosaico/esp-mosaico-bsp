#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef enum { PCM_CACHE_OK, PCM_CACHE_CANCELLED, PCM_CACHE_IO_ERROR, PCM_CACHE_INVALID } pcm_cache_result_t;
typedef bool (*pcm_cache_progress_t)(size_t loaded, size_t total, void *ctx);
/* Source is positioned at PCM data; caller owns source and destination lifetime.
 * Only PCM_CACHE_OK permits playback of destination. */
pcm_cache_result_t pcm_cache_load(FILE *source, uint8_t *destination, size_t bytes,
                                  pcm_cache_progress_t progress, void *ctx);
