idf_component_register(
    SRCS "mosaico_joystick.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_adc
        esp_driver_gpio
)
