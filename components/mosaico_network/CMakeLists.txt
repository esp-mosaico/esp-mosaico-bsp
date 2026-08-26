idf_component_register(
    SRCS "mosaico_network.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_timer
        freertos
        mosaico_interaction
        mosaico_peer_link
)
