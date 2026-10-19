# ESP-IDF 6.1 moved and renamed wifi_provisioning to network_provisioning.
# Mesh Lite 1.0.2 still lists the former name unconditionally, although its
# provisioning sources are disabled by the Katmed board defaults.
if(CONFIG_MESH_LITE_PROV_ENABLE)
    message(FATAL_ERROR
        "Mesh Lite 1.0.2 uses the removed wifi_provisioning API; migrate that optional feature before enabling it")
endif()
idf_component_register()
