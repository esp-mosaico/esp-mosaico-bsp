idf_component_register(
    SRCS
        "test_mosaico_peer_session.c"
        "test_mosaico_mesh.c"
    INCLUDE_DIRS "."
    REQUIRES unity mosaico_peer_link
)
