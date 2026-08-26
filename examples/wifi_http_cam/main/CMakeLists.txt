idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_wifi
        esp_http_server
        nvs_flash
        esp_netif
        esp_event
)
