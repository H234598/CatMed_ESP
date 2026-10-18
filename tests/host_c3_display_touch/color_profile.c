#include <assert.h>
#include <stdio.h>
#include "sdk_stubs.h"
#define LCD_RGB_ELEMENT_ORDER_BGR 1
#include "../../firmware/components/board/board.c"

static int captured_order = -1;
int spi_bus_initialize(int bus, const spi_bus_config_t *config, int dma)
{ (void)bus; (void)config; (void)dma; return ESP_OK; }
int esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
    const esp_lcd_panel_io_spi_config_t *config, esp_lcd_panel_io_handle_t *out)
{ (void)bus; (void)config; *out = (void *)1; return ESP_OK; }
int esp_lcd_new_panel_gc9a01(esp_lcd_panel_io_handle_t io,
    const esp_lcd_panel_dev_config_t *config, esp_lcd_panel_handle_t *out)
{ (void)io; captured_order = config->rgb_ele_order; *out = (void *)2; return ESP_OK; }
int esp_lcd_new_panel_jd9853(esp_lcd_panel_io_handle_t io,
    const esp_lcd_panel_dev_config_t *config, esp_lcd_panel_handle_t *out)
{ (void)io; captured_order = config->rgb_ele_order; *out = (void *)2; return ESP_OK; }
int esp_lcd_panel_reset(esp_lcd_panel_handle_t handle)
{ (void)handle; return ESP_OK; }
int esp_lcd_panel_init(esp_lcd_panel_handle_t handle)
{ (void)handle; return ESP_OK; }
int esp_lcd_panel_set_gap(esp_lcd_panel_handle_t handle, int x, int y)
{ (void)handle; (void)x; (void)y; return ESP_OK; }
int esp_lcd_panel_invert_color(esp_lcd_panel_handle_t handle, int invert)
{ (void)handle; assert(invert); return ESP_OK; }
int esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t handle, int on)
{ (void)handle; assert(on); return ESP_OK; }

int main(void)
{
    assert(init_panel() == ESP_OK);
#if CONFIG_IDF_TARGET_ESP32C3
    fprintf(stderr, "c3_madctl_order=%d expected_bgr=%d\n",
            captured_order, LCD_RGB_ELEMENT_ORDER_BGR);
    assert(captured_order == LCD_RGB_ELEMENT_ORDER_BGR);
#else
    fprintf(stderr, "c6_madctl_order=%d expected_rgb=%d\n",
            captured_order, LCD_RGB_ELEMENT_ORDER_RGB);
    assert(captured_order == LCD_RGB_ELEMENT_ORDER_RGB);
#endif
    return 0;
}
