# Compatibility component for current Mesh Lite 1.0.2 sources, which still
# request ESP-IDF's former `json` component name. cJSON now resolves from the
# Espressif component registry as `cjson`.
idf_component_register(REQUIRES cjson)
