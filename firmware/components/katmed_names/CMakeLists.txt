idf_component_register(SRCS "katmed_names.cpp" "katmed_names_wire.cpp" "katmed_names_idf.c" INCLUDE_DIRS "include"
    REQUIRES katmed_store mbedtls esp_rom katmed_security joltwallet__littlefs)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

# Scoped size optimization; the supplied toolchain has no LTO linker plugin.
# SDK, crypto options, languages and assets remain unchanged.
target_compile_options(${COMPONENT_LIB} PRIVATE -Oz)
