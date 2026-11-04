set(srcs)
set(reqs esp-mosaico-bsp esp_blockdev)
if(CONFIG_MOSAICO_NAND_STORAGE_ENABLE)
    list(APPEND srcs src/mosaico_nand_storage.c src/storage_blockdev.c src/storage_volume.c)
    list(APPEND reqs joltwallet__littlefs)
    if(CONFIG_MOSAICO_NAND_STORAGE_RECLAIM)
        list(APPEND srcs src/storage_reclaim.c)
    endif()
endif()
idf_component_register(SRCS ${srcs}
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS private
    REQUIRES ${reqs}
    PRIV_REQUIRES esp_timer)
