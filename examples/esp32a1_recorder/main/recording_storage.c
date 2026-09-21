#define _POSIX_C_SOURCE 200809L
#include "recording_storage.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
bool recording_metadata_path(const char *pcm, char *out, size_t capacity)
{
    size_t n=strlen(pcm);
    if(n<4 || strcmp(pcm+n-4,".pcm") || n+2>capacity)return false;
    memcpy(out,pcm,n-4);memcpy(out+n-4,".meta",6);return true;
}
FILE *recording_open(const char *path, uint32_t *bytes)
{
    FILE *data=fopen(path,"rb");
    if(!data)return NULL;
    char meta[384];
    bool valid;
    if(recording_metadata_path(path,meta,sizeof(meta))) {
        FILE *header=fopen(meta,"rb");
        struct stat st,meta_st;
        valid=header && wav_read_metadata(header,bytes) && *bytes>0 &&
              fstat(fileno(header),&meta_st)==0 && meta_st.st_size==44 &&
              fstat(fileno(data),&st)==0 && (uint64_t)st.st_size>=*bytes;
        if(header)fclose(header);
    } else valid=wav_read_header(data,bytes);
    if(!valid) {fclose(data);return NULL;}
    return data;
}
static bool missing(const char *path)
{
    struct stat st;
    if(stat(path,&st)==0) {errno=EEXIST;return false;}
    return errno==ENOENT;
}
bool recording_publish(const char *part, const char *pcm, uint32_t bytes)
{
    char meta[384],temp[400];
    if(!bytes || !recording_metadata_path(pcm,meta,sizeof(meta)) ||
       !missing(pcm) || !missing(meta))return false;
    snprintf(temp,sizeof(temp),"%s.part",meta);
    int fd=open(temp,O_WRONLY|O_CREAT|O_EXCL,0666);
    if(fd<0)return false;
    FILE *f=fdopen(fd,"wb");
    if(!f) {close(fd);return false;}
    bool ok=wav_write_header(f,bytes);
    if(fflush(f))ok=false;
    if(fsync(fileno(f)))ok=false;
    if(fclose(f))ok=false;
    if(!ok)return false;
    /* Data is already fully synced by the caller. The final metadata is the
     * commit marker; until it exists, the recording is not listed/playable. */
    return rename(part,pcm)==0 && rename(temp,meta)==0;
}
