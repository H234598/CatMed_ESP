set(KATMED_CATALOG_TARGET "c3")
if(IDF_TARGET STREQUAL "esp32c6")
    set(KATMED_CATALOG_TARGET "c6")
endif()

set(KATMED_ASSET_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../assets/generated")
set(KATMED_CATALOG_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/katmed_catalog_generated.c")
# IDF also reads this file in script mode to collect component requirements.
# Build rules are valid only in the actual project configuration phase.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    file(GLOB KATMED_CATALOG_JPEGS "${KATMED_ASSET_ROOT}/*/*.jpg")
    add_custom_command(
        OUTPUT "${KATMED_CATALOG_SOURCE}"
        COMMAND /usr/bin/python3 "${CMAKE_CURRENT_LIST_DIR}/../../../tools/generate_firmware_catalog.py"
                --target "${KATMED_CATALOG_TARGET}"
                --manifest "${KATMED_ASSET_ROOT}/manifest.json"
                --output "${KATMED_CATALOG_SOURCE}"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../../tools/generate_firmware_catalog.py"
                "${CMAKE_CURRENT_LIST_DIR}/../../../tools/prepare_images.py"
                "${CMAKE_CURRENT_LIST_DIR}/../../../assets/crop_focus.json"
                "${KATMED_ASSET_ROOT}/manifest.json"
                ${KATMED_CATALOG_JPEGS}
        VERBATIM
        COMMENT "Validating all Katmed JPEGs and generating ${KATMED_CATALOG_TARGET} firmware index")
endif()

idf_component_register(
    SRCS "katmed_images_hash.c" "katmed_images_reader.c" "katmed_images_lvgl.c"
         "katmed_images_idf.c" "katmed_image_store_idf.c"
         "katmed_image_store_cleanup.c" "${KATMED_CATALOG_SOURCE}"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES katmed_core lvgl esp_partition
    PRIV_REQUIRES mbedtls esp_timer joltwallet__littlefs katmed_security)
