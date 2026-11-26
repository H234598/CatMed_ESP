#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
typedef struct panel_io *esp_lcd_panel_io_handle_t;
typedef struct { int unused; } esp_lcd_panel_t;
typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
} jd9853_panel_t;

enum {
    ESP_OK = 0,
    LCD_CMD_CASET = 0x2a,
    LCD_CMD_RASET = 0x2b,
    LCD_CMD_RAMWR = 0x2c,
};

#define __containerof(pointer, type, member) \
    ((type *)((char *)(pointer) - offsetof(type, member)))
#define ESP_RETURN_ON_ERROR(expression, tag, message) do { \
    esp_err_t error = (expression); \
    if (error != ESP_OK) { \
        return error; \
    } \
} while (0)

struct panel_io {
    int reject;
    esp_err_t error;
    unsigned int calls;
    const void *pixels;
};

static esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int command,
                                        const void *data, size_t length)
{
    static const uint8_t columns[] = {0x01, 0x00, 0x01, 0x02};
    static const uint8_t rows[] = {0x00, 0x12, 0x00, 0x13};
    assert(io->calls < 2);
    assert(command == (io->calls == 0 ? LCD_CMD_CASET : LCD_CMD_RASET));
    assert(length == sizeof(columns));
    assert(memcmp(data, io->calls == 0 ? columns : rows, length) == 0);
    ++io->calls;
    return command == io->reject ? io->error : ESP_OK;
}

static esp_err_t esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t io, int command,
                                        const void *data, size_t length)
{
    assert(io->calls == 2);
    assert(command == LCD_CMD_RAMWR);
    assert(data == io->pixels);
    assert(length == 12);
    ++io->calls;
    return command == io->reject ? io->error : ESP_OK;
}

static esp_err_t panel_jd9853_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                        int x_end, int y_end, const void *color_data);

int main(void)
{
    static const struct {
        const char *name;
        int reject;
        esp_err_t result;
        unsigned int calls;
    } cases[] = {
        {"accepted", 0, ESP_OK, 3},
        {"CASET rejected", LCD_CMD_CASET, 0x101, 1},
        {"RASET rejected", LCD_CMD_RASET, 0x102, 2},
        {"RAMWR rejected", LCD_CMD_RAMWR, 0x103, 3},
    };
    const uint16_t pixels[6] = {0};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct panel_io io = {
            .reject = cases[i].reject,
            .error = cases[i].result,
            .pixels = pixels,
        };
        jd9853_panel_t panel = {
            .io = &io,
            .x_gap = 34,
            .y_gap = 7,
            .fb_bits_per_pixel = 16,
        };
        esp_err_t result = panel_jd9853_draw_bitmap(&panel.base, 222, 11, 225, 13, pixels);
        if (result != cases[i].result || io.calls != cases[i].calls) {
            fprintf(stderr, "JD9853 transfer contract failed (%s)\n", cases[i].name);
            return 1;
        }
    }
    puts("JD9853 transfer contract passed (4 cases)");
    return 0;
}
