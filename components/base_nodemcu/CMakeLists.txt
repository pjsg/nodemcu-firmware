idf_component_register(SRC_DIRS . PRIV_REQUIRES esp32 nvs_flash spi_flash lua platform driver_console INCLUDE_DIRS include)
target_compile_options(${COMPONENT_LIB} PRIVATE -D__ESP32__)
#target_linker_script(${COMPONENT_LIB} INTERFACE ld/nodemcu_rodata.ld)
target_link_libraries(${COMPONENT_LIB} INTERFACE ${CONFIG_IDF_TARGET})
