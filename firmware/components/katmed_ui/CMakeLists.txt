idf_build_get_property(KATMED_C3_ALARM_DIAG KATMED_C3_ALARM_DIAG)
if(KATMED_C3_ALARM_DIAG)
    set(diagnostic_port espressif__esp_lvgl_port)
endif()
idf_component_register(
    SRCS "navigation.c" "clock_view.c" "ui.c" "morse.c" "i18n_catalog.c" "i18n_date.c"
         "katmed_font_10.c" "katmed_font_12.c" "katmed_font_14.c"
         "katmed_font_26.c" "katmed_font_i18n_12.c" "katmed_font_i18n_14.c"
    INCLUDE_DIRS "include"
    REQUIRES katmed_core katmed_images lvgl__lvgl esp_timer
    PRIV_REQUIRES ${diagnostic_port})
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
if(KATMED_C3_ALARM_DIAG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KATMED_C3_ALARM_DIAG=1)
endif()
idf_build_get_property(KATMED_APPSTORE_MAINTENANCE KATMED_APPSTORE_MAINTENANCE)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    KATMED_APPSTORE_MAINTENANCE=$<BOOL:${KATMED_APPSTORE_MAINTENANCE}>)
