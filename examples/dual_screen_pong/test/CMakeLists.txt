idf_component_register(
    SRCS
        "test_pong_physics.c"
        "test_pong_protocol.c"
        "../main/pong_physics.c"
        "../main/pong_protocol.c"
    INCLUDE_DIRS
        "."
        "../main"
    REQUIRES unity
)
