if(CONFIG_IDF_TARGET STREQUAL "esp32")
idf_component_register(SRC_DIRS . REQUIRES task INCLUDE_DIRS include)
target_compile_options(${COMPONENT_LIB} PRIVATE -D__ESP32__)
endif()
