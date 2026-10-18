#include "katmed_board.h"
#include "board_geometry.h"
#include "board_protocol.h"
#include "board_indev_gate.h"
#include "board_touch_buffer.h"
#include "board_display_budget.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
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
#endif
static lv_display_t *lv_display;
static lv_indev_t *lv_touch;
static bool spi_ready;
static bool port_ready;
static bool backlight_ready;
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

static void touch_sampler(void *argument)
{
    (void)argument;
    TickType_t last = xTaskGetTickCount();
    while (touch_sampler_active) {
        BoardTouch sample;
        BoardResult result = board_read_touch(&sample);
        portENTER_CRITICAL(&touch_lock);
        board_touch_buffer_push(&touch_samples, sample, result);
        portEXIT_CRITICAL(&touch_lock);
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

static esp_err_t init_touch(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = TOUCH_SDA, .scl_io_num = TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &i2c_bus);
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
    if (err != ESP_OK) { c6_touch = NULL; return err; }
#endif
    return i2c_master_probe(i2c_bus, TOUCH_ADDRESS, I2C_TIMEOUT_MS);
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

BoardResult board_set_backlight(bool enabled)
{
    if (!backlight_ready) return BOARD_INVALID;
    return gpio_set_level(LCD_BACKLIGHT, enabled ? 1 : 0) == ESP_OK ? BOARD_OK : BOARD_IO_ERROR;
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
    current_orientation = orientation;
    portENTER_CRITICAL(&touch_lock);
    latest_valid = false;
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
    err = touch_read_register(0x01, packet, sizeof(packet), true);
    if (err == ESP_OK && !board_c6_touch_decode(packet, &touch->down, &x, &y))
        err = ESP_ERR_INVALID_RESPONSE;
#endif
    const bool read_ok = err == ESP_OK;
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
    portENTER_CRITICAL(&touch_lock);
    latest_touch = *touch;
    latest_valid = result == BOARD_OK;
    latest_base_x = base_x;
    latest_base_y = base_y;
    if (result != BOARD_OK) board_indev_gate_invalidate(&indev_gate);
    portEXIT_CRITICAL(&touch_lock);
    return result;
}

void board_deinit(void)
{
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
#endif
    if (i2c_dev) { (void)i2c_master_bus_rm_device(i2c_dev); i2c_dev = NULL; }
    if (i2c_bus) { (void)i2c_del_master_bus(i2c_bus); i2c_bus = NULL; }
    if (panel) { (void)esp_lcd_panel_del(panel); panel = NULL; }
    if (panel_io) { (void)esp_lcd_panel_io_del(panel_io); panel_io = NULL; }
    if (spi_ready) { (void)spi_bus_free(SPI2_HOST); spi_ready = false; }
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
    if (board_set_backlight(false) != BOARD_OK) goto fail;
    err = init_panel();
    if (err != ESP_OK) goto fail;
    err = init_touch();
    if (err != ESP_OK) goto fail;
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
