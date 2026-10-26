#include "katmed_board.h"
#include "board_geometry.h"
#include "board_protocol.h"
#include "board_indev_gate.h"
#include "board_touch_buffer.h"
#include "board_display_budget.h"
#include "board_boot_gate.h"
#include "board_orientation.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#ifdef ESP_PLATFORM
#include "driver/ledc.h"
#endif
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_IDF_TARGET_ESP32C3
#include "c3_pins.h"
#include "esp_lcd_gc9a01.h"
#define LCD_WIDTH KATMED_C3_LCD_WIDTH
#define LCD_HEIGHT KATMED_C3_LCD_HEIGHT
#define LCD_SCK KATMED_C3_LCD_SCK
#define LCD_MOSI KATMED_C3_LCD_MOSI
#define LCD_CS KATMED_C3_LCD_CS
#define LCD_DC KATMED_C3_LCD_DC
#define LCD_RESET KATMED_C3_LCD_RESET
#define LCD_BACKLIGHT KATMED_C3_LCD_BACKLIGHT
#define TOUCH_SDA KATMED_C3_TOUCH_SDA
#define TOUCH_SCL KATMED_C3_TOUCH_SCL
#define TOUCH_RESET KATMED_C3_TOUCH_RESET
#define TOUCH_INTERRUPT KATMED_C3_TOUCH_INTERRUPT
#define TOUCH_ADDRESS KATMED_C3_TOUCH_I2C_ADDRESS_7BIT
#define BOOT_GPIO KATMED_C3_BOOT_GPIO
#define PANEL_MIRROR_X_AT_ZERO KATMED_C3_PANEL_MIRROR_X_AT_ZERO
#else
#include "c6_pins.h"
#include "esp_lcd_jd9853.h"
#include "esp_lcd_touch_axs5106.h"
#define LCD_WIDTH 172
#define LCD_HEIGHT 320
#define LCD_SCK KATMED_C6_LCD_SCK
#define LCD_MOSI KATMED_C6_LCD_MOSI
#define LCD_CS KATMED_C6_LCD_CS
#define LCD_DC KATMED_C6_LCD_DC
#define LCD_RESET KATMED_C6_LCD_RESET
#define LCD_BACKLIGHT KATMED_C6_LCD_BACKLIGHT
#define TOUCH_SDA KATMED_C6_TOUCH_SDA
#define TOUCH_SCL KATMED_C6_TOUCH_SCL
#define TOUCH_RESET KATMED_C6_TOUCH_RESET
#define TOUCH_INTERRUPT KATMED_C6_TOUCH_INTERRUPT
#define TOUCH_ADDRESS ESP_LCD_TOUCH_IO_I2C_AXS5106_ADDRESS
#define BOOT_GPIO KATMED_C6_BOOT_GPIO
#define PANEL_MIRROR_X_AT_ZERO 0
#endif

#if CONFIG_IDF_TARGET_ESP32C3
#define DRAW_LINES KATMED_C3_FAST_DRAW_LINES
#else
#define DRAW_LINES 16U
#endif
#define I2C_TIMEOUT_MS 100
static const BoardGeometry geometry = {
    .width = LCD_WIDTH, .height = LCD_HEIGHT,
#if CONFIG_IDF_TARGET_ESP32C6
    .mirror_x_at_zero = true,
#endif
};
static esp_lcd_panel_io_handle_t panel_io;
static esp_lcd_panel_handle_t panel;
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t i2c_dev;
#if CONFIG_IDF_TARGET_ESP32C6
static esp_lcd_touch_handle_t c6_touch;
static i2c_master_dev_handle_t imu_dev;
static BoardOrientationPolicy orientation_policy;
static uint64_t imu_poll_ms;
#if defined(ESP_PLATFORM) || defined(KATMED_ORIENTATION_HOST_TEST)
static void poll_orientation(uint64_t now);
#endif
#endif
static lv_display_t *lv_display;
static lv_indev_t *lv_touch;
static bool spi_ready;
static bool port_ready;
static bool backlight_ready;
static uint8_t backlight_percent=100;
static bool backlight_enabled;
#ifdef ESP_PLATFORM
static bool pwm_ready;
static bool pwm_fade_owned;
#endif
static Orientation current_orientation = ORIENTATION_0;
static portMUX_TYPE touch_lock = portMUX_INITIALIZER_UNLOCKED;
static BoardTouch latest_touch;
static bool latest_valid;
static int16_t latest_base_x, latest_base_y;
static BoardIndevGate indev_gate = {.quarantined = true};
#ifdef ESP_PLATFORM
static TaskHandle_t touch_sampler_task;
static volatile bool touch_sampler_active;
static BoardTouchBuffer touch_samples;
static uint32_t touch_generation;
static BoardBootGate boot_gate;
static bool boot_input_ready;

