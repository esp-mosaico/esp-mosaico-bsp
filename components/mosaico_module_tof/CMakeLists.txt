idf_component_register(
    SRCS "mosaico_tof.c" "private/vl53l4cd/vl53l4cd_api.c" "private/vl53l4cd/platform.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private/vl53l4cd"
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_timer freertos)
include(driver_fix.cmake)
