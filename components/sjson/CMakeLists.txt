idf_component_register(SRC_DIRS . INCLUDE_DIRS . PRIV_REQUIRES platform)
target_compile_options(${COMPONENT_LIB} PRIVATE -D__ESP32__ -Wno-unused-const-variable)
