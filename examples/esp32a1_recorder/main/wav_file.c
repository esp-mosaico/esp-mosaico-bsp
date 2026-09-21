#include "wav_file.h"
#include <string.h>
static void put32(uint8_t *p, uint32_t v) { for (int i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
bool wav_write_header(FILE *f, uint32_t bytes)
{
    if (bytes > UINT32_MAX-36 || bytes%WAV_FRAME_BYTES) return false;
    uint8_t h[44] = {0};
    memcpy(h,"RIFF",4); put32(h+4,bytes+36); memcpy(h+8,"WAVEfmt ",8);
    put32(h+16,16); h[20]=1; h[22]=2; put32(h+24,WAV_RATE);
    put32(h+28,WAV_BYTES_PER_SECOND); h[32]=6; h[34]=24;
    memcpy(h+36,"data",4); put32(h+40,bytes);
    return fseek(f,0,SEEK_SET)==0 && fwrite(h,1,sizeof(h),f)==sizeof(h);
}
bool wav_read_metadata(FILE *f, uint32_t *bytes)
{
    uint8_t h[44];
    if (fseek(f,0,SEEK_SET) || fread(h,1,44,f)!=44) return false;
    if (memcmp(h,"RIFF",4)||memcmp(h+8,"WAVEfmt ",8)||get32(h+16)!=16 ||
        h[20]!=1||h[21]||h[22]!=2||h[23]||get32(h+24)!=WAV_RATE ||
        get32(h+28)!=WAV_BYTES_PER_SECOND||h[32]!=6||h[33]||h[34]!=24||h[35]||
        memcmp(h+36,"data",4)) return false;
    *bytes=get32(h+40);
    if (*bytes>UINT32_MAX-36 || *bytes%6 || get32(h+4)!=*bytes+36) return false;
    return true;
}
bool wav_read_header(FILE *f, uint32_t *bytes)
{
    if(!wav_read_metadata(f,bytes))return false;
    if (fseek(f,0,SEEK_END)) return false;
    long size=ftell(f);
    return size>=44 && (uint64_t)size>=44ULL+*bytes && fseek(f,44,SEEK_SET)==0;
}
