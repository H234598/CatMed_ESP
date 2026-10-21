idf_component_register(SRCS "katmed_nvs_init.c" "katmed_source_export.c" INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_partition bootloader_support esp_rom esp_timer mbedtls)
