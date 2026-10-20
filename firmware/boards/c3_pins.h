#ifndef KATMED_C3_PINS_H
#define KATMED_C3_PINS_H

/* ESP32-C3 1.28-inch touch unit, installed GUITION factory firmware.
 * Public touch factory image SHA-256:
 * cdc9e32253331bbcd1654f58519e6c72630cba424c625aef92daa2f0e21c96fa
 * All 2,960,400 public bytes match the start of the saved local flash. The
 * active LCD, backlight and touch setup calls were independently disassembled
 * and traced. See 2026-10-14-Biene-Hardware-Nachweise.md and
 * 2026-10-14-Biene-Bildpfad-Unabhaengige-Pruefung.md for evidence and limits.
 * GPIO/protocol use is proven; the exact PCB revision and touch IC marking
 * remain unknown. No physical test with Katmed firmware is implied. */
#define KATMED_C3_LCD_SCK 6
#define KATMED_C3_LCD_MOSI 7
#define KATMED_C3_LCD_DC 2
#define KATMED_C3_LCD_CS 10
#define KATMED_C3_LCD_RESET -1
#define KATMED_C3_LCD_BACKLIGHT 3
#define KATMED_C3_LCD_BACKLIGHT_ACTIVE_HIGH 1
#define KATMED_C3_LCD_WIDTH 240
#define KATMED_C3_LCD_HEIGHT 240
#define KATMED_C3_LCD_SPI_MODE 0
#define KATMED_C3_LCD_INVERT_COLORS 1
/* Candidate panel X calibration; the physical effect needs a sight test. */
#define KATMED_C3_PANEL_MIRROR_X_AT_ZERO 1

#define KATMED_C3_TOUCH_SDA 4
#define KATMED_C3_TOUCH_SCL 5
#define KATMED_C3_TOUCH_RESET 1
#define KATMED_C3_TOUCH_INTERRUPT 0
#define KATMED_C3_TOUCH_I2C_ADDRESS_7BIT 0x15

/* GUITION ESP32-2424S012 V1.0: BOOT switch pulls IO9 low; R2 pulls it
 * high. This is a ROM strap during reset, sampled as input only in app. */
#define KATMED_C3_BOOT_GPIO 9

#endif
