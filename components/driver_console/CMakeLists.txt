idf_component_register(SRC_DIRS . REQUIRES task INCLUDE_DIRS include PRIV_REQUIRES esp_rom)
target_compile_options(${COMPONENT_LIB} PRIVATE -D__ESP32__)