BoardResult board_boot_button_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        /* Both documented boards have an external 10K pull-up. */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    if (gpio_config(&config) != ESP_OK) return BOARD_IO_ERROR;
    boot_gate = (BoardBootGate){0};
    boot_input_ready = true;
    return BOARD_OK;
}

bool board_boot_button_released(uint64_t monotonic_ms)
{
    if (!boot_input_ready) return false;
    return board_boot_gate_poll(&boot_gate, gpio_get_level(BOOT_GPIO) == 0,
                                monotonic_ms);
}

bool board_boot_button_high(void)
{
    return boot_input_ready && gpio_get_level(BOOT_GPIO) == 1;
}

static void touch_sampler(void *argument)
{
    (void)argument;
    TickType_t last = xTaskGetTickCount();
    while (touch_sampler_active) {
        BoardTouch sample;
        portENTER_CRITICAL(&touch_lock);
        const uint32_t generation=touch_generation;
        portEXIT_CRITICAL(&touch_lock);
        BoardResult result = board_read_touch(&sample);
        portENTER_CRITICAL(&touch_lock);
        if (generation==touch_generation)
            board_touch_buffer_push(&touch_samples, sample, result);
        portEXIT_CRITICAL(&touch_lock);
#if CONFIG_IDF_TARGET_ESP32C6
        poll_orientation((uint64_t)(esp_timer_get_time()/1000));
#endif
        vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
    }
    touch_sampler_task = NULL;
    vTaskDelete(NULL);
}

BoardResult board_touch_sampler_start(void)
{
    if (!i2c_dev || !lv_display || touch_sampler_active) return BOARD_INVALID;
    touch_samples = (BoardTouchBuffer){0};
    touch_sampler_active = true;
    if (xTaskCreate(touch_sampler, "katmed_touch", 3072, NULL, 5,
                    &touch_sampler_task) != pdPASS) {
        touch_sampler_active = false;
        touch_sampler_task = NULL;
        return BOARD_IO_ERROR;
    }
    return BOARD_OK;
}

void board_touch_sampler_stop(void)
{
    touch_sampler_active = false;
    /* Let an in-flight I2C read and its critical section finish first. */
    for (unsigned i = 0; touch_sampler_task && i < pdMS_TO_TICKS(500); ++i)
        vTaskDelay(1);
    if (touch_sampler_task) {
        vTaskDelete(touch_sampler_task);
        touch_sampler_task = NULL;
    }
    touch_samples = (BoardTouchBuffer){0};
}

bool board_touch_sampler_running(void) { return touch_sampler_active; }

bool board_touch_sampler_take(BoardTouch *sample, BoardResult *result)
{
    bool available;
    portENTER_CRITICAL(&touch_lock);
    available = board_touch_buffer_take(&touch_samples, sample, result);
    portEXIT_CRITICAL(&touch_lock);
    return available;
}
#endif

static BoardTouchFilter touch_filter;
void board_set_touch_filter(BoardTouchFilter filter) { touch_filter = filter; }

