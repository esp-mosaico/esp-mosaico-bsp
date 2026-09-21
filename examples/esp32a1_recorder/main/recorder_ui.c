#include "recorder.h"
#include "bsp/esp_mosaico.h"
#include "lvgl.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static lv_obj_t *status_label,*name_label,*time_label,*list,*record_button,*stop_button,*record_label,*count_label;
static recorder_state_t snapshot;
static char names[REC_MAX_FILES][32];
static lv_obj_t *play_buttons[REC_MAX_FILES];
static lv_obj_t *delete_buttons[REC_MAX_FILES], *volume_label, *delete_modal, *delete_prompt;
static char delete_name[32];
static void show_delete_confirmation(const char *name);
static void close_delete_confirmation(void)
{
    if (delete_modal) lv_obj_delete(delete_modal);
    delete_modal=NULL;
    delete_prompt=NULL;
    delete_name[0]='\0';
}
static uint32_t revision=UINT32_MAX;
static lv_obj_t *label(lv_obj_t *parent,const char *text,int x,int y,const lv_font_t *font)
{
    lv_obj_t *obj=lv_label_create(parent); lv_label_set_text(obj,text);
    lv_obj_set_pos(obj,x,y); lv_obj_set_style_text_font(obj,font,0);
    lv_obj_set_style_text_color(obj,lv_color_hex(0xe9edf5),0); return obj;
}
static void clicked(lv_event_t *e)
{
    uintptr_t action=(uintptr_t)lv_event_get_user_data(e);
    if(action==1)recorder_toggle();
    else if(action==2)recorder_stop();
    else if(action>=3 && action-3<REC_MAX_FILES)recorder_play(names[action-3]);
    else if(action>=1000 && action-1000<REC_MAX_FILES)show_delete_confirmation(names[action-1000]);
    else if(action==2000) {
        if(recorder_delete(delete_name)) close_delete_confirmation();
        else lv_label_set_text(delete_prompt,"Recorder busy. Cancel and try again.");
    } else if(action==2001)close_delete_confirmation();
}
static lv_obj_t *button(lv_obj_t *parent,int x,int y,int w,int h,uintptr_t action)
{
    lv_obj_t *obj=lv_button_create(parent); lv_obj_set_pos(obj,x,y);lv_obj_set_size(obj,w,h);
    lv_obj_add_event_cb(obj,clicked,LV_EVENT_CLICKED,(void *)action);return obj;
}
static void show_delete_confirmation(const char *name)
{
    if(delete_modal)return;
    snprintf(delete_name,sizeof(delete_name),"%s",name);
    delete_modal=lv_obj_create(lv_layer_top());
    lv_obj_set_size(delete_modal,LV_PCT(100),LV_PCT(100));
    lv_obj_set_style_bg_color(delete_modal,lv_color_hex(0x000000),0);
    lv_obj_set_style_bg_opa(delete_modal,LV_OPA_70,0);
    lv_obj_set_style_border_width(delete_modal,0,0);
    lv_obj_remove_flag(delete_modal,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *card=lv_obj_create(delete_modal);
    lv_obj_set_size(card,400,232);lv_obj_center(card);
    lv_obj_set_style_bg_color(card,lv_color_hex(0x203248),0);
    lv_obj_remove_flag(card,LV_OBJ_FLAG_SCROLLABLE);
    label(card,"Delete recording?",0,0,&lv_font_montserrat_20);
    delete_prompt=label(card,"",0,40,&lv_font_montserrat_16);
    lv_obj_set_width(delete_prompt,352);
    lv_label_set_text_fmt(delete_prompt,"%s\n\nThis cannot be undone.",delete_name);
    lv_obj_t *cancel=button(card,0,148,165,46,2001);
    lv_obj_t *text=label(cancel,"Cancel",0,0,&lv_font_montserrat_16);lv_obj_center(text);
    lv_obj_t *confirm=button(card,187,148,165,46,2000);
    lv_obj_set_style_bg_color(confirm,lv_color_hex(0xb52f52),0);
    text=label(confirm,"Delete",0,0,&lv_font_montserrat_16);lv_obj_center(text);
}
static void refresh(lv_timer_t *timer)
{
    (void)timer; recorder_snapshot(&snapshot);
    bool recording=snapshot.mode==REC_RECORDING;
    bool ready=snapshot.mode==REC_IDLE || recording || snapshot.mode==REC_PLAYING;
    if(delete_modal && (recording || !ready))close_delete_confirmation();
    lv_label_set_text_fmt(volume_label,"VOL %u%%  TK5- / TK9+",snapshot.volume);
    lv_label_set_text(status_label,recording?"RECORDING":snapshot.mode==REC_BUFFERING?"BUFFERING":snapshot.mode==REC_SAVING?"SAVING":snapshot.mode==REC_PLAYING?"PLAYING":snapshot.mode==REC_IDLE?"READY":snapshot.mode==REC_ERROR?"ERROR":"STARTING");
    lv_obj_set_style_text_color(status_label,lv_color_hex(recording?0xff667f:0x64dfb5),0);
    lv_label_set_text(name_label,snapshot.message);
    lv_label_set_text_fmt(time_label,"%02lu:%02lu",(unsigned long)(snapshot.seconds/60),(unsigned long)(snapshot.seconds%60));
    lv_label_set_text(record_label,recording?"Finish recording":"Record");
    if(ready)lv_obj_remove_state(record_button,LV_STATE_DISABLED);else lv_obj_add_state(record_button,LV_STATE_DISABLED);
    if(recording || snapshot.mode==REC_PLAYING || snapshot.mode==REC_BUFFERING)lv_obj_remove_state(stop_button,LV_STATE_DISABLED);else lv_obj_add_state(stop_button,LV_STATE_DISABLED);
    if(revision!=snapshot.revision) {
        revision=snapshot.revision; lv_obj_clean(list);
        memset(play_buttons,0,sizeof(play_buttons));
        memset(delete_buttons,0,sizeof(delete_buttons));
        lv_label_set_text_fmt(count_label,"RECORDINGS  /  %u",snapshot.count);
        if(!snapshot.count)label(list,"No recordings yet. Tap TK7 to record.",4,6,&lv_font_montserrat_14);
        for(unsigned i=0;i<snapshot.count;i++) {
            snprintf(names[i],sizeof(names[i]),"%s",snapshot.files[i].name);
            lv_obj_t *row=lv_obj_create(list);
            lv_obj_set_pos(row,0,(int)i*66);lv_obj_set_size(row,396,60);
            lv_obj_set_style_pad_all(row,6,0);lv_obj_set_style_border_width(row,0,0);
            lv_obj_remove_flag(row,LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_color(row,lv_color_hex(0x203248),0);
            lv_obj_t *text=label(row,names[i],0,0,&lv_font_montserrat_16);
            lv_obj_set_width(text,200);lv_label_set_long_mode(text,LV_LABEL_LONG_DOT);
            lv_obj_align(text,LV_ALIGN_TOP_LEFT,2,0);
            text=lv_label_create(row);
            lv_label_set_text_fmt(text,"%02lu:%02lu",(unsigned long)(snapshot.files[i].seconds/60),(unsigned long)(snapshot.files[i].seconds%60));
            lv_obj_set_style_text_color(text,lv_color_hex(0xa7b7cd),0);
            lv_obj_align(text,LV_ALIGN_BOTTOM_LEFT,2,0);
            lv_obj_t *play=button(row,0,0,80,44,i+3);
            play_buttons[i]=play;lv_obj_align(play,LV_ALIGN_RIGHT_MID,-88,0);
            text=label(play,LV_SYMBOL_PLAY " Play",0,0,&lv_font_montserrat_16);lv_obj_center(text);
            lv_obj_t *del=button(row,0,0,80,44,i+1000);
            delete_buttons[i]=del;lv_obj_align(del,LV_ALIGN_RIGHT_MID,0,0);
            lv_obj_set_style_bg_color(del,lv_color_hex(0xb52f52),0);
            text=label(del,LV_SYMBOL_TRASH " Del",0,0,&lv_font_montserrat_16);lv_obj_center(text);
        }
    }
    for(unsigned i=0;i<lv_obj_get_child_count(list);i++) {
        lv_obj_t *row=lv_obj_get_child(list,i);
        if(recording || !ready)lv_obj_add_state(row,LV_STATE_DISABLED);else lv_obj_remove_state(row,LV_STATE_DISABLED);
    }
    for(unsigned i=0;i<snapshot.count;i++) {
        if(!play_buttons[i])continue;
        if(recording || !ready)lv_obj_add_state(play_buttons[i],LV_STATE_DISABLED);
        else lv_obj_remove_state(play_buttons[i],LV_STATE_DISABLED);
        if(recording || !ready)lv_obj_add_state(delete_buttons[i],LV_STATE_DISABLED);
        else lv_obj_remove_state(delete_buttons[i],LV_STATE_DISABLED);
    }
}
esp_err_t recorder_ui_start(void)
{
    bsp_display_config_t config=BSP_DISPLAY_DEFAULT_CONFIG();
    config.rotation=BSP_DISPLAY_ROTATE_270; config.enable_touch=true;
    if(!bsp_display_start_with_config(&config))return ESP_FAIL;
    if(!bsp_display_lock(-1))return ESP_ERR_TIMEOUT;
    lv_obj_t *screen=lv_screen_active();
    lv_obj_set_style_bg_color(screen,lv_color_hex(0x0b1422),0);
    lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    label(screen,"MOSAICO / RECORDER",24,20,&lv_font_montserrat_20);
    status_label=label(screen,"STARTING",24,60,&lv_font_montserrat_16);
    time_label=label(screen,"00:00",330,56,&lv_font_montserrat_28);
    name_label=label(screen,"Initializing...",24,101,&lv_font_montserrat_16);
    lv_obj_set_width(name_label,432);lv_label_set_long_mode(name_label,LV_LABEL_LONG_SCROLL_CIRCULAR);
    record_button=button(screen,24,139,270,55,1);
    lv_obj_set_style_bg_color(record_button,lv_color_hex(0xb52f52),0);
    record_label=label(record_button,"Record",0,0,&lv_font_montserrat_20);lv_obj_center(record_label);
    stop_button=button(screen,310,139,146,55,2);
    lv_obj_t *text=label(stop_button,LV_SYMBOL_STOP " Stop",0,0,&lv_font_montserrat_20);lv_obj_center(text);
    count_label=label(screen,"RECORDINGS",24,217,&lv_font_montserrat_14);
    volume_label=label(screen,"VOL 60%  TK5- / TK9+",250,217,&lv_font_montserrat_14);
    list=lv_obj_create(screen);lv_obj_set_pos(list,24,246);lv_obj_set_size(list,432,180);
    lv_obj_set_style_bg_color(list,lv_color_hex(0x111f30),0);lv_obj_set_style_border_width(list,0,0);
    lv_obj_set_scroll_dir(list,LV_DIR_VER);
    label(screen,"Tap TK7: start / finish recording",24,449,&lv_font_montserrat_14);
    lv_timer_create(refresh,100,NULL);refresh(NULL);
    bsp_display_unlock();return ESP_OK;
}
