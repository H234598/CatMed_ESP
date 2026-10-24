/* Execute the production init/read path. Only physical SDK calls are faked. */
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
static bool boot_input_ready;
#include "../../firmware/components/board/board.c"
static unsigned init_fault;
static bool transfer_failure;
static unsigned count, x;
int64_t esp_timer_get_time(void) {return 100000;}
int i2c_new_master_bus(const i2c_master_bus_config_t *c,i2c_master_bus_handle_t *out)
{assert(c->sda_io_num==18 && c->scl_io_num==19);*out=(void*)1;return init_fault==1?-2:ESP_OK;}
int i2c_master_bus_add_device(i2c_master_bus_handle_t b,const i2c_device_config_t *c,i2c_master_dev_handle_t *out)
{assert(b && c->device_address==0x63 && c->scl_speed_hz==400000);*out=(void*)1;return init_fault==2?-2:ESP_OK;}
int esp_lcd_touch_new_i2c_axs5106(i2c_master_dev_handle_t dev,const esp_lcd_touch_config_t *c,esp_lcd_touch_handle_t *out)
{assert(dev && c->rst_gpio_num==20 && c->int_gpio_num==21);*out=(void*)1;return init_fault==3?-2:ESP_OK;}
int i2c_master_probe(i2c_master_bus_handle_t b,unsigned addr,int timeout)
{assert(b && addr==0x63 && timeout==100);return init_fault==4?-2:ESP_OK;}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,const void *reg,size_t n,void *out,size_t len,int timeout)
{assert(dev && n==1 && *(const uint8_t*)reg==1 && len==14 && timeout==100);if(transfer_failure)return -2;
 uint8_t *p=out;memset(p,0,len);p[1]=count;p[2]=x>>8;p[3]=x;p[5]=10;return ESP_OK;}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d,const void *p,size_t n,int t)
{(void)d;(void)p;(void)n;(void)t;assert(0);return -2;}
esp_err_t i2c_master_receive(i2c_master_dev_handle_t d,void *p,size_t n,int t)
{(void)d;(void)p;(void)n;(void)t;assert(0);return -2;}
int main(void) {
 BoardTouchDiagnostics s=board_touch_diagnostics();assert(!s.init && !s.probe && !s.sequence && !s.transfer && !s.sample);
 for(init_fault=1;init_fault<=4;++init_fault) {
  assert(init_touch()!=ESP_OK);s=board_touch_diagnostics();
  assert(s.init==(init_fault==4?4:init_fault));assert(s.probe==(init_fault==4?2:0));assert(!s.sequence);
 }
 init_fault=0;assert(init_touch()==ESP_OK);s=board_touch_diagnostics();assert(s.init==4 && s.probe==1);
 lv_display=(lv_display_t*)1;BoardTouch touch;
 transfer_failure=true;assert(board_read_touch(&touch)==BOARD_IO_ERROR);s=board_touch_diagnostics();assert(s.sequence==1 && s.transfer==2 && s.sample==4);
 transfer_failure=false;count=2;assert(board_read_touch(&touch)==BOARD_IO_ERROR);s=board_touch_diagnostics();assert(s.sequence==2 && s.transfer==1 && s.sample==2);
 count=1;x=900;assert(board_read_touch(&touch)==BOARD_INVALID);s=board_touch_diagnostics();assert(s.sequence==3 && s.transfer==1 && s.sample==3);
 x=10;assert(board_read_touch(&touch)==BOARD_OK);s=board_touch_diagnostics();assert(s.sequence==4 && s.transfer==1 && s.sample==1);
 count=0;assert(board_read_touch(&touch)==BOARD_OK);s=board_touch_diagnostics();assert(s.sequence==5 && s.sample==1 && !touch.down);
 assert(s.init==4 && s.probe==1);assert(init_touch()==ESP_OK);s=board_touch_diagnostics();assert(!s.sequence && !s.transfer && !s.sample);
 puts("C6 native init/probe/transfer/decode/geometry/valid/release/reset diagnostics PASS");return 0;
}
