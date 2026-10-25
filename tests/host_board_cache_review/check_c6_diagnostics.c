/* Execute the production init/read path. Only physical SDK calls are faked. */
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
static bool boot_input_ready;
#define KATMED_ORIENTATION_HOST_TEST 1
#include "../../firmware/components/board/board.c"
static unsigned init_fault;
static bool transfer_failure;
static esp_err_t transfer_error=ESP_ERR_TIMEOUT;
static unsigned count, x;
static esp_err_t tx_error=ESP_OK, rx_error=ESP_OK;
static unsigned touch_tx, touch_rx, touch_combined;
static bool register_selected;
static esp_err_t touch_packet(void *out,size_t len) {
 assert(len==14);if(transfer_failure)return transfer_error;
 uint8_t *p=out;memset(p,0,len);p[1]=count;p[2]=x>>8;p[3]=x;p[5]=10;return ESP_OK;
}
static unsigned imu_fault, imu_writes, imu_removed;
static int64_t fake_time=100000;
static int16_t imu_x,imu_y=8192,imu_z;
int64_t esp_timer_get_time(void) {return fake_time;}
int i2c_new_master_bus(const i2c_master_bus_config_t *c,i2c_master_bus_handle_t *out)
{assert(c->sda_io_num==18 && c->scl_io_num==19);*out=(void*)1;return init_fault==1?-2:ESP_OK;}
int i2c_master_bus_add_device(i2c_master_bus_handle_t b,const i2c_device_config_t *c,i2c_master_dev_handle_t *out)
{assert(b && c->scl_speed_hz==400000);
 if(c->device_address==0x6b){*out=(void*)2;return imu_fault==4?-2:ESP_OK;}
 assert(c->device_address==0x63);*out=(void*)1;return init_fault==2?-2:ESP_OK;}
int esp_lcd_touch_new_i2c_axs5106(i2c_master_dev_handle_t dev,const esp_lcd_touch_config_t *c,esp_lcd_touch_handle_t *out)
{assert(dev && c->rst_gpio_num==20 && c->int_gpio_num==21);*out=(void*)1;return init_fault==3?-2:ESP_OK;}
int i2c_master_probe(i2c_master_bus_handle_t b,unsigned addr,int timeout)
{assert(b && addr==0x63 && timeout==100);return init_fault==4?-2:ESP_OK;}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,const void *reg,size_t n,void *out,size_t len,int timeout)
{assert(dev && n==1);
 if(dev==(void*)2){assert(timeout==20);uint8_t r=*(const uint8_t*)reg;
  if(imu_fault==1)return ESP_ERR_TIMEOUT;
  if(r==0){assert(len==1);*(uint8_t*)out=imu_fault==2?0:5;return ESP_OK;}
  if(r==0x2e){assert(len==1);*(uint8_t*)out=1;return ESP_OK;}
  assert(r==0x35 && len==6);uint8_t *p=out;int16_t values[]={imu_x,imu_y,imu_z};
  for(unsigned i=0;i<3;++i){p[2*i]=(uint8_t)values[i];p[2*i+1]=(uint8_t)((uint16_t)values[i]>>8);}return ESP_OK;}
 assert(*(const uint8_t*)reg==1 && len==14 && timeout==100);++touch_combined;return touch_packet(out,len);}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d,const void *p,size_t n,int t)
{if(d==(void*)1) {assert(n==1 && t==100 && *(const uint8_t*)p==1);
  ++touch_tx;register_selected=false;if(tx_error!=ESP_OK)return tx_error;
  register_selected=true;return ESP_OK;}
 assert(d==(void*)2 && n==2 && t==20);const uint8_t *v=p;
 assert((v[0]==8 && (v[1]==0 || v[1]==1)) || (v[0]==2 && v[1]==0x40) || (v[0]==3 && v[1]==0x18));
 ++imu_writes;return imu_fault==3?ESP_ERR_TIMEOUT:ESP_OK;}
esp_err_t i2c_master_receive(i2c_master_dev_handle_t d,void *p,size_t n,int t)
{assert(d==(void*)1 && n==14 && t==100 && register_selected);
 ++touch_rx;register_selected=false;if(rx_error!=ESP_OK)return rx_error;
 return touch_packet(p,n);}
