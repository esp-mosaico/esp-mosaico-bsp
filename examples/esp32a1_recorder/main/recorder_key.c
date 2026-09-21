#include "recorder.h"
#include "bsp/esp_mosaico.h"
#include "si12t.h"
#include "key_click.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static si12t_handle_t touch;
static void key_task(void *arg)
{
    (void)arg;
    key_click_t click={0}, down={0}, up={0};
    for (;;) {
        uint16_t keys=0;
        bool valid=si12t_read_pressed(touch,&keys)==ESP_OK;
        if(key_click_update(&down,valid,(keys&(1U<<4))!=0)) {
            if(!recorder_volume(false))ESP_LOGW("recorder_key","volume command unavailable");
        }
        if(key_click_update(&up,valid,(keys&(1U<<8))!=0)) {
            if(!recorder_volume(true))ESP_LOGW("recorder_key","volume command unavailable");
        }
        if(key_click_update(&click,valid,(keys&(1U<<6))!=0)) {
            if(!recorder_toggle())ESP_LOGW("recorder_key","command queue full");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
esp_err_t recorder_key_start(void)
{
    esp_err_t err=bsp_subboard_init();
    if(err!=ESP_OK)return err;
    si12t_config_t config=SI12T_CONFIG_DEFAULT(bsp_subboard_get_i2c_bus());
    err=si12t_create(&config,&touch);
    if(err!=ESP_OK)return err;
    err=si12t_set_sensitivity(touch,0x01f0,0x04);
    if(err==ESP_OK && xTaskCreate(key_task,"recorder_key",4096,NULL,4,NULL)!=pdPASS)err=ESP_ERR_NO_MEM;
    if(err!=ESP_OK) {si12t_delete(touch);touch=NULL;}
    return err;
}
