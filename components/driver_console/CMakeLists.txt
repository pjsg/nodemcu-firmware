idf_component_register(SRC_DIRS . PRIV_REQUIRES task INCLUDE_DIRS include)
target_compile_options(${COMPONENT_LIB} PRIVATE -D__ESP32__)
