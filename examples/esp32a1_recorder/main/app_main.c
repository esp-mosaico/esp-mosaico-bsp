#include "recorder.h"
#include "bsp/esp_mosaico.h"
#include "esp_log.h"
void app_main(void)
{
    bsp_board_variant_t variant;
    ESP_ERROR_CHECK(bsp_board_variant_get(&variant));
    if(variant!=BSP_BOARD_VARIANT_V1_2) {
        ESP_LOGE("recorder","Requires ESP-Mosaico V1.2");return;
    }
    /* Complete the shared GPIO60 power ramp before display/recorder tasks race
     * to initialize peripherals. BSP power initialization is not mutex-guarded. */
    ESP_ERROR_CHECK(bsp_power_set_vcc_3v3(true));
    ESP_ERROR_CHECK(recorder_start());
    ESP_ERROR_CHECK(recorder_ui_start());
}
