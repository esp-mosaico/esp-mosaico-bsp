idf_component_register(
    SRCS
        "mosaico_peer_link.c"
        "mosaico_peer_session.c"
        "mosaico_mesh.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_event
        esp_netif
        esp_wifi
        freertos
        mosaico_interaction
        nvs_flash
)
