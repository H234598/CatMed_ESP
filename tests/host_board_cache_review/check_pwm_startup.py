"""Compile production PWM init/apply/cleanup with the SDK v6.1 fade prerequisite.

The board-review fakes normally omit ESP_PLATFORM. This focused check extracts
only its real PWM blocks, so missing native prerequisites cannot pass through
that host-only omission. Physical LEDC operations are faked; no device is used.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

def native_block(source):
    return source.split('#ifdef ESP_PLATFORM\n', 1)[1].split('#endif', 1)[0]

def main():
    board = (ROOT / 'firmware/components/board/board.c').read_text()
    declarations = board[board.index('static bool pwm_ready;'):].split('#endif', 1)[0]
    init = native_block(function(board, 'BoardResult board_init('))
    deinit = function(board, 'void board_deinit(')
    cleanup = deinit[deinit.index('    if (pwm_ready)'):].split('#endif', 1)[0]
    apply = function(board, 'static BoardResult apply_backlight(')
    setters = function(board, 'BoardResult board_set_backlight(') + '\n' + function(board, 'BoardResult board_set_brightness(')
    code = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define ESP_PLATFORM 1
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_LOGE(...) ((void)0)
#define LEDC_TAG "ledc"
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_TIMER_10_BIT 10
#define LEDC_TIMER_0 0
#define LEDC_CHANNEL_0 0
#define LEDC_AUTO_CLK 0
#define LCD_BACKLIGHT 23
typedef int esp_err_t;
typedef enum {BOARD_OK,BOARD_UNPROVEN_WIRING,BOARD_IO_ERROR,BOARD_INVALID} BoardResult;
typedef struct {int speed_mode,duty_resolution,timer_num,freq_hz,clk_cfg;} ledc_timer_config_t;
typedef struct {int gpio_num,speed_mode,channel,timer_sel,duty,hpoint;} ledc_channel_config_t;
static bool backlight_ready,backlight_enabled;
static uint8_t backlight_percent=100;
static void *s_ledc_fade_isr_handle;
static unsigned installs,uninstalls,writes,duty;
static bool install_fail,update_fail,timer_fail,channel_fail;
static esp_err_t ledc_timer_config(const ledc_timer_config_t *c) {
 if(c->duty_resolution!=10 || c->freq_hz!=5000)abort();
 return timer_fail?ESP_FAIL:ESP_OK;
}
static esp_err_t ledc_channel_config(const ledc_channel_config_t *c) {
 if(c->gpio_num!=LCD_BACKLIGHT || c->channel!=0 || c->duty!=0)abort();
 return channel_fail?ESP_FAIL:ESP_OK;
}
static esp_err_t ledc_fade_func_install(int flags) {
 if(flags!=0)abort();++installs;
 if(s_ledc_fade_isr_handle)return ESP_ERR_INVALID_STATE;
 if(install_fail)return ESP_ERR_NO_MEM;
 s_ledc_fade_isr_handle=(void*)1;return ESP_OK;
}
static void ledc_fade_func_uninstall(void) {
 if(!s_ledc_fade_isr_handle)abort();++uninstalls;s_ledc_fade_isr_handle=NULL;
}
/* SDK v6.1 components/esp_driver_ledc/src/ledc.c:1354..1357, exact guard.
 * Remaining allocation/hardware work is deliberately not emulated here. */
static esp_err_t fade_precondition(void) {
    if (s_ledc_fade_isr_handle == NULL) {
        ESP_LOGE(LEDC_TAG, "Fade service not installed, call ledc_fade_func_install");
        return ESP_FAIL;
    }
    return ESP_OK;
}
static esp_err_t ledc_set_duty_and_update(int mode,int channel,unsigned value,unsigned hpoint) {
 if(mode || channel || hpoint || value>1023)abort();
 /* Real set_duty_and_update:1654 propagates failure of this prerequisite. */
 if(fade_precondition()!=ESP_OK)return ESP_FAIL;
 if(update_fail)return ESP_FAIL;
 ++writes;duty=value;return ESP_OK;
}
static esp_err_t ledc_stop(int mode,int channel,int idle) {
 if(mode || channel || idle)abort();return ESP_OK;
}
static esp_err_t gpio_set_level(int pin,int level) {(void)pin;(void)level;return ESP_OK;}
'''
    code += declarations + '\n' + apply + '\n' + setters
    code += '\nstatic void deinit_pwm(void) {\n if(backlight_ready)(void)board_set_backlight(false);\n' + cleanup + '\n backlight_ready=false;\n}\n'
    code += '\nstatic BoardResult init_pwm(void) { esp_err_t err;backlight_ready=true;backlight_enabled=false;backlight_percent=100;\n' + init + '\n if(board_set_backlight(false)!=BOARD_OK)goto fail;return BOARD_OK;\nfail:deinit_pwm();return BOARD_IO_ERROR;\n}\n'
    code += r'''
#define CHECK(c,n) do {if(!(c))return n;} while(0)
int main(void) {
 CHECK(init_pwm()==BOARD_OK,17); /* Original47b fails here before panel init. */
 CHECK(installs==1 && s_ledc_fade_isr_handle && duty==0,18);
 CHECK(board_set_brightness(1)==BOARD_OK && board_set_backlight(true)==BOARD_OK && duty==10,19);
 CHECK(board_set_brightness(50)==BOARD_OK && duty==512,20);
 CHECK(board_set_brightness(100)==BOARD_OK && duty==1023,21);
 CHECK(board_set_brightness(0)==BOARD_INVALID && duty==1023,22);
 CHECK(board_set_backlight(false)==BOARD_OK && duty==0,23);
 deinit_pwm();CHECK(!s_ledc_fade_isr_handle && uninstalls==1 && !pwm_ready,24);
 CHECK(init_pwm()==BOARD_OK && installs==2,25);deinit_pwm();CHECK(uninstalls==2,26);
 unsigned saved=uninstalls,oldwrites=writes;
 install_fail=true;CHECK(init_pwm()==BOARD_IO_ERROR && !s_ledc_fade_isr_handle && uninstalls==saved && writes==oldwrites,27);install_fail=false;
 update_fail=true;CHECK(init_pwm()==BOARD_IO_ERROR && !s_ledc_fade_isr_handle && uninstalls==saved+1,28);update_fail=false;
 saved=installs;timer_fail=true;CHECK(init_pwm()==BOARD_IO_ERROR && installs==saved,29);timer_fail=false;
 channel_fail=true;CHECK(init_pwm()==BOARD_IO_ERROR && installs==saved,30);channel_fail=false;
 saved=uninstalls;s_ledc_fade_isr_handle=(void*)1;
 CHECK(init_pwm()==BOARD_IO_ERROR && s_ledc_fade_isr_handle && uninstalls==saved,31);
 return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='katmed-pwm-prerequisite-') as tmp:
        source = Path(tmp) / 'check.c'
        binary = Path(tmp) / 'check'
        source.write_text(code)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation', '-Wno-unused-function', str(source), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)])
        if result.returncode:
            raise SystemExit(result.returncode)
    print('production PWM startup prerequisite, duty1/50/100/off, init failure, owned cleanup/reinit PASS')

if __name__ == '__main__':
    main()
