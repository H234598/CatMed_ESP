idf_component_register(SRCS "katmed_reset_journal.c" "katmed_reset_idf.c"
    "katmed_reset_acme_files.c"
    INCLUDE_DIRS "include" REQUIRES katmed_names nvs_flash katmed_security katmed_images joltwallet__littlefs)
idf_build_get_property(KATMED_APPSTORE_MAINTENANCE KATMED_APPSTORE_MAINTENANCE)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    KATMED_APPSTORE_MAINTENANCE=$<BOOL:${KATMED_APPSTORE_MAINTENANCE}>)
