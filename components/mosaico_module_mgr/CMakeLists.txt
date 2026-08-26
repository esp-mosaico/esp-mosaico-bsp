idf_component_register(
    SRCS "mosaico_module_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-mosaico-bsp esp_driver_i2c
)