static void lv_touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    BoardTouch sample;
    bool valid;
    bool reset;
    bool pressed;
    int16_t base_x, base_y;
    portENTER_CRITICAL(&touch_lock);
    sample = latest_touch;
    valid = latest_valid;
    base_x = latest_base_x;
    base_y = latest_base_y;
    bool stale = !valid || (uint64_t)(esp_timer_get_time() / 1000) - sample.monotonic_ms > 150;
    pressed = board_indev_gate_poll(&indev_gate, valid, sample.down, stale, &reset);
    portEXIT_CRITICAL(&touch_lock);
    if (touch_filter && !touch_filter(sample.down, sample.x, sample.y,
                                       valid && !stale, sample.monotonic_ms))
        reset = true;
    if (reset) {
        /* LV_EVENT_INDEV_RESET aborts a button press without generating CLICKED. */
        lv_indev_reset(indev, NULL);
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    /* LVGL rotates this base point exactly once in indev_pointer_proc(). */
    data->point.x = base_x;
    data->point.y = base_y;
}

static esp_err_t touch_read_register(uint8_t reg, uint8_t *out, size_t len, bool repeated_start)
{
    if (repeated_start) return i2c_master_transmit_receive(i2c_dev, &reg, 1, out, len, I2C_TIMEOUT_MS);
    esp_err_t err = i2c_master_transmit(i2c_dev, &reg, 1, I2C_TIMEOUT_MS);
    return err == ESP_OK ? i2c_master_receive(i2c_dev, out, len, I2C_TIMEOUT_MS) : err;
}

static BoardTouchDiagnostics touch_diagnostics;
BoardTouchDiagnostics board_touch_diagnostics(void)
{
    portENTER_CRITICAL(&touch_lock);
    BoardTouchDiagnostics sample = touch_diagnostics;
    portEXIT_CRITICAL(&touch_lock);
    return sample;
}

/* QST QMI8658A datasheet 13-52-25 Rev A: WHO_AM_I=05, address 6b.
 * CTRL1 ADDR_AI/little endian, CTRL2 ±4g/31.25Hz, CTRL7 accel only.
 * Do not copy the vendor demo's self-test bit or enable its unused gyro. */
#if CONFIG_IDF_TARGET_ESP32C6
static bool imu_write(uint8_t reg, uint8_t value)
{
    const uint8_t command[2]={reg,value};
    return i2c_master_transmit(imu_dev,command,sizeof(command),20)==ESP_OK;
}
static void init_orientation(void)
{
    orientation_policy=(BoardOrientationPolicy){0};
    imu_poll_ms=0;
    const i2c_device_config_t config={.dev_addr_length=I2C_ADDR_BIT_LEN_7,
        .device_address=0x6b,.scl_speed_hz=400000};
    if (i2c_master_bus_add_device(i2c_bus,&config,&imu_dev)!=ESP_OK) {
        imu_dev=NULL; return;
    }
    const uint8_t reg=0; uint8_t id=0;
    if (i2c_master_transmit_receive(imu_dev,&reg,1,&id,1,20)!=ESP_OK || id!=0x05) {
        (void)i2c_master_bus_rm_device(imu_dev); imu_dev=NULL; return;
    }
    if (!imu_write(0x08,0x00) || !imu_write(0x02,0x40) ||
        !imu_write(0x03,0x18) || !imu_write(0x08,0x01)) {
        (void)imu_write(0x08,0x00);
        (void)i2c_master_bus_rm_device(imu_dev); imu_dev=NULL;
    }
    /* Optional sensor errors cannot make the board or manual rotation fail. */
}
#if defined(ESP_PLATFORM) || defined(KATMED_ORIENTATION_HOST_TEST)
static void poll_orientation(uint64_t now)
{
    if (!imu_dev || now-imu_poll_ms<100) return;
    imu_poll_ms=now;
    uint8_t status=0, bytes[6]={0};
    const uint8_t status_reg=0x2e, accel_reg=0x35;
    bool ok=i2c_master_transmit_receive(imu_dev,&status_reg,1,&status,1,20)==ESP_OK;
    ok=ok && (status&1u)!=0;
    if (ok) ok=i2c_master_transmit_receive(imu_dev,&accel_reg,1,bytes,6,20)==ESP_OK;
    const int16_t x=(int16_t)((uint16_t)bytes[0]|(uint16_t)bytes[1]<<8);
    const int16_t y=(int16_t)((uint16_t)bytes[2]|(uint16_t)bytes[3]<<8);
    const int16_t z=(int16_t)((uint16_t)bytes[4]|(uint16_t)bytes[5]<<8);
    /* Only the small cached policy is shared; no I2C inside this section. */
    portENTER_CRITICAL(&touch_lock);
    (void)board_orientation_update(&orientation_policy,ok,x,y,z,now);
    portEXIT_CRITICAL(&touch_lock);
}
#endif
#endif
bool board_auto_orientation(Orientation *orientation)
{
#if CONFIG_IDF_TARGET_ESP32C6
    if (!orientation) return false;
    portENTER_CRITICAL(&touch_lock);
    const bool valid=orientation_policy.valid;
    if (valid) *orientation=orientation_policy.accepted;
    portEXIT_CRITICAL(&touch_lock);
    return valid;
#else
    (void)orientation; return false;
#endif
}

