idf_component_register(SRCS "katmed_import.cpp" "katmed_import_idf.c"
    "katmed_import_files_idf.c"
    INCLUDE_DIRS "include"
    REQUIRES katmed_store katmed_mesh katmed_security nvs_flash esp_rom esp_hw_support
    PRIV_REQUIRES katmed_web joltwallet__littlefs)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
