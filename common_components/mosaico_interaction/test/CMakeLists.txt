idf_component_register(
    SRCS
        "test_mosaico_mag_classifier.c"
        "test_mosaico_game.c"
    INCLUDE_DIRS "."
    REQUIRES unity mosaico_interaction
)