static esp_err_t init_touch(void)
{
#if CONFIG_IDF_TARGET_ESP32C6
    portENTER_CRITICAL(&touch_lock);
    touch_diagnostics = (BoardTouchDiagnostics){0};
    portEXIT_CRITICAL(&touch_lock);
#endif
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = TOUCH_SDA, .scl_io_num = TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &i2c_bus);
#if CONFIG_IDF_TARGET_ESP32C6
    portENTER_CRITICAL(&touch_lock);
    touch_diagnostics.init = err == ESP_OK ? 0u : 1u;
    portEXIT_CRITICAL(&touch_lock);
#endif
    if (err != ESP_OK) return err;
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TOUCH_ADDRESS,
#if CONFIG_IDF_TARGET_ESP32C3
        .scl_speed_hz = 100000,
#else
        .scl_speed_hz = 400000,
#endif
    };
    err = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &i2c_dev);
#if CONFIG_IDF_TARGET_ESP32C6
    portENTER_CRITICAL(&touch_lock);
    touch_diagnostics.init = err == ESP_OK ? 0u : 2u;
    portEXIT_CRITICAL(&touch_lock);
#endif
    if (err != ESP_OK) return err;
#if CONFIG_IDF_TARGET_ESP32C3
    gpio_config_t gpio_cfg = {.pin_bit_mask = (1ULL << TOUCH_INTERRUPT) | (1ULL << TOUCH_RESET),
                              .mode = GPIO_MODE_OUTPUT};
    err = gpio_config(&gpio_cfg);
    if (err != ESP_OK) return err;
    err = gpio_set_level(TOUCH_INTERRUPT, 1);
    if (err != ESP_OK) return err;
    esp_rom_delay_us(1000);
    err = gpio_set_level(TOUCH_INTERRUPT, 0);
    if (err != ESP_OK) return err;
    esp_rom_delay_us(1000);
    err = gpio_set_level(TOUCH_RESET, 0);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));
    err = gpio_set_level(TOUCH_RESET, 1);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(300));
    gpio_cfg.pin_bit_mask = 1ULL << TOUCH_INTERRUPT;
    gpio_cfg.mode = GPIO_MODE_INPUT;
    err = gpio_config(&gpio_cfg);
    if (err != ESP_OK) return err;
    const uint8_t init_cmd[] = {0xfe, 0xff};
    err = i2c_master_transmit(i2c_dev, init_cmd, sizeof(init_cmd), I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;
#else
    esp_lcd_touch_config_t config = {
        .x_max = LCD_WIDTH, .y_max = LCD_HEIGHT,
        .rst_gpio_num = TOUCH_RESET, .int_gpio_num = TOUCH_INTERRUPT,
    };
    err = esp_lcd_touch_new_i2c_axs5106(i2c_dev, &config, &c6_touch);
    portENTER_CRITICAL(&touch_lock);
    touch_diagnostics.init = err == ESP_OK ? 4u : 3u;
    portEXIT_CRITICAL(&touch_lock);
    if (err != ESP_OK) { c6_touch = NULL; return err; }
#endif
    err = i2c_master_probe(i2c_bus, TOUCH_ADDRESS, I2C_TIMEOUT_MS);
#if CONFIG_IDF_TARGET_ESP32C6
    portENTER_CRITICAL(&touch_lock);
    touch_diagnostics.probe = err == ESP_OK ? 1u : 2u;
    portEXIT_CRITICAL(&touch_lock);
#endif
    return err;
}

