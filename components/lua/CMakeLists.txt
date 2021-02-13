idf_component_register(SRC_DIRS . PRIV_REQUIRES base_nodemcu driver_console platform uzlib INCLUDE_DIRS .)
target_compile_options(${COMPONENT_LIB} PRIVATE -D__ESP32__)
