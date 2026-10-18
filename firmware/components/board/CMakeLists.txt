set(board_reqs katmed_core esp_lcd esp_lvgl_port lvgl esp_timer driver esp_driver_spi esp_driver_i2c esp_driver_gpio espressif__esp_lcd_touch)
if(IDF_TARGET STREQUAL "esp32c6")
    list(APPEND board_reqs esp_lcd_jd9853 esp_lcd_touch_axs5106)
elseif(IDF_TARGET STREQUAL "esp32c3")
    list(APPEND board_reqs esp_lcd_gc9a01)
else()
    message(FATAL_ERROR "Katmed board supports only esp32c3 and esp32c6")
endif()
idf_component_register(SRCS "board.c" "board_geometry.c" "board_protocol.c" "board_indev_gate.c" "board_touch_buffer.c"
                       INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "../../boards"
                       REQUIRES ${board_reqs})