static esp_err_t init_panel(void)
{
    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_SCK, .mosi_io_num = LCD_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
#if CONFIG_IDF_TARGET_ESP32C3
        .max_transfer_sz = KATMED_C3_SPI_TRANSFER_MAX_BYTES(LCD_WIDTH),
#else
        .max_transfer_sz = LCD_HEIGHT * DRAW_LINES * sizeof(uint16_t) + 8,
#endif
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    spi_ready = true;
#if CONFIG_IDF_TARGET_ESP32C3
    esp_lcd_panel_io_spi_config_t io_cfg = GC9A01_PANEL_IO_SPI_CONFIG(LCD_CS, LCD_DC, NULL, NULL);
#else
    esp_lcd_panel_io_spi_config_t io_cfg = JD9853_PANEL_IO_SPI_CONFIG(LCD_CS, LCD_DC, NULL, NULL);
#endif
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &panel_io);
    if (err != ESP_OK) return err;
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RESET,
#if CONFIG_IDF_TARGET_ESP32C3
        /* The matched touch factory firmware uses rgb_order=false, which
         * LovyanGFX maps to MAD_BGR for this GC9A01 panel. */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
#if CONFIG_IDF_TARGET_ESP32C3
    err = esp_lcd_new_panel_gc9a01(panel_io, &panel_cfg, &panel);
#else
    err = esp_lcd_new_panel_jd9853(panel_io, &panel_cfg, &panel);
#endif
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_reset(panel);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_invert_color(panel, true);
    if (err != ESP_OK) return err;
#if CONFIG_IDF_TARGET_ESP32C6
    err = esp_lcd_panel_set_gap(panel, 34, 0);
    if (err != ESP_OK) return err;
#endif
    return esp_lcd_panel_disp_on_off(panel, true);
}

static BoardResult apply_backlight(void)
{
    if (!backlight_ready) return BOARD_INVALID;
#ifdef ESP_PLATFORM
    if (pwm_ready) {
        const uint32_t duty=backlight_enabled ? (1023u*backlight_percent+50u)/100u : 0;
        return ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,duty,0)==ESP_OK
            ? BOARD_OK : BOARD_IO_ERROR;
    }
#endif
    return gpio_set_level(LCD_BACKLIGHT,backlight_enabled?1:0)==ESP_OK ? BOARD_OK : BOARD_IO_ERROR;
}
BoardResult board_set_backlight(bool enabled)
{
    const bool previous=backlight_enabled;backlight_enabled=enabled;
    BoardResult result=apply_backlight();if(result!=BOARD_OK)backlight_enabled=previous;
    return result;
}
BoardResult board_set_brightness(uint8_t percent)
{
    if (percent<1 || percent>100) return BOARD_INVALID;
    const uint8_t previous=backlight_percent;backlight_percent=percent;
    BoardResult result=apply_backlight();if(result!=BOARD_OK)backlight_percent=previous;
    return result;
}

