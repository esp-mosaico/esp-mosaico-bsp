#include "wav_file.h"
#include <assert.h>
#include <limits.h>
#include <string.h>
int main(void)
{
    FILE *f=tmpfile(); assert(f);
    uint8_t pcm[60]; memset(pcm,0xa5,sizeof(pcm));
    assert(wav_write_header(f,0));
    assert(fwrite(pcm,1,sizeof(pcm),f)==sizeof(pcm));
    assert(wav_write_header(f,sizeof(pcm)));
    uint32_t size=0; assert(wav_read_header(f,&size));assert(size==sizeof(pcm));
    uint8_t actual[60];assert(fread(actual,1,size,f)==size);assert(!memcmp(pcm,actual,size));
    assert(!wav_write_header(f,5));assert(!wav_write_header(f,UINT32_MAX));
    assert(wav_write_header(f,66));assert(!wav_read_header(f,&size));
    assert(wav_write_header(f,60));assert(fseek(f,22,SEEK_SET)==0);assert(fputc(1,f)!=EOF);
    assert(!wav_read_header(f,&size));
    fclose(f);
    f=tmpfile();assert(f);assert(wav_write_header(f,0));assert(wav_read_header(f,&size));assert(size==0);fclose(f);
    puts("WAV tests passed");return 0;
}
