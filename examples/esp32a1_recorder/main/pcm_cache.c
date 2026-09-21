#include "pcm_cache.h"
#include "wav_file.h"
pcm_cache_result_t pcm_cache_load(FILE *source, uint8_t *destination, size_t bytes,
                                  pcm_cache_progress_t progress, void *ctx)
{
    if (!source || !destination || !bytes || bytes%WAV_FRAME_BYTES) return PCM_CACHE_INVALID;
    size_t loaded=0;
    for (;;) {
        if (progress && !progress(loaded,bytes,ctx)) return PCM_CACHE_CANCELLED;
        if (loaded==bytes) return PCM_CACHE_OK;
        size_t n=bytes-loaded;
        if (n>32768) n=32768;
        if (fread(destination+loaded,1,n,source)!=n) return PCM_CACHE_IO_ERROR;
        loaded+=n;
    }
}
