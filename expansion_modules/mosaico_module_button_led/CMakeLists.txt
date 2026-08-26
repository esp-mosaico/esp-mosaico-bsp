idf_component_register(
    SRCS "mosaico_button_led.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp-mosaico-bsp
        mosaico_module_mgr
        led_strip
)
