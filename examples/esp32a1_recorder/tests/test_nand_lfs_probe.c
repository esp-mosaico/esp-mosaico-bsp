#include "nand_lfs_probe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char media[40][512], saved[40][512];
static unsigned writes, erases;
static bool logical_erase_noop;
static int read_page(const struct lfs_config *c, lfs_block_t b, lfs_off_t off,
                     void *data, lfs_size_t n)
{
    (void)c;
    if (b>=40 || off>512 || n>512-off) return LFS_ERR_IO;
    memcpy(data,media[b]+off,n);return 0;
}
static int write_page(const struct lfs_config *c, lfs_block_t b, lfs_off_t off,
                      const void *data, lfs_size_t n)
{
    (void)c;
    if (b>=40 || off>512 || n>512-off) return LFS_ERR_IO;
    writes++;memcpy(media[b]+off,data,n);return 0;
}
static int erase_page(const struct lfs_config *c, lfs_block_t b)
{
    (void)c;if(b>=40)return LFS_ERR_IO;
    erases++;
    if(!logical_erase_noop)memset(media[b],0xff,512);
    return 0;
}
static int sync_media(const struct lfs_config *c) { (void)c;return 0; }
int main(void)
{
    struct lfs_config c={.read=read_page,.prog=write_page,.erase=erase_page,.sync=sync_media,
        .read_size=512,.prog_size=512,.block_size=512,.block_count=32,
        .block_cycles=-1,.cache_size=512,.lookahead_size=8};
    memset(media,0xff,sizeof(media));
    lfs_t fs={0};assert(lfs_format(&fs,&c)==0);assert(lfs_mount(&fs,&c)==0);
    lfs_file_t file={0};
    assert(lfs_file_open(&fs,&file,"recording",LFS_O_CREAT|LFS_O_WRONLY)==0);
    assert(lfs_file_write(&fs,&file,"keep audio",10)==10);
    assert(lfs_file_close(&fs,&file)==0);assert(lfs_unmount(&fs)==0);
    memcpy(saved,media,sizeof(media));writes=erases=0;
    uint32_t blocks=0;
    assert(nand_lfs_probe(&c,32,&blocks)==0 && blocks==32);
    assert(nand_lfs_probe(&c,40,&blocks)==0 && blocks==32);
    assert(nand_lfs_probe(&c,24,&blocks)==LFS_ERR_INVAL && blocks==0);
    assert(!writes && !erases && !memcmp(media,saved,sizeof(media)));
    assert(lfs_mount(&fs,&c)==0);
    assert(lfs_file_open(&fs,&file,"recording",LFS_O_RDONLY)==0);
    char data[10];assert(lfs_file_read(&fs,&file,data,10)==10);
    assert(!memcmp(data,"keep audio",10));
    assert(lfs_file_close(&fs,&file)==0);assert(lfs_unmount(&fs)==0);
    memset(media,0xff,sizeof(media));
    assert(nand_lfs_probe(&c,40,&blocks)!=0 && blocks==0);
    assert(!writes && !erases);
    puts("LittleFS geometry tests passed: equal/larger/smaller/invalid media; no writes");
    memcpy(media,saved,sizeof(media));
    logical_erase_noop=true;
    unsigned char pcm[2048], readback[2048];
    for(unsigned cycle=0;cycle<64;cycle++) {
        memset(pcm,(int)cycle,sizeof(pcm));
        assert(lfs_mount(&fs,&c)==0);
        assert(lfs_file_open(&fs,&file,"new_recording",LFS_O_CREAT|LFS_O_WRONLY)==0);
        for(unsigned chunk=0;chunk<3;chunk++)assert(lfs_file_write(&fs,&file,pcm,sizeof(pcm))==sizeof(pcm));
        assert(lfs_file_close(&fs,&file)==0);
        assert(lfs_unmount(&fs)==0);
        assert(lfs_mount(&fs,&c)==0);
        assert(lfs_file_open(&fs,&file,"new_recording",LFS_O_RDONLY)==0);
        for(unsigned chunk=0;chunk<3;chunk++) {
            assert(lfs_file_read(&fs,&file,readback,sizeof(readback))==sizeof(readback));
            assert(!memcmp(pcm,readback,sizeof(pcm)));
        }
        assert(lfs_file_close(&fs,&file)==0);
        assert(lfs_remove(&fs,"new_recording")==0);
        assert(lfs_file_open(&fs,&file,"recording",LFS_O_RDONLY)==0);
        assert(lfs_file_read(&fs,&file,data,10)==10 && !memcmp(data,"keep audio",10));
        assert(lfs_file_close(&fs,&file)==0);
        assert(lfs_unmount(&fs)==0);
    }
    puts("Logical-page no-erase tests passed: 64 write/read/delete/remount cycles; existing file preserved");
    return 0;
}
