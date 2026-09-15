idf_component_register(
    SRCS
        "main.c"
        "slot_panes.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_ppa
        esp_mm
)
