idf_component_register(
    SRCS "katmed_web.c" "katmed_web_auth.c" "katmed_web_identity.c"
         "katmed_web_policy.c" "katmed_web_wifi.c" "katmed_web_reset.c"
         "katmed_web_media.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES katmed_config_bridge
    PRIV_REQUIRES esp_https_server esp_http_server esp-tls mbedtls nvs_flash cjson
                  esp_timer esp_hw_support esp_system katmed_images)
target_add_binary_data(${COMPONENT_LIB} "ui/index.html" BINARY)
target_add_binary_data(${COMPONENT_LIB} "ui/style.css" BINARY)
target_add_binary_data(${COMPONENT_LIB} "ui/app.js" BINARY)
