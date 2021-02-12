idf_component_register(SRC_DIRS . PRIV_REQUIRES spi_flash lua platform driver_console INCLUDE_DIRS include)
target_compile_options(${COMPONENT_LIB} PRIVATE -D__ESP32__)
