idf_component_register(
    SRCS "navigation.c" "clock_view.c" "ui.c" "morse.c" "i18n_catalog.c" "i18n_date.c"
         "katmed_font_10.c" "katmed_font_12.c" "katmed_font_14.c"
         "katmed_font_26.c" "katmed_font_i18n_12.c" "katmed_font_i18n_14.c"
    INCLUDE_DIRS "include"
    REQUIRES katmed_core katmed_images lvgl__lvgl esp_timer)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
