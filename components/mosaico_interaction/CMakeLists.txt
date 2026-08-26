idf_component_register(
    SRCS
        "mosaico_mag_classifier.c"
        "mosaico_interaction.c"
        "mosaico_topology.c"
        "mosaico_game.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
)
