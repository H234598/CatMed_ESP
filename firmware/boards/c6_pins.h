#ifndef KATMED_C6_PINS_H
#define KATMED_C6_PINS_H
/* Waveshare ESP32-C6-Touch-LCD-1.47, schematic + official 03_lvgl_example.
 * https://files.waveshare.com/wiki/ESP32-C6-Touch-LCD-1.47/ESP32-C6-Touch-LCD-1.47-Schematic.pdf
 * Schematic SHA-256: b679d1a6198ece26992c72a1b93a97ed2db6cc5f0066d041cb49150c5759c480
 * These definitions are evidence-backed; no physical acceptance is implied.
 * The C3 has its separate, independently verified c3_pins.h definition. */
#define KATMED_C6_LCD_SCK 1
#define KATMED_C6_LCD_MOSI 2
#define KATMED_C6_LCD_CS 14
#define KATMED_C6_LCD_DC 15
#define KATMED_C6_LCD_RESET 22
#define KATMED_C6_LCD_BACKLIGHT 23
#define KATMED_C6_TOUCH_SDA 18
#define KATMED_C6_TOUCH_SCL 19
#define KATMED_C6_TOUCH_RESET 20
#define KATMED_C6_TOUCH_INTERRUPT 21
/* Waveshare ESP32-C6-Touch-LCD-1.47 schematic: Key2 BOOT -> GPIO9,
 * with external R10 10K pull-up. Key1 ESP_RST is a separate reset line. */
#define KATMED_C6_BOOT_GPIO 9
#endif
