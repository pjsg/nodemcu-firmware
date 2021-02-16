idf_component_register(SRC_DIRS . PRIV_REQUIRES platform INCLUDE_DIRS include)
target_compile_options(${COMPONENT_LIB} PUBLIC -D__ESP32__)
