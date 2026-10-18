#ifndef REVIEW_SDK_STUBS_H
#define REVIEW_SDK_STUBS_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "lvgl.h"
typedef int esp_err_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef void *esp_lcd_panel_handle_t;
typedef void *esp_lcd_spi_bus_handle_t;
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef void *esp_lcd_touch_handle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define ESP_OK 0
#define ESP_ERR_INVALID_RESPONSE -1
#define I2C_NUM_0 0
#define I2C_CLK_SRC_DEFAULT 0
#define I2C_ADDR_BIT_LEN_7 0
#define GPIO_MODE_OUTPUT 1
#define GPIO_MODE_INPUT 2
#define SPI2_HOST 1
#define SPI_DMA_CH_AUTO 0
#define LCD_RGB_ELEMENT_ORDER_RGB 0
#define LCD_RGB_ELEMENT_ORDER_BGR 1
#define ESP_LCD_TOUCH_IO_I2C_AXS5106_ADDRESS 0x63
#define pdMS_TO_TICKS(x) (x)
#define GC9A01_PANEL_IO_SPI_CONFIG(...) {0}
#define JD9853_PANEL_IO_SPI_CONFIG(...) {0}
#define ESP_LVGL_PORT_INIT_CONFIG() {0}
typedef struct {int i2c_port,sda_io_num,scl_io_num,clk_source,glitch_ignore_cnt;struct {bool enable_internal_pullup;} flags;} i2c_master_bus_config_t;
typedef struct {int dev_addr_length,device_address,scl_speed_hz;} i2c_device_config_t;
typedef struct {uint64_t pin_bit_mask;int mode;} gpio_config_t;
typedef struct {int x_max,y_max,rst_gpio_num,int_gpio_num;} esp_lcd_touch_config_t;
typedef struct {int sclk_io_num,mosi_io_num,miso_io_num,quadwp_io_num,quadhd_io_num,max_transfer_sz;} spi_bus_config_t;
typedef struct {int dummy;} esp_lcd_panel_io_spi_config_t;
typedef struct {int reset_gpio_num,rgb_ele_order,bits_per_pixel;} esp_lcd_panel_dev_config_t;
typedef struct {int dummy;} lvgl_port_cfg_t;
typedef struct {void*io_handle,*panel_handle;unsigned buffer_size;bool double_buffer;int hres,vres,color_format;struct {bool buff_dma,swap_bytes;} flags;} lvgl_port_display_cfg_t;
int64_t esp_timer_get_time(void);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t,const void*,size_t,void*,size_t,int);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t,const void*,size_t,int);
esp_err_t i2c_master_receive(i2c_master_dev_handle_t,void*,size_t,int);
bool lvgl_port_lock(unsigned);
void lvgl_port_unlock(void);
int esp_lcd_panel_set_gap();
int esp_lcd_panel_swap_xy();
int esp_lcd_panel_mirror();
int i2c_new_master_bus(); int i2c_master_bus_add_device(); int gpio_config(); int gpio_set_level();
void esp_rom_delay_us(); void vTaskDelay(); int esp_lcd_touch_new_i2c_axs5106(); int i2c_master_probe();
int spi_bus_initialize(); int esp_lcd_new_panel_io_spi(); int esp_lcd_new_panel_gc9a01(); int esp_lcd_new_panel_jd9853();
int esp_lcd_panel_reset(); int esp_lcd_panel_init(); int esp_lcd_panel_invert_color(); int esp_lcd_panel_disp_on_off();
int lvgl_port_remove_disp(); int lvgl_port_deinit(); int esp_lcd_touch_del(); int i2c_master_bus_rm_device();
int i2c_del_master_bus(); int esp_lcd_panel_del(); int esp_lcd_panel_io_del(); int spi_bus_free(); int gpio_reset_pin();
int lvgl_port_init(); lv_display_t *lvgl_port_add_disp();
#endif
