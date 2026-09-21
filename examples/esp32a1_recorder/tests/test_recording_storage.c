#define _POSIX_C_SOURCE 200809L
#include "recording_storage.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void)
{
    char dir[]="/tmp/recorder-storage-XXXXXX";assert(mkdtemp(dir));
    char part[256],pcm[256],meta[256],wav[256];
    snprintf(part,sizeof(part),"%s/rec_000001.pcm.part",dir);
    snprintf(pcm,sizeof(pcm),"%s/rec_000001.pcm",dir);
    snprintf(wav,sizeof(wav),"%s/old.wav",dir);
    assert(recording_metadata_path(pcm,meta,sizeof(meta)));
    assert(!recording_metadata_path(wav,meta,2));
    uint8_t data[6000],out[6000];for(unsigned i=0;i<sizeof(data);i++)data[i]=(uint8_t)i;
    FILE *f=fopen(part,"wb");assert(f);assert(fwrite(data,1,sizeof(data),f)==sizeof(data));assert(fclose(f)==0);
    assert(recording_publish(part,pcm,sizeof(data)));
    uint32_t bytes;
    f=recording_open(pcm,&bytes);assert(f && bytes==sizeof(data));assert(ftell(f)==0);
    assert(fread(out,1,bytes,f)==bytes && !memcmp(out,data,bytes));fclose(f);
    assert(!recording_publish(part,pcm,sizeof(data))); /* Never overwrite. */
    f=fopen(wav,"wb+");assert(f);assert(wav_write_header(f,sizeof(data)));
    assert(fwrite(data,1,sizeof(data),f)==sizeof(data));fclose(f);
    f=recording_open(wav,&bytes);assert(f && bytes==sizeof(data) && ftell(f)==44);fclose(f);
    /* A missing commit marker or truncated raw file cannot be listed/played. */
    assert(unlink(meta)==0);assert(!recording_open(pcm,&bytes));
    f=fopen(meta,"wb");assert(f);assert(wav_write_header(f,6006));fclose(f);
    assert(!recording_open(pcm,&bytes));
    assert(unlink(meta)==0);assert(unlink(pcm)==0);assert(unlink(wav)==0);assert(rmdir(dir)==0);
    puts("Recording storage tests passed: PCM commit/read, old WAV, no overwrite, missing metadata, truncation");
    return 0;
}
