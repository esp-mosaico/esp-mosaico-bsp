idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp-mosaico-bsp esp_audio_simple_player
    EMBED_FILES "alarm.mp3"
)
