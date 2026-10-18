idf_component_register(SRCS "katmed_store.cpp" "katmed_store_idf.c"
    INCLUDE_DIRS "include" REQUIRES katmed_core nvs_flash esp_rom)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
