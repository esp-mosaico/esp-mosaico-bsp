#pragma once
#include "wav_file.h"
#include <stddef.h>
bool recording_metadata_path(const char *pcm, char *out, size_t capacity);
/* Returns an open stream positioned at the first PCM sample (PCM+meta or WAV). */
FILE *recording_open(const char *path, uint32_t *bytes);
/* Publish metadata last. Never overwrites an existing destination. */
bool recording_publish(const char *part, const char *pcm, uint32_t bytes);
