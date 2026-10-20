idf_component_register(SRCS "katmed_reset_journal.c" "katmed_reset_idf.c"
    "katmed_reset_acme_files.c"
    INCLUDE_DIRS "include" REQUIRES nvs_flash katmed_security katmed_images)
