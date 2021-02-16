idf_component_register(SRC_DIRS qrcodegen/c INCLUDE_DIRS qrcodegen/c)
target_compile_options(${COMPONENT_LIB} PRIVATE -D__ESP32__)
