idf_component_register(
    SRCS "mosaico_joystick.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp-mosaico-bsp
        mosaico_module_mgr
        esp_adc
        esp_driver_gpio
)