static esp_err_t apply_panel_orientation(Orientation orientation)
{
    esp_err_t err;
    BoardPanelTransform transform;
    if (!board_geometry_panel_transform(orientation, PANEL_MIRROR_X_AT_ZERO, &transform))
        return ESP_ERR_INVALID_ARG;
#if CONFIG_IDF_TARGET_ESP32C6
    err = esp_lcd_panel_set_gap(panel,
        (orientation == ORIENTATION_90 || orientation == ORIENTATION_270) ? 0 : 34,
        (orientation == ORIENTATION_90 || orientation == ORIENTATION_270) ? 34 : 0);
    if (err != ESP_OK) return err;
#endif
    err = esp_lcd_panel_swap_xy(panel, transform.swap_xy);
    if (err != ESP_OK) return err;
    return esp_lcd_panel_mirror(panel, transform.mirror_x, transform.mirror_y);
}

BoardResult board_set_orientation(Orientation orientation)
{
    uint16_t width, height;
    if (!board_geometry_size(geometry, orientation, &width, &height)) return BOARD_INVALID;
    if (!lv_display) { current_orientation = orientation; return BOARD_OK; }
    if (!lvgl_port_lock(1000)) return BOARD_IO_ERROR;
    if (apply_panel_orientation(orientation) != ESP_OK) {
        (void)apply_panel_orientation(current_orientation);
        lvgl_port_unlock();
        return BOARD_IO_ERROR;
    }
    lv_display_set_rotation(lv_display, (lv_display_rotation_t)orientation);
    /* The pinned LVGL port repeats these panel calls without propagating errors. */
    if (apply_panel_orientation(orientation) != ESP_OK) {
        lv_display_set_rotation(lv_display, (lv_display_rotation_t)current_orientation);
        (void)apply_panel_orientation(current_orientation);
        lvgl_port_unlock();
        return BOARD_IO_ERROR;
    }
    portENTER_CRITICAL(&touch_lock);
    current_orientation = orientation;
    latest_valid = false;
#ifdef ESP_PLATFORM
    ++touch_generation;
    touch_samples=(BoardTouchBuffer){0};
#endif
    board_indev_gate_invalidate(&indev_gate);
    portEXIT_CRITICAL(&touch_lock);
    lvgl_port_unlock();
    return BOARD_OK;
}

