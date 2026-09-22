idf_component_register(
    SRCS "mosaico_module_interact.c"
    INCLUDE_DIRS "include"
    REQUIRES led_strip esp_adc esp_driver_gpio esp_driver_rmt touch_button touch_sensor_lowlevel
)
