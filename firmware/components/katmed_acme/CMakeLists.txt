idf_component_register(
    SRCS "katmed_acme.cpp" "katmed_acme_atomic.c"
    INCLUDE_DIRS "include"
    REQUIRES acmeclient katmed_web
    PRIV_REQUIRES esp_timer freertos)
