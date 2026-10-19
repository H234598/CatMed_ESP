idf_component_register(
    SRCS "katmed_mesh_wire.cpp" "katmed_mesh_payload.cpp" "katmed_idf6_netif_compat.c"
         "katmed_mesh_status.cpp" "katmed_mesh_session.cpp" "katmed_mesh_idf.cpp" "katmed_dhcp_diag.cpp"
    INCLUDE_DIRS "include"
    REQUIRES katmed_store katmed_core mesh_lite espressif__iot_bridge
             mbedtls esp_event esp_netif esp_app_format lwip heap freertos
)

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=udp_sendto_if_src" "-Wl,--wrap=esp_netif_transmit_wrap" "-Wl,--wrap=heap_caps_malloc"
    "-Wl,-u,esp_netif_next")