int i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{assert(dev==(void*)2);++imu_removed;return ESP_OK;}
int main(void) {
 i2c_bus=(void*)1;
 for(imu_fault=1;imu_fault<=4;++imu_fault){init_orientation();assert(!imu_dev);Orientation pose;assert(!board_auto_orientation(&pose));}
 imu_fault=0;imu_writes=0;init_orientation();assert(imu_dev && imu_writes==4);
 Orientation pose=ORIENTATION_270;assert(!board_auto_orientation(&pose));
 for(unsigned i=1;i<=5;++i)poll_orientation(i*100);
 assert(board_auto_orientation(&pose) && pose==ORIENTATION_180);
 imu_x=-8192;imu_y=0;
 for(unsigned i=6;i<=10;++i)poll_orientation(i*100);
 assert(board_auto_orientation(&pose) && pose==ORIENTATION_90);
 imu_fault=1;poll_orientation(1100);assert(board_auto_orientation(&pose) && pose==ORIENTATION_90);
 imu_fault=0;imu_x=0;imu_z=8192;poll_orientation(1200);assert(board_auto_orientation(&pose) && pose==ORIENTATION_90);

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
 const esp_err_t errors[]={ESP_ERR_TIMEOUT,ESP_ERR_INVALID_RESPONSE,ESP_ERR_INVALID_ARG};
 for(unsigned i=0;i<3;++i) {
  transfer_failure=true;transfer_error=errors[i];
  assert(board_read_touch(&touch)==BOARD_IO_ERROR);s=board_touch_diagnostics();
  assert(s.native_result==i+2 && s.transfer==2 && s.sample==4);
 }
 transfer_failure=false;assert(board_read_touch(&touch)==BOARD_OK);
 s=board_touch_diagnostics();assert(s.native_result==1 && s.transfer==1);
 assert(init_touch()==ESP_OK);assert(board_touch_diagnostics().native_result==0);
 /* C6 must use the existing STOP helper, without hiding the first TX error.
  * Test production board_read_touch as well as exact helper error preservation. */
 const esp_err_t split_errors[]={ESP_ERR_TIMEOUT,ESP_ERR_INVALID_RESPONSE,ESP_ERR_INVALID_ARG};
 uint8_t bytes[14];
 for(unsigned i=0;i<3;++i) {
  tx_error=split_errors[i];rx_error=ESP_OK;touch_tx=touch_rx=touch_combined=0;
  assert(touch_read_register(1,bytes,sizeof(bytes),false)==split_errors[i]);
  assert(touch_tx==1 && touch_rx==0 && touch_combined==0);
  assert(board_read_touch(&touch)==BOARD_IO_ERROR);s=board_touch_diagnostics();
  assert(touch_tx==2 && touch_rx==0 && touch_combined==0);
  assert(s.native_result==i+2 && s.transfer==2 && s.sample==4);
 }
 tx_error=ESP_OK;
 for(unsigned i=0;i<3;++i) {
  rx_error=split_errors[i];touch_tx=touch_rx=touch_combined=0;
  assert(touch_read_register(1,bytes,sizeof(bytes),false)==split_errors[i]);
  assert(touch_tx==1 && touch_rx==1 && touch_combined==0);
  assert(board_read_touch(&touch)==BOARD_IO_ERROR);s=board_touch_diagnostics();
  assert(touch_tx==2 && touch_rx==2 && touch_combined==0);
  assert(s.native_result==i+2 && s.transfer==2 && s.sample==4);
 }
 rx_error=ESP_OK;count=1;x=10;touch_tx=touch_rx=touch_combined=0;
 assert(board_read_touch(&touch)==BOARD_OK && touch.down && touch.x==161 && touch.y==10);
 s=board_touch_diagnostics();assert(s.native_result==1 && s.transfer==1 && s.sample==1);
 assert(touch_tx==1 && touch_rx==1 && touch_combined==0);
 puts("C6 STOP contract TXfail-noRX3 RXerror-preserved3 success-bytes1 metricsPASS; QMI combined unchanged");
 puts("C6 native init/probe/transfer/decode/geometry/valid/release/reset diagnostics PASS");return 0;
}
