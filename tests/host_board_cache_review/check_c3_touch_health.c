/* Product board, cache, decoder and buffer paths; only I2C/time are faked. */
#include <stdbool.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
typedef void *TaskHandle_t;
typedef unsigned TickType_t;
#define pdPASS 1
static TickType_t xTaskGetTickCount(void);
static int xTaskCreate(void (*fn)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
static void vTaskDelayUntil(TickType_t *, TickType_t);
static void vTaskDelete(TaskHandle_t);
#define KATMED_C3_TOUCH_HOST_TEST 1
#include "../../firmware/components/board/board.c"
static int64_t now_us;
static bool down, fail;
static uint8_t selected;
int64_t esp_timer_get_time(void) { return now_us; }
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d, const void *r,
    size_t n, void *out, size_t len, int timeout)
{ (void)d;(void)timeout; assert(n==1 && *(const uint8_t *)r==2 && len==1);
  if(fail)return ESP_ERR_TIMEOUT;
  *(uint8_t *)out=down ? 1 : 0; return ESP_OK; }
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d,const void *r,size_t n,int timeout)
{ (void)d;(void)timeout; assert(n==1); selected=*(const uint8_t *)r; return ESP_OK; }
esp_err_t i2c_master_receive(i2c_master_dev_handle_t d,void *out,size_t len,int timeout)
{ (void)d;(void)timeout; assert(selected==3 && len==4); memset(out,0,len); return ESP_OK; }
static jmp_buf task_yield;
static bool create_fails, run_early;
static TickType_t xTaskGetTickCount(void) { return 0; }
static void vTaskDelayUntil(TickType_t *tick, TickType_t delay)
{ (void)tick; (void)delay; longjmp(task_yield, 1); }
static void vTaskDelete(TaskHandle_t task) { (void)task; }
void vTaskDelay(void) { touch_sampler_task = NULL; }
static int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *handle)
{
    (void)name; (void)stack; (void)priority;
    if (create_fails) return 0;
    *handle=(void *)1;
    if (run_early && setjmp(task_yield)==0) fn(arg);
    return pdPASS;
}
static bool lock_allowed;
bool lvgl_port_lock(unsigned timeout) { (void)timeout; return lock_allowed; }
void lvgl_port_unlock(void) {}
int esp_lcd_panel_swap_xy(void) { return 0; }
int esp_lcd_panel_mirror(void) { return 0; }
int gpio_set_level(void) { return 0; }
int gpio_reset_pin(void) { return 0; }
int lvgl_port_remove_disp(void) { return 0; }
int lvgl_port_deinit(void) { return 0; }
int i2c_master_bus_rm_device(void) { return 0; }
int i2c_del_master_bus(void) { return 0; }
int esp_lcd_panel_del(void) { return 0; }
int esp_lcd_panel_io_del(void) { return 0; }
int spi_bus_free(void) { return 0; }
static BoardTouchBuffer buffer;
static void sample(bool pressed, bool error)
{
    BoardTouch t; down=pressed; fail=error;
    uint64_t stamp=c3_interrupt_generation;
    BoardResult r=board_read_touch(&t);
    c3_sample_finish(&buffer,true,c3_health.period,stamp,t,r);
}
int main(void)
{
    lv_init(); lv_display=lv_display_create(240,240); assert(lv_display);
    i2c_dev=(void *)1; lv_indev_t *indev=lv_indev_create(); assert(indev);
    c3_board_ready=true;
    create_fails=true; assert(board_touch_sampler_start()==BOARD_IO_ERROR);
    assert(!board_c3_touch_health().coverage);
    create_fails=false; run_early=true; down=false;
    assert(board_touch_sampler_start()==BOARD_OK);
    assert(c3_health.coverage && c3_health.period==1 && c3_health.contact_state==0);
    board_touch_sampler_stop(); assert(!c3_health.coverage);
    run_early=false; assert(board_touch_sampler_start()==BOARD_OK);
    assert(c3_health.coverage && c3_health.period==2 && c3_health.contact_state==0);
    board_touch_sampler_stop(); c3_period_start();
    sample(false,false); sample(false,false); assert(c3_health.clean_release==0 && c3_health.contact_state==1);
    sample(true,false); sample(false,false); assert(c3_health.clean_release==1 && c3_health.contact_state==1);
    sample(true,false); sample(true,true); sample(false,false);
    assert(c3_health.invalid==1 && c3_health.interrupted==1 && c3_health.clean_release==1);
    sample(true,false);
    BoardTouch held; down=false; fail=false; uint64_t stamp=c3_interrupt_generation;
    assert(board_read_touch(&held)==BOARD_OK);
    now_us=150000; lv_indev_data_t data={0}; lv_touch_read(indev,&data); assert(c3_health.stale==0);
    now_us=151000; lv_touch_read(indev,&data); assert(c3_health.stale==1 && c3_health.contact_state==0);
    c3_sample_finish(&buffer,true,c3_health.period,stamp,held,BOARD_OK);
    assert(c3_health.contact_state==0 && c3_health.clean_release==1);
    sample(false,false); now_us+=151000; lv_touch_read(indev,&data); assert(c3_health.stale==2);
    buffer=(BoardTouchBuffer){0}; sample(false,false); sample(true,false);
    buffer.count=BOARD_TOUCH_BUFFER_CAPACITY; stamp=c3_interrupt_generation;
    down=false; assert(board_read_touch(&held)==BOARD_OK);
    c3_sample_finish(&buffer,true,c3_health.period,stamp,held,BOARD_OK);
    assert(c3_health.overflow==1 && buffer.overflow && c3_health.clean_release==1);
    sample(false,false); assert(!buffer.overflow && buffer.count==2 && c3_health.clean_release==1);
    uint32_t old=c3_health.period, generation=touch_generation;
    lock_allowed=true;
    assert(board_set_orientation(ORIENTATION_90)==BOARD_OK);
    assert(c3_health.period==old+1 && touch_generation!=generation);
    lock_allowed=false;
    stamp=c3_interrupt_generation;
    c3_sample_finish(&buffer,generation==touch_generation,old,stamp,held,BOARD_OK);
    assert(c3_health.discard==1 && c3_health.contact_state==0);
    c3_health.contact_state=2; lv_touch=indev; port_ready=true;
    board_deinit();
    assert(!c3_health.coverage && c3_health.contact_state==0 && !c3_board_ready);
    assert(lv_touch==indev); /* lock failure is the real early-return boundary */
    port_ready=false; lv_touch=NULL; c3_board_ready=true; c3_period_start();
    c3_health.clean_release=UINT32_MAX; c3_increment(&c3_health.clean_release);
    assert(c3_health.clean_release==UINT32_MAX);
    c3_health.period=UINT32_MAX-1; c3_period_start(); assert(!c3_health.coverage && c3_health.period==UINT32_MAX);
    c3_period_start(); assert(!c3_health.coverage && c3_health.period==UINT32_MAX);
    lv_indev_delete(indev); lv_display_delete(lv_display); lv_deinit();
    puts("C3 board cache, buffer and bounded touch observation passed");
}
