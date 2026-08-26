idf_component_register(
    SRCS
        "main.c"
        "interaction_controller.c"
        "interaction_ui.c"
        "diagnostic_stream.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp-mosaico-bsp
        mosaico_interaction
        mosaico_network
        mosaico_peer_link
)
