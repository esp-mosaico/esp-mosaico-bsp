idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_ppa
        esp_lcd
        esp_mm
)
