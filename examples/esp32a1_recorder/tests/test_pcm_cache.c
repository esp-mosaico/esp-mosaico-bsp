#include "pcm_cache.h"
#include <assert.h>
#include <string.h>
static uint8_t input[70002],output[70002];
typedef struct { size_t last, stop_at; } progress_t;
static bool progress(size_t loaded,size_t total,void *ctx)
{
    progress_t *p=ctx;
    assert(loaded>=p->last && loaded<=total);
    p->last=loaded;
    return loaded<p->stop_at;
}
int main(void)
{
    for(size_t i=0;i<sizeof(input);i++)input[i]=(uint8_t)(i*7);
    FILE *f=tmpfile();assert(f);
    assert(fwrite(input,1,sizeof(input),f)==sizeof(input));rewind(f);
    progress_t p={.stop_at=SIZE_MAX};
    assert(pcm_cache_load(f,output,sizeof(output),progress,&p)==PCM_CACHE_OK);
    assert(p.last==sizeof(input));assert(!memcmp(input,output,sizeof(input)));
    /* A closed source cannot affect the fully cached audio, including the tail. */
    assert(fclose(f)==0);assert(!memcmp(input,output,sizeof(input)));
    f=tmpfile();assert(f);assert(fwrite(input,1,60000,f)==60000);rewind(f);
    assert(pcm_cache_load(f,output,sizeof(output),NULL,NULL)==PCM_CACHE_IO_ERROR);
    rewind(f);p=(progress_t){.stop_at=32768};
    assert(pcm_cache_load(f,output,60000,progress,&p)==PCM_CACHE_CANCELLED);
    assert(p.last==32768);
    rewind(f);p=(progress_t){.stop_at=0};
    assert(pcm_cache_load(f,output,60000,progress,&p)==PCM_CACHE_CANCELLED);
    assert(ftell(f)==0);
    assert(pcm_cache_load(f,output,5,NULL,NULL)==PCM_CACHE_INVALID);
    assert(pcm_cache_load(f,output,0,NULL,NULL)==PCM_CACHE_INVALID);
    fclose(f);puts("Full PCM cache tests passed: full load, tail, truncated input, cancel, invalid size");
    return 0;
}
