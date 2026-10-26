idf_component_register(
    SRCS "katmed_web.c" "katmed_web_acme_route.c" "katmed_web_auth.c" "katmed_web_identity.c"
         "katmed_web_policy.c" "katmed_web_wifi.c" "katmed_web_reset.c"
         "katmed_web_media.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES katmed_config_bridge katmed_core katmed_security
    PRIV_REQUIRES esp_rom esp_https_server esp_http_server esp_netif esp-tls mbedtls nvs_flash cjson lwip
                  esp_timer esp_hw_support esp_system katmed_images)
target_add_binary_data(${COMPONENT_LIB} "ui/index.html" BINARY)
target_add_binary_data(${COMPONENT_LIB} "ui/style.css" BINARY)
if(CONFIG_IDF_TARGET_ESP32C6 AND CONFIG_SECURE_FLASH_ENC_ENABLED)
    # Keep the full browser scripts and language catalog, but store their wire
    # representation in gzip form in the size-constrained C6 security image.
    set(_katmed_gzip_dir "${CMAKE_CURRENT_BINARY_DIR}/security_js")
    set(_katmed_gzip_tool "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/build_web_gzip.py")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/app.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/i18n.js"
        "${_katmed_gzip_tool}")
    execute_process(
        COMMAND "${PYTHON}" "${_katmed_gzip_tool}"
            --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/ui"
            --output-dir "${_katmed_gzip_dir}"
        RESULT_VARIABLE _katmed_gzip_result)
    if(NOT _katmed_gzip_result EQUAL 0)
        message(FATAL_ERROR "Building C6 security browser assets failed")
    endif()
    target_add_binary_data(${COMPONENT_LIB} "${_katmed_gzip_dir}/app.js.gz" BINARY
        RENAME_TO app_js)
    target_add_binary_data(${COMPONENT_LIB} "${_katmed_gzip_dir}/i18n.js.gz" BINARY
        RENAME_TO i18n_js)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KATMED_WEB_SECURITY_JS_GZIP=1)
else()
    target_add_binary_data(${COMPONENT_LIB} "ui/app.js" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "ui/i18n.js" BINARY)
endif()
