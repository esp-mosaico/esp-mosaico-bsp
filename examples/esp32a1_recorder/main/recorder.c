#include "recorder.h"
#include "a1_rec_audio.h"
#include "nand_littlefs.h"
#include "wav_file.h"
#include "pcm_cache.h"
#include "recording_storage.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define REC_DIR "/nandflash/recordings"
#define RING_SIZE (8U*1024U*1024U)
typedef struct { int type; uint32_t generation; char name[32]; } command_t;
static QueueHandle_t commands;
static SemaphoreHandle_t lock;
static recorder_state_t state;
static uint32_t generation;
static uint8_t *ring;
typedef struct { uint8_t *pcm; uint32_t size; } playback_job_t;
static QueueHandle_t playback_jobs;
static SemaphoreHandle_t playback_done;
static bool playback_active, playback_cancel, playback_failed;
static uint8_t *playback_cache;
static uint32_t playback_cache_size, recording_cache_capacity;
static char playback_cache_name[32];
static void release_playback_cache(void);
static size_t head, tail, used;
static bool capturing, capture_error;
static uint32_t captured_bytes;
static unsigned serial;
static FILE *file;
static uint32_t bytes;
static char part_path[96], final_path[96];
static const char *TAG="recorder";
static void take(void) { xSemaphoreTake(lock,portMAX_DELAY); }
static void give(void) { xSemaphoreGive(lock); }
static void status(recorder_mode_t mode,const char *message)
{
    take(); state.mode=mode; state.seconds=0; generation++;
    snprintf(state.message,sizeof(state.message),"%s",message); give();
    ESP_LOGI(TAG,"%s",message);
}
void recorder_snapshot(recorder_state_t *out) { take(); *out=state; give(); }
static bool send(int type,const char *name)
{
    if (!commands) return false;
    command_t c={.type=type};
    take();
    if (state.mode==REC_BUFFERING && type==2) {
        playback_cancel=true;
        give();
        return true;
    }
    bool ready=state.mode==REC_IDLE || state.mode==REC_RECORDING || state.mode==REC_PLAYING;
    if (type==6 && state.mode==REC_RECORDING) ready=false;
    c.generation=generation;
    bool stopping=(type==1 || type==2) && state.mode==REC_RECORDING;
    if (!ready) { give(); return false; }
    if (name) snprintf(c.name,sizeof(c.name),"%s",name);
    bool queued=(stopping?xQueueSendToFront(commands,&c,0):xQueueSend(commands,&c,0))==pdTRUE;
    if (queued && stopping) {
        /* Stop acquisition immediately, even if the writer is blocked in NAND. */
        capturing=false;
        state.mode=REC_SAVING;
        snprintf(state.message,sizeof(state.message),"Finishing NAND write...");
    }
    give();
    return queued;
}
bool recorder_toggle(void) { return send(1,NULL); }
bool recorder_stop(void) { return send(2,NULL); }
bool recorder_play(const char *name) { return name && !strchr(name,'/') && send(3,name); }
bool recorder_volume(bool increase) { return send(increase?5:4,NULL); }
bool recorder_delete(const char *name)
{
    return name && strlen(name)<sizeof(((command_t *)0)->name) &&
           !strchr(name,'/') && send(6,name);
}
static int compare(const void *a,const void *b) { return strcmp(((const recording_t *)a)->name,((const recording_t *)b)->name); }
/* LittleFS provides stat(), but not access(): unsupported access returns ENOSYS. */
static bool destination_available(const char *path)
{
    struct stat st;
    if (stat(path,&st)==0) { errno=EEXIST; return false; }
    return errno==ENOENT;
}
static void recover_completed_parts(void)
{
    DIR *dir=opendir(REC_DIR);
    if (!dir) return;
    struct dirent *entry;
    while ((entry=readdir(dir))) {
        size_t len=strlen(entry->d_name);
        if(strstr(entry->d_name,".pcm.part") || strstr(entry->d_name,".meta.part"))continue;
        if (len<6 || len>=32 || strncmp(entry->d_name,"rec_",4) || strcmp(entry->d_name+len-5,".part")) continue;
        char src[96],dst[96];
        snprintf(src,sizeof(src),REC_DIR "/%s",entry->d_name);
        snprintf(dst,sizeof(dst),REC_DIR "/%.*s.wav",(int)(len-5),entry->d_name);
        FILE *f=fopen(src,"rb"); uint32_t size=0; struct stat st;
        /* Only recover already-finalized WAV data, never guess a power-loss header. */
        bool valid=f && wav_read_header(f,&size) && size>0 &&
                   stat(src,&st)==0 && (uint64_t)st.st_size==44ULL+size;
        if (f && fclose(f)!=0) valid=false;
        if (valid && destination_available(dst)) {
            if (rename(src,dst)==0) {
                ESP_LOGI(TAG,"Recovered completed recording: %s",dst);
                /* Renaming changes directory ordering; rescan to avoid skipped entries. */
                closedir(dir);
                dir=opendir(REC_DIR);
                if (!dir) return;
            } else ESP_LOGE(TAG,"recover rename %s: errno=%d (%s)",src,errno,strerror(errno));
        }
    }
    closedir(dir);
}
static bool scan(void)
{
    int64_t started=esp_timer_get_time();
    recording_t *list=calloc(REC_MAX_FILES,sizeof(*list));
    DIR *dir=opendir(REC_DIR);
    if (!list || !dir) { free(list); if(dir)closedir(dir); return false; }
    unsigned count=0;
    struct dirent *entry;
    while ((entry=readdir(dir))) {
        unsigned n; char suffix[8];
        if (sscanf(entry->d_name,"rec_%u.%7s",&n,suffix)==2 && n>serial) serial=n;
        size_t len=strlen(entry->d_name);
        if (len<5 || len>=sizeof(list[0].name) ||
            (strcmp(entry->d_name+len-4,".wav") && strcmp(entry->d_name+len-4,".pcm")) || count==REC_MAX_FILES) continue;
        char path[320]; snprintf(path,sizeof(path),"%s/%s",REC_DIR,entry->d_name);
        uint32_t size;
        FILE *f=recording_open(path,&size);
        if (f) {
            snprintf(list[count].name,sizeof(list[count].name),"%s",entry->d_name);
            list[count++].seconds=size/WAV_BYTES_PER_SECOND;
        }
        if (f) fclose(f);
    }
    closedir(dir); qsort(list,count,sizeof(*list),compare);
    take(); memcpy(state.files,list,count*sizeof(*list)); state.count=count; state.revision++; give();
    free(list);
    ESP_LOGI(TAG,"Recording list scan: %u files, %lld ms",count,
             (long long)((esp_timer_get_time()-started)/1000));
    return true;
}
static void capture_task(void *arg)
{
    (void)arg;
    uint8_t tdm[256*15], stereo[256*6];
    for (;;) {
        esp_err_t err=a1_rec_audio_read(tdm,sizeof(tdm));
        if (err==ESP_OK) {
            for (unsigned i=0;i<256;i++) memcpy(stereo+i*6,tdm+i*15,6);
        }
        take();
        if (capturing) {
            if (err!=ESP_OK || RING_SIZE-used<sizeof(stereo)) {
                capture_error=true; capturing=false;
                ESP_LOGE(TAG,"Capture stopped: %s, buffered=%u bytes, captured=%lu bytes",
                         err!=ESP_OK?"ADC read failure":"NAND writer too slow / buffer full",
                         (unsigned)used,(unsigned long)captured_bytes);
            } else {
                size_t first=RING_SIZE-head;
                if (first>sizeof(stereo)) first=sizeof(stereo);
                memcpy(ring+head,stereo,first); memcpy(ring,stereo+first,sizeof(stereo)-first);
                head=(head+sizeof(stereo))%RING_SIZE; used+=sizeof(stereo);
                captured_bytes+=sizeof(stereo);
                state.seconds=captured_bytes/WAV_BYTES_PER_SECOND;
            }
        }
        give();
        if (err!=ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
    }
}
static size_t drain(uint8_t *out,size_t capacity)
{
    take(); size_t n=used<capacity?used:capacity;
    size_t first=RING_SIZE-tail; if(first>n)first=n;
    memcpy(out,ring+tail,first); memcpy(out+first,ring,n-first);
    tail=(tail+n)%RING_SIZE; used-=n; give(); return n;
}
static bool write_pcm(uint8_t *pcm,size_t n)
{
    if (n>UINT32_MAX-36-bytes) return false;
    if (recording_cache_capacity) {
        if (bytes<=recording_cache_capacity && n<=recording_cache_capacity-bytes)
            memcpy(playback_cache+bytes,pcm,n);
        else release_playback_cache();
    }
    size_t written=fwrite(pcm,1,n,file);
    if (written!=n) ESP_LOGE(TAG,"PCM write: %u/%u bytes, errno=%d (%s)",
                            (unsigned)written,(unsigned)n,errno,strerror(errno));
    bytes+=(uint32_t)(written-written%WAV_FRAME_BYTES);
    return written==n;
}
static void finish_record(bool failed)
{
    int64_t finish_started=esp_timer_get_time();
    bool write_failed=failed;
    take(); capturing=false; failed|=capture_error;
    uint32_t total_captured=captured_bytes;
    give();
    status(REC_SAVING,"Saving recording...");
    take(); state.seconds=total_captured/WAV_BYTES_PER_SECOND; give();
    uint8_t pcm[3072]; size_t n;
    while (!write_failed && (n=drain(pcm,sizeof(pcm)))) {
        if (!write_pcm(pcm,n)) { failed=true; write_failed=true; }
    }
    ESP_LOGI(TAG,"Finalize: captured=%lu, written=%lu PCM bytes",
             (unsigned long)total_captured,(unsigned long)bytes);
    take(); used=0; give();
    clearerr(file);
    /* No seeking to the start: LittleFS would copy the entire audio tail. */
    bool saved=true;
    if (fflush(file)) { ESP_LOGE(TAG,"flush failed: errno=%d (%s)",errno,strerror(errno)); saved=false; }
    if (fsync(fileno(file))) { ESP_LOGE(TAG,"sync failed: errno=%d (%s)",errno,strerror(errno)); saved=false; }
    if (fclose(file)) { ESP_LOGE(TAG,"close failed: errno=%d (%s)",errno,strerror(errno)); saved=false; }
    file=NULL;
    if (!bytes) saved=false; /* Never publish an empty recording as successful. */
    if(saved)saved=recording_publish(part_path,final_path,bytes);
    if(!saved)ESP_LOGE(TAG,"PCM commit failed; data retained: errno=%d (%s)",errno,strerror(errno));
    else ESP_LOGI(TAG,"Saved %s: %lu PCM bytes; finalize=%lld ms (no WAV rewrite)",
                  final_path,(unsigned long)bytes,(long long)((esp_timer_get_time()-finish_started)/1000));
    if (saved) {
        const char *name=strrchr(final_path,'/')+1;
        take();
        if(state.count<REC_MAX_FILES) {
            recording_t *entry=&state.files[state.count++];
            snprintf(entry->name,sizeof(entry->name),"%s",name);
            entry->seconds=bytes/WAV_BYTES_PER_SECOND;
            qsort(state.files,state.count,sizeof(state.files[0]),compare);
            state.revision++;
        }
        give();
        if(recording_cache_capacity) {
            playback_cache_size=bytes;
            snprintf(playback_cache_name,sizeof(playback_cache_name),"%s",name);
            recording_cache_capacity=0;
            ESP_LOGI(TAG,"New recording retained in RAM: %s (%lu bytes)",name,(unsigned long)bytes);
        }
    } else release_playback_cache();
    status(REC_IDLE,!saved?"Save failed: unfinished data retained":failed?"Recording interrupted; partial PCM saved":"Recording saved");
}
static void playback_task(void *arg)
{
    (void)arg;
    playback_job_t job;
    /* Internal task-stack staging keeps the I2S write path independent of NAND. */
    uint8_t pcm[3072];
    for (;;) {
        xQueueReceive(playback_jobs,&job,portMAX_DELAY);
        uint32_t offset=0;
        bool failed=false, cancelled=false;
        a1_rec_audio_speaker(true);
        while (offset<job.size) {
            take(); cancelled=playback_cancel; give();
            if (cancelled) break;
            size_t n=job.size-offset;
            if (n>sizeof(pcm)) n=sizeof(pcm);
            memcpy(pcm,job.pcm+offset,n);
            if (a1_rec_audio_write(pcm,n)!=ESP_OK) { failed=true; break; }
            offset+=n;
            a1_rec_audio_update_route(true);
            take(); state.seconds=offset/WAV_BYTES_PER_SECOND; give();
        }
        /* DMA holds up to 60 * 128 / 48000 = 160 ms of queued audio. */
        if (!cancelled && !failed) vTaskDelay(pdMS_TO_TICKS(170));
        a1_rec_audio_speaker(false);
        vTaskDelay(pdMS_TO_TICKS(170));
        take(); playback_failed=failed; give();
        ESP_LOGI(TAG,"RAM playback finished: %lu/%lu PCM bytes, cancelled=%d, failed=%d",
                 (unsigned long)offset,(unsigned long)job.size,cancelled,failed);
        xSemaphoreGive(playback_done);
    }
}
static void release_playback_cache(void)
{
    if (playback_cache && playback_cache!=ring) heap_caps_free(playback_cache);
    playback_cache=NULL;
    playback_active=false;
    playback_cache_size=recording_cache_capacity=0;
    playback_cache_name[0]='\0';
}
static void stop_playback(void)
{
    if (playback_active) {
        take(); playback_cancel=true; give();
        /* Never reuse/free PCM memory until the feeder has stopped accessing it. */
        xSemaphoreTake(playback_done,portMAX_DELAY);
        playback_active=false; /* Retain a valid cache for immediate replay. */
    }
    status(REC_IDLE,"Ready");
}
static void begin_record(void)
{
    take(); unsigned count=state.count; give();
    if(count>=REC_MAX_FILES) {status(REC_IDLE,"File limit reached (128)");return;}
    release_playback_cache();
    int fd=-1;
    while(serial<999999) {
        serial++;
        snprintf(final_path,sizeof(final_path),REC_DIR "/rec_%06u.pcm",serial);
        snprintf(part_path,sizeof(part_path),REC_DIR "/rec_%06u.pcm.part",serial);
        if(!destination_available(final_path)) {
            if(errno==EEXIST)continue;
            ESP_LOGE(TAG,"stat %s failed: errno=%d (%s)",final_path,errno,strerror(errno));
            break;
        }
        fd=open(part_path,O_CREAT|O_EXCL|O_RDWR,0666);
        if(fd>=0 || errno!=EEXIST)break;
    }
    if(fd<0) {status(REC_IDLE,"Cannot create recording (storage full?)");return;}
    file=fdopen(fd,"wb+");
    if(!file) {close(fd);status(REC_IDLE,"Cannot open recording");return;}
    bytes=0;
    /* Optional mirror: no extra NAND read is needed to play a new short recording.
     * Allocation failure affects instant replay only, not recording or saving. */
    playback_cache=heap_caps_malloc(RING_SIZE,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    recording_cache_capacity=playback_cache?RING_SIZE:0;
    ESP_LOGI(TAG,"New-recording playback mirror: %u bytes",(unsigned)recording_cache_capacity);
    a1_rec_audio_speaker(false);
    status(REC_RECORDING,strrchr(final_path,'/')+1);
    take(); head=tail=used=0; captured_bytes=0; capture_error=false; capturing=true; give();
}
static bool cache_progress(size_t loaded, size_t total, void *ctx)
{
    take();
    bool cancelled=playback_cancel;
    snprintf(state.message,sizeof(state.message),"Loading %u%%: %s",
             (unsigned)((uint64_t)loaded*100/total),(const char *)ctx);
    give();
    return !cancelled;
}
static void start_cached_playback(const char *name)
{
    take(); playback_cancel=false; playback_failed=false; give();
    playback_job_t job={.pcm=playback_cache,.size=playback_cache_size};
    playback_active=true;
    status(REC_PLAYING,name);
    if(xQueueSend(playback_jobs,&job,0)!=pdTRUE) {
        playback_active=false;status(REC_IDLE,"Playback task busy");
    }
}
static void begin_play(const char *name)
{
    bool found=false;
    take(); for(unsigned i=0;i<state.count;i++) if(!strcmp(name,state.files[i].name))found=true; give();
    if(!found)return;
    if(playback_cache && playback_cache_size && !strcmp(name,playback_cache_name)) {
        ESP_LOGI(TAG,"Playback RAM cache hit: %s; skipping NAND load",name);
        start_cached_playback(name);
        return;
    }
    release_playback_cache();
    char path[96]; snprintf(path,sizeof(path),"%s/%s",REC_DIR,name);
    take(); playback_cancel=false; playback_failed=false; give();
    status(REC_BUFFERING,"Loading entire recording into PSRAM...");
    uint32_t total=0;
    FILE *source=recording_open(path,&total);
    if(!source || !total) {
        if(source)fclose(source);
        status(REC_IDLE,"Invalid or empty recording");return;
    }
    /* Capture is inactive. Reuse its ring for short recordings; allocate one
     * complete contiguous cache for larger recordings. Never stream from NAND. */
    playback_cache=total<=RING_SIZE?ring:heap_caps_malloc(total,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!playback_cache) {
        ESP_LOGE(TAG,"Full playback cache requires %lu bytes, largest PSRAM block=%u",
                 (unsigned long)total,(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        fclose(source);status(REC_IDLE,"Recording too large for full PSRAM cache");return;
    }
    int64_t load_started=esp_timer_get_time();
    pcm_cache_result_t result=pcm_cache_load(source,playback_cache,total,cache_progress,(void *)name);
    int64_t load_us=esp_timer_get_time()-load_started;
    bool cancelled=result==PCM_CACHE_CANCELLED;
    bool failed=result!=PCM_CACHE_OK && !cancelled;
    if(fclose(source)!=0)failed=true;
    take(); cancelled|=playback_cancel; give();
    if(failed || cancelled) {
        release_playback_cache();
        status(REC_IDLE,cancelled?"Loading cancelled":"NAND preload failed; playback not started");return;
    }
    ESP_LOGI(TAG,"Full playback cache ready: %s, %lu bytes, %lu ms; NAND file closed",
             name,(unsigned long)total,(unsigned long)((uint64_t)total*1000/WAV_BYTES_PER_SECOND));
    ESP_LOGI(TAG,"NAND preload: %lld ms, %llu KiB/s",(long long)(load_us/1000),
             (unsigned long long)(load_us>0?(uint64_t)total*1000000/(1024*(uint64_t)load_us):0));
    playback_cache_size=total;
    snprintf(playback_cache_name,sizeof(playback_cache_name),"%s",name);
    start_cached_playback(name);
}
static void delete_recording(const char *name, recorder_mode_t mode)
{
    bool found=false;
    take();
    for(unsigned i=0;i<state.count;i++) if(!strcmp(name,state.files[i].name))found=true;
    give();
    if (!found || (mode!=REC_IDLE && mode!=REC_PLAYING)) return;
    /* Serialize file removal with playback; never unlink an open audio file. */
    if (mode==REC_PLAYING) stop_playback();
    char path[96]; snprintf(path,sizeof(path),REC_DIR "/%s",name);
    if (unlink(path)!=0) {
        ESP_LOGE(TAG,"delete %s failed: errno=%d (%s)",path,errno,strerror(errno));
        status(REC_IDLE,"Delete failed; file retained");
        return;
    }
    ESP_LOGI(TAG,"Deleted recording: %s (permanent)",path);
    char meta[128];
    if(recording_metadata_path(path,meta,sizeof(meta)) && unlink(meta)!=0 && errno!=ENOENT)
        ESP_LOGW(TAG,"Audio removed but metadata cleanup failed: %s",meta);
    if(!strcmp(name,playback_cache_name))release_playback_cache();
    take();
    for(unsigned i=0;i<state.count;i++)if(!strcmp(name,state.files[i].name)) {
        memmove(&state.files[i],&state.files[i+1],(state.count-i-1)*sizeof(state.files[0]));
        state.count--;state.revision++;break;
    }
    give();
    status(REC_IDLE,"Recording deleted");
}
static void worker(void *arg)
{
    (void)arg;
    esp_err_t err=nand_littlefs_mount("/nandflash");
    if(err!=ESP_OK) {status(REC_ERROR,"NAND LittleFS mount failed; not formatted");vTaskDelete(NULL);return;}
    if(mkdir(REC_DIR,0777)!=0 && errno!=EEXIST) {
        status(REC_ERROR,"Cannot access recordings directory");vTaskDelete(NULL);return;
    }
    int64_t recovery_started=esp_timer_get_time();
    recover_completed_parts();
    ESP_LOGI(TAG,"Recording recovery scan: %lld ms",(long long)((esp_timer_get_time()-recovery_started)/1000));
    if(!scan()) {status(REC_ERROR,"Cannot scan recordings directory");vTaskDelete(NULL);return;}
    int64_t audio_started=esp_timer_get_time();
    err=a1_rec_audio_init();
    ESP_LOGI(TAG,"A1 initialization/calibration: %lld ms",(long long)((esp_timer_get_time()-audio_started)/1000));
    if(err!=ESP_OK) {status(REC_ERROR,"A1 init failed; check subboard and reboot");vTaskDelete(NULL);return;}
    if(xTaskCreate(capture_task,"a1_capture",8192,NULL,8,NULL)!=pdPASS) {
        status(REC_ERROR,"Cannot start capture task");vTaskDelete(NULL);return;
    }
    playback_jobs=xQueueCreate(1,sizeof(playback_job_t));
    playback_done=xSemaphoreCreateBinary();
    if(!playback_jobs || !playback_done ||
       xTaskCreate(playback_task,"a1_playback",6144,NULL,9,NULL)!=pdPASS) {
        status(REC_ERROR,"Cannot start RAM playback task");vTaskDelete(NULL);return;
    }
    err=recorder_key_start();
    status(REC_IDLE,err==ESP_OK?"Ready - tap TK7 to record":"TK7 unavailable; use screen controls");
    uint8_t pcm[3072];
    for(;;) {
        command_t c; recorder_mode_t mode;
        take(); mode=state.mode; give();
        if(xQueueReceive(commands,&c,mode==REC_IDLE?portMAX_DELAY:mode==REC_PLAYING?pdMS_TO_TICKS(10):0)==pdTRUE) {
            take(); bool current=c.generation==generation; give();
            if(!current && c.type<=3)continue;
            if(c.type==1) {
                if(mode==REC_RECORDING || mode==REC_SAVING)finish_record(false);
                else {if(mode==REC_PLAYING)stop_playback();begin_record();}
            } else if(c.type==2) {
                if(mode==REC_RECORDING || mode==REC_SAVING)finish_record(false);
                else if(mode==REC_PLAYING)stop_playback();
            } else if(c.type==3 && mode!=REC_RECORDING) {
                if(mode==REC_PLAYING)stop_playback();
                begin_play(c.name);
            } else if(c.type==4 || c.type==5) {
                take(); unsigned volume=state.volume; give();
                volume=c.type==5?(volume>=95?100:volume+5):(volume<=5?0:volume-5);
                if(a1_rec_audio_set_volume(volume)==ESP_OK) {
                    take(); state.volume=volume; give();
                    ESP_LOGI(TAG,"Playback volume: %u%%",volume);
                }
            } else if(c.type==6) {
                delete_recording(c.name,mode);
            }
            continue;
        }
        if(mode==REC_RECORDING) {
            take(); bool failed=capture_error; give();
            size_t n=drain(pcm,sizeof(pcm));
            bool write_failed=n && !write_pcm(pcm,n);
            if(failed || write_failed) {finish_record(write_failed);continue;}
            if(!n)vTaskDelay(pdMS_TO_TICKS(5));
        } else if(mode==REC_PLAYING) {
            if(xSemaphoreTake(playback_done,0)==pdTRUE) {
                take(); bool failed=playback_failed; give();
                playback_active=false;
                status(REC_IDLE,failed?"A1 RAM playback failed":"Playback complete");
            }
        }
    }
}
esp_err_t recorder_start(void)
{
    lock=xSemaphoreCreateMutex(); commands=xQueueCreate(8,sizeof(command_t));
    ring=heap_caps_malloc(RING_SIZE,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!lock || !commands || !ring)return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG,"Capture buffer: %u bytes (%u seconds at 48kHz stereo 24-bit)",
             (unsigned)RING_SIZE,(unsigned)(RING_SIZE/WAV_BYTES_PER_SECOND));
    state.volume=60;
    status(REC_STARTING,"Initializing NAND and A1...");
    return xTaskCreate(worker,"recorder",12288,NULL,5,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