BoardResult board_read_touch(BoardTouch *touch)
{
    if (!touch) return BOARD_INVALID;
    memset(touch, 0, sizeof(*touch));
    touch->monotonic_ms = (uint64_t)(esp_timer_get_time() / 1000);
    if (!i2c_dev || !lv_display) return BOARD_INVALID;
    uint16_t x = 0, y = 0;
    esp_err_t err;
#if CONFIG_IDF_TARGET_ESP32C3
    uint8_t count = 0;
    err = touch_read_register(0x02, &count, 1, true);
    if (err == ESP_OK && count == 1) {
        uint8_t point[4];
        err = touch_read_register(0x03, point, sizeof(point), false);
        if (err == ESP_OK && !board_c3_touch_decode(count, point, &touch->down, &x, &y))
            err = ESP_ERR_INVALID_RESPONSE;
    } else if (err == ESP_OK && !board_c3_touch_decode(count, NULL, &touch->down, &x, &y)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
#else
    uint8_t packet[14];
    /* Match the C6 reference's STOP-separated register read. The existing
     * helper returns the TX error immediately and only then permits RX. */
    err = touch_read_register(0x01, packet, sizeof(packet), false);
    const bool transfer_ok = err == ESP_OK;
    const uint32_t native_result = err == ESP_OK ? 1u :
        err == ESP_ERR_TIMEOUT ? 2u : err == ESP_ERR_INVALID_RESPONSE ? 3u : 4u;
    if (err == ESP_OK && !board_c6_touch_decode(packet, &touch->down, &x, &y))
        err = ESP_ERR_INVALID_RESPONSE;
#endif
    const bool read_ok = err == ESP_OK;
    /* The sampler and the main-thread rotation share only the pure transform
     * and cached result; all slow I2C has already completed outside this lock. */
    portENTER_CRITICAL(&touch_lock);
    bool decoded_down = touch->down;
    bool valid = board_geometry_sample(geometry, current_orientation, read_ok,
                                       decoded_down, x, y, &touch->down, &touch->x, &touch->y);
    BoardResult result = valid ? BOARD_OK : (read_ok ? BOARD_INVALID : BOARD_IO_ERROR);
    int16_t base_x = 0, base_y = 0;
    if (result == BOARD_OK && touch->down &&
        !board_geometry_touch(geometry, ORIENTATION_0, x, y, &base_x, &base_y)) {
        result = BOARD_INVALID;
        touch->down = false;
    }
#if CONFIG_IDF_TARGET_ESP32C6
    if(++touch_diagnostics.sequence == 0) ++touch_diagnostics.sequence;
    touch_diagnostics.transfer = transfer_ok ? 1u : 2u;
    touch_diagnostics.native_result = native_result;
    touch_diagnostics.sample = !transfer_ok ? 4u : !read_ok ? 2u :
                               result == BOARD_OK ? 1u : 3u;
#endif
    latest_touch = *touch;
    latest_valid = result == BOARD_OK;
    /* LVGL's slider recalculates on RELEASED before the UI callback. A valid
     * finger-up has no controller coordinates, so keep the last press point. */
    if (touch->down || result != BOARD_OK) {
        latest_base_x = base_x;
        latest_base_y = base_y;
    }
    if (result != BOARD_OK) board_indev_gate_invalidate(&indev_gate);
    portEXIT_CRITICAL(&touch_lock);
    return result;
}

void board_deinit(void)
{
    boot_input_ready = false;
#ifdef ESP_PLATFORM
    board_touch_sampler_stop();
#endif
    if (backlight_ready) (void)board_set_backlight(false);
    if (lv_touch && port_ready) {
        /* The LVGL task may still run; the port mutex is recursive. */
        if (!lvgl_port_lock(0)) return;
        lv_indev_delete(lv_touch);
        lvgl_port_unlock();
        lv_touch = NULL;
    }
    if (lv_display) { (void)lvgl_port_remove_disp(lv_display); lv_display = NULL; }
    if (port_ready) { (void)lvgl_port_deinit(); port_ready = false; }
#if CONFIG_IDF_TARGET_ESP32C6
    if (c6_touch) { (void)esp_lcd_touch_del(c6_touch); c6_touch = NULL; }
    if (imu_dev) { (void)imu_write(0x08,0x00); (void)i2c_master_bus_rm_device(imu_dev); imu_dev=NULL; }
    orientation_policy=(BoardOrientationPolicy){0};
#endif
    if (i2c_dev) { (void)i2c_master_bus_rm_device(i2c_dev); i2c_dev = NULL; }
    if (i2c_bus) { (void)i2c_del_master_bus(i2c_bus); i2c_bus = NULL; }
    if (panel) { (void)esp_lcd_panel_del(panel); panel = NULL; }
    if (panel_io) { (void)esp_lcd_panel_io_del(panel_io); panel_io = NULL; }
    if (spi_ready) { (void)spi_bus_free(SPI2_HOST); spi_ready = false; }
#ifdef ESP_PLATFORM
    if (pwm_ready) {(void)ledc_stop(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,0);pwm_ready=false;}
    if (pwm_fade_owned) {ledc_fade_func_uninstall();pwm_fade_owned=false;}
#endif
    if (backlight_ready) { (void)gpio_reset_pin(LCD_BACKLIGHT); backlight_ready = false; }
    portENTER_CRITICAL(&touch_lock);
    latest_valid = false;
    board_indev_gate_invalidate(&indev_gate);
    portEXIT_CRITICAL(&touch_lock);
}

BoardResult board_init(lv_display_t **display)
{
    if (!display || lv_display || port_ready) return BOARD_INVALID;
    *display = NULL;
    gpio_config_t bl_cfg = {.pin_bit_mask = 1ULL << LCD_BACKLIGHT,
                            .mode = GPIO_MODE_OUTPUT};
    esp_err_t err = gpio_config(&bl_cfg);
    if (err != ESP_OK) return BOARD_IO_ERROR;
    backlight_ready = true;
    backlight_percent=100;backlight_enabled=false;
#ifdef ESP_PLATFORM
    const ledc_timer_config_t pwm_timer={.speed_mode=LEDC_LOW_SPEED_MODE,
        .duty_resolution=LEDC_TIMER_10_BIT,.timer_num=LEDC_TIMER_0,.freq_hz=5000,
        .clk_cfg=LEDC_AUTO_CLK};
    err=ledc_timer_config(&pwm_timer);if(err!=ESP_OK)goto fail;
    const ledc_channel_config_t pwm_channel={.gpio_num=LCD_BACKLIGHT,
        .speed_mode=LEDC_LOW_SPEED_MODE,.channel=LEDC_CHANNEL_0,
        .timer_sel=LEDC_TIMER_0,.duty=0,.hpoint=0};
    err=ledc_channel_config(&pwm_channel);if(err!=ESP_OK)goto fail;
    pwm_ready=true;
    /* The thread-safe duty update API requires the native fade service. */
    err=ledc_fade_func_install(0);if(err!=ESP_OK)goto fail;
    pwm_fade_owned=true;
#endif
    if (board_set_backlight(false) != BOARD_OK) goto fail;
    err = init_panel();
    if (err != ESP_OK) goto fail;
    err = init_touch();
    if (err != ESP_OK) goto fail;
#if CONFIG_IDF_TARGET_ESP32C6
    init_orientation();
#endif
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) goto fail;
    port_ready = true;
    uint16_t draw_lines = DRAW_LINES;
    bool double_buffer = true;
#if CONFIG_IDF_TARGET_ESP32C3
    const size_t largest_dma_block = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    const BoardC3DisplayPlan plan = board_c3_display_plan(LCD_WIDTH, largest_dma_block);
    draw_lines = plan.lines;
    double_buffer = plan.double_buffer;
    ESP_LOGI("katmed_board", "c3 draw dma_largest=%zu lines=%u buffers=%u transfer_max=%u",
             largest_dma_block, (unsigned)draw_lines, double_buffer ? 2u : 1u,
             (unsigned)KATMED_C3_SPI_TRANSFER_MAX_BYTES(LCD_WIDTH));
#endif
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io, .panel_handle = panel,
        .buffer_size = LCD_HEIGHT * draw_lines, .double_buffer = double_buffer,
        .hres = LCD_WIDTH, .vres = LCD_HEIGHT,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {.mirror_x = PANEL_MIRROR_X_AT_ZERO},
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    lv_display = lvgl_port_add_disp(&disp_cfg);
    if (!lv_display) goto fail;
    if (!lvgl_port_lock(1000)) goto fail;
    lv_touch = lv_indev_create();
    if (lv_touch) {
        lv_indev_set_type(lv_touch, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(lv_touch, lv_display);
        lv_indev_set_read_cb(lv_touch, lv_touch_read);
    }
    lvgl_port_unlock();
    if (!lv_touch) goto fail;
    Orientation initial = current_orientation;
    current_orientation = ORIENTATION_0;
    if (board_set_orientation(initial) != BOARD_OK) goto fail;
    if (board_set_backlight(true) != BOARD_OK) goto fail;
    *display = lv_display;
    return BOARD_OK;
fail:
    board_deinit();
    return BOARD_IO_ERROR;
}
