idf_component_register(SRCS "main.c" "eeprom_program.c" "program_ui.c"
                       INCLUDE_DIRS "." PRIV_REQUIRES esp-mosaico-bsp mosaico_module_mgr)
