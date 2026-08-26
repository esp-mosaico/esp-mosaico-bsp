idf_component_register(
    SRCS "main.c" "app_settings.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash
)
