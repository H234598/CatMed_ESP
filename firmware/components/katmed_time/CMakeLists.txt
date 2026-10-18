idf_component_register(SRCS "katmed_time.c" "katmed_time_idf.c"
                       INCLUDE_DIRS "include"
                       REQUIRES katmed_core esp_netif esp_timer freertos)
