/*******************************************************************************
 * Size: 10 px
 * Bpp: 2
 * Opts: --font /usr/share/fonts/dejavu-sans-fonts/DejaVuSansCondensed.ttf -r 0x20-0x7e,0xb0,0xc4,0xd6,0xdc,0xdf,0xe4,0xf6,0xfc --size 10 --bpp 2 --no-compress --format lvgl --lv-font-name katmed_font_10 --output firmware/components/katmed_ui/katmed_font_10.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef KATMED_FONT_10
#define KATMED_FONT_10 1
#endif

#if KATMED_FONT_10

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */

    /* U+0021 "!" */
    0x99, 0x98, 0x80, 0x9,

    /* U+0022 "\"" */
    0x25, 0x25, 0x24,

    /* U+0023 "#" */
    0x2, 0x20, 0x8, 0x82, 0xff, 0x81, 0x20, 0x8,
    0x80, 0xff, 0xd0, 0x88, 0x5, 0x50,

    /* U+0024 "$" */
    0x4, 0x7, 0xd2, 0x40, 0xd0, 0x1f, 0x1, 0x30,
    0x4c, 0xfd, 0x4, 0x0, 0x0,

    /* U+0025 "%" */
    0x28, 0x14, 0x14, 0x48, 0x5, 0x55, 0x0, 0xa2,
    0x0, 0x1, 0x59, 0x0, 0x88, 0x80, 0x52, 0x20,
    0x20, 0x68,

    /* U+0026 "&" */
    0xf, 0x40, 0x90, 0x2, 0x40, 0xb, 0x0, 0x27,
    0x15, 0x46, 0x82, 0xa, 0x7, 0xa9,

    /* U+0027 "'" */
    0x22, 0x20,

    /* U+0028 "(" */
    0x8, 0x83, 0x8, 0x20, 0x83, 0x8, 0x14, 0x10,

    /* U+0029 ")" */
    0x20, 0x81, 0x42, 0x8, 0x21, 0x89, 0x20, 0x40,

    /* U+002A "*" */
    0x8, 0x16, 0x82, 0xc1, 0x68, 0x4, 0x0,

    /* U+002B "+" */
    0x1, 0x0, 0x8, 0x0, 0x20, 0xf, 0xfe, 0x2,
    0x0, 0x8, 0x0, 0x20, 0x0,

    /* U+002C "," */
    0x23, 0x10,

    /* U+002D "-" */
    0x78,

    /* U+002E "." */
    0x13,

    /* U+002F "/" */
    0xc, 0x8, 0x14, 0x20, 0x30, 0x20, 0x50, 0x80,
    0xc0,

    /* U+0030 "0" */
    0x1f, 0x3, 0x8, 0x60, 0xc6, 0xc, 0x60, 0xc6,
    0xc, 0x30, 0x81, 0xf0,

    /* U+0031 "1" */
    0x2d, 0x5, 0x40, 0x50, 0x14, 0x5, 0x1, 0x40,
    0x50, 0xff,

    /* U+0032 "2" */
    0x3f, 0x0, 0x60, 0x8, 0x9, 0x3, 0x3, 0x3,
    0x1, 0xfe,

    /* U+0033 "3" */
    0x3f, 0x0, 0x30, 0xc, 0x3c, 0x1, 0x80, 0x30,
    0x1c, 0xfc,

    /* U+0034 "4" */
    0x3, 0x40, 0xa4, 0xa, 0x42, 0x24, 0x52, 0x4b,
    0xfc, 0x2, 0x40, 0x24,

    /* U+0035 "5" */
    0x3f, 0x4c, 0x3, 0x0, 0xf8, 0x1, 0x80, 0x30,
    0x18, 0xfc,

    /* U+0036 "6" */
    0xf, 0x83, 0x0, 0x20, 0x6, 0xf4, 0x70, 0xc2,
    0xc, 0x30, 0xc1, 0xf4,

    /* U+0037 "7" */
    0x3f, 0xc0, 0x60, 0x24, 0xc, 0x2, 0x2, 0x40,
    0xc0, 0x20,

    /* U+0038 "8" */
    0x2f, 0x43, 0xc, 0x30, 0xc2, 0xf0, 0x20, 0xc6,
    0xc, 0x30, 0xc2, 0xf4,

    /* U+0039 "9" */
    0x2f, 0x6, 0x8, 0x50, 0xc2, 0x1c, 0x2e, 0xc0,
    0xc, 0x2, 0x83, 0xe0,

    /* U+003A ":" */
    0x30, 0x70,

    /* U+003B ";" */
    0x3, 0x0, 0x2, 0x31,

    /* U+003C "<" */
    0x0, 0x0, 0x1, 0xe0, 0x78, 0xe, 0x0, 0x1a,
    0x40, 0x2, 0xd0, 0x0, 0x0,

    /* U+003D "=" */
    0x3f, 0xf8, 0x0, 0x3, 0xff, 0x80,

    /* U+003E ">" */
    0x0, 0x0, 0xe0, 0x0, 0x79, 0x0, 0x1e, 0x2,
    0xe0, 0xb4, 0x1, 0x0, 0x0,

    /* U+003F "?" */
    0x3e, 0x0, 0xc0, 0x30, 0x24, 0x8, 0x2, 0x0,
    0x0, 0x60,

    /* U+0040 "@" */
    0x2, 0xe0, 0x2, 0x2, 0x42, 0x0, 0x20, 0x4a,
    0x94, 0x52, 0x25, 0x55, 0x45, 0x51, 0x22, 0x60,
    0x8a, 0xa0, 0x8, 0x4, 0x0, 0xa8, 0x0,

    /* U+0041 "A" */
    0xa, 0x0, 0x3c, 0x0, 0xa4, 0x9, 0x20, 0x30,
    0xc0, 0xff, 0x45, 0x6, 0x30, 0xc,

    /* U+0042 "B" */
    0x3f, 0x83, 0xc, 0x30, 0xc3, 0xf8, 0x30, 0x93,
    0x5, 0x30, 0x93, 0xf8,

    /* U+0043 "C" */
    0xb, 0xd3, 0x41, 0x60, 0x5, 0x0, 0x50, 0x6,
    0x0, 0x34, 0x10, 0xfd,

    /* U+0044 "D" */
    0x3f, 0x80, 0xc1, 0xc3, 0x3, 0xc, 0x9, 0x30,
    0x24, 0xc0, 0xc3, 0x7, 0xf, 0xe0,

    /* U+0045 "E" */
    0x3f, 0xc3, 0x0, 0x30, 0x3, 0xfc, 0x30, 0x3,
    0x0, 0x30, 0x3, 0xfc,

    /* U+0046 "F" */
    0x3f, 0x8c, 0x3, 0x0, 0xfd, 0x30, 0xc, 0x3,
    0x0, 0xc0,

    /* U+0047 "G" */
    0xb, 0xe0, 0xd0, 0x46, 0x0, 0x14, 0x0, 0x50,
    0xf1, 0x80, 0x83, 0x42, 0x2, 0xf8,

    /* U+0048 "H" */
    0x30, 0x33, 0x3, 0x30, 0x33, 0xff, 0x30, 0x33,
    0x3, 0x30, 0x33, 0x3,

    /* U+0049 "I" */
    0x33, 0x33, 0x33, 0x33,

    /* U+004A "J" */
    0xc, 0x30, 0xc3, 0xc, 0x30, 0xc3, 0x19, 0xc0,

    /* U+004B "K" */
    0x30, 0x90, 0xca, 0x3, 0x60, 0xf, 0x0, 0x3d,
    0x0, 0xcc, 0x3, 0x1c, 0xc, 0x18,

    /* U+004C "L" */
    0x30, 0xc, 0x3, 0x0, 0xc0, 0x30, 0xc, 0x3,
    0x0, 0xff,

    /* U+004D "M" */
    0x34, 0x2c, 0xe0, 0xb3, 0x82, 0xcd, 0x63, 0x32,
    0xcc, 0xce, 0x33, 0x10, 0xcc, 0x3,

    /* U+004E "N" */
    0x34, 0x33, 0x83, 0x38, 0x33, 0x63, 0x33, 0x33,
    0x17, 0x30, 0xf3, 0xb,

    /* U+004F "O" */
    0xf, 0xc0, 0xd0, 0xc6, 0x2, 0x54, 0x6, 0x50,
    0x19, 0x80, 0x93, 0x43, 0x3, 0xf0,

    /* U+0050 "P" */
    0x3f, 0x43, 0xc, 0x30, 0xc3, 0xc, 0x3f, 0x43,
    0x0, 0x30, 0x3, 0x0,

    /* U+0051 "Q" */
    0xf, 0xc0, 0xd0, 0xc6, 0x2, 0x54, 0x6, 0x50,
    0x19, 0x80, 0x93, 0x43, 0x3, 0xf0, 0x0, 0xa0,

    /* U+0052 "R" */
    0x3f, 0x43, 0xc, 0x30, 0xc3, 0xc, 0x3f, 0x43,
    0xc, 0x30, 0x93, 0x3,

    /* U+0053 "S" */
    0x2f, 0x86, 0x0, 0x60, 0x3, 0xd0, 0x6, 0xc0,
    0xc, 0x0, 0xc3, 0xf4,

    /* U+0054 "T" */
    0x3f, 0xf4, 0x8, 0x0, 0x20, 0x0, 0x80, 0x2,
    0x0, 0x8, 0x0, 0x20, 0x0, 0x80,

    /* U+0055 "U" */
    0x20, 0x32, 0x3, 0x20, 0x32, 0x3, 0x20, 0x32,
    0x2, 0x30, 0xa1, 0xf8,

    /* U+0056 "V" */
    0xc0, 0x31, 0x41, 0x83, 0x9, 0xc, 0x30, 0x24,
    0x80, 0x29, 0x0, 0xf0, 0x2, 0x80,

    /* U+0057 "W" */
    0x90, 0xc1, 0x98, 0x74, 0x92, 0x25, 0x30, 0xc8,
    0x8c, 0x32, 0x22, 0x9, 0x89, 0x41, 0xd1, 0xd0,
    0x30, 0x70,

    /* U+0058 "X" */
    0x30, 0x52, 0x4c, 0xe, 0x40, 0xb0, 0xf, 0x1,
    0x98, 0x30, 0xc5, 0x6,

    /* U+0059 "Y" */
    0x20, 0x20, 0x31, 0x80, 0x9c, 0x0, 0xe0, 0x2,
    0x0, 0x8, 0x0, 0x20, 0x0, 0x80,

    /* U+005A "Z" */
    0x7f, 0xe0, 0xc, 0x2, 0x80, 0x30, 0xc, 0x1,
    0x80, 0x30, 0xb, 0xfe,

    /* U+005B "[" */
    0x34, 0x82, 0x8, 0x20, 0x82, 0x8, 0x20, 0xd0,

    /* U+005C "\\" */
    0x80, 0x80, 0x50, 0x20, 0x30, 0x20, 0x14, 0x8,
    0x8,

    /* U+005D "]" */
    0x38, 0x20, 0x82, 0x8, 0x20, 0x82, 0x8, 0xe0,

    /* U+005E "^" */
    0x3, 0x80, 0x32, 0x42, 0x42, 0x0,

    /* U+005F "_" */
    0x3f, 0xd0,

    /* U+0060 "`" */
    0x20, 0x20,

    /* U+0061 "a" */
    0x3e, 0x0, 0x62, 0xf9, 0x82, 0x51, 0x8a, 0xa0,

    /* U+0062 "b" */
    0x20, 0x2, 0x0, 0x2b, 0x43, 0xc, 0x20, 0x82,
    0x8, 0x30, 0xc2, 0xb4,

    /* U+0063 "c" */
    0x1f, 0x4c, 0x5, 0x1, 0x40, 0x30, 0x7, 0xd0,

    /* U+0064 "d" */
    0x0, 0xc0, 0x32, 0xec, 0x87, 0x50, 0xd4, 0x32,
    0xc, 0xab,

    /* U+0065 "e" */
    0x1f, 0x42, 0xc, 0x7f, 0xc5, 0x0, 0x30, 0x1,
    0xf8,

    /* U+0066 "f" */
    0x2d, 0x30, 0xbc, 0x30, 0x30, 0x30, 0x30, 0x30,

    /* U+0067 "g" */
    0x2e, 0xc8, 0x75, 0xd, 0x43, 0x21, 0xcb, 0xb0,
    0x8, 0xbc,

    /* U+0068 "h" */
    0x20, 0x8, 0x2, 0xb4, 0xc3, 0x20, 0xc8, 0x32,
    0xc, 0x83,

    /* U+0069 "i" */
    0x20, 0x22, 0x22, 0x22,

    /* U+006A "j" */
    0x8, 0x0, 0x82, 0x8, 0x20, 0x82, 0x8, 0xc0,

    /* U+006B "k" */
    0x20, 0x2, 0x0, 0x21, 0x82, 0x60, 0x38, 0x2,
    0xc0, 0x23, 0x2, 0x18,

    /* U+006C "l" */
    0x22, 0x22, 0x22, 0x22,

    /* U+006D "m" */
    0x2b, 0x6d, 0x30, 0xc3, 0x20, 0xc3, 0x20, 0xc3,
    0x20, 0xc3, 0x20, 0xc3,

    /* U+006E "n" */
    0x2b, 0x4c, 0x32, 0xc, 0x83, 0x20, 0xc8, 0x30,

    /* U+006F "o" */
    0x2f, 0x2, 0x8, 0x50, 0xc5, 0xc, 0x20, 0x82,
    0xf0,

    /* U+0070 "p" */
    0x2a, 0x43, 0xc, 0x20, 0x82, 0x8, 0x30, 0xc2,
    0xb4, 0x20, 0x2, 0x0,

    /* U+0071 "q" */
    0x2e, 0xc8, 0x75, 0xd, 0x43, 0x21, 0xcb, 0xb0,
    0xc, 0x3,

    /* U+0072 "r" */
    0x0, 0x2a, 0x30, 0x20, 0x20, 0x20, 0x20,

    /* U+0073 "s" */
    0x3f, 0x14, 0x3, 0x40, 0x1c, 0x2, 0x1f, 0x80,

    /* U+0074 "t" */
    0x10, 0x20, 0xbc, 0x20, 0x20, 0x20, 0x20, 0x2d,

    /* U+0075 "u" */
    0x20, 0xc8, 0x32, 0xc, 0x83, 0x20, 0xca, 0xb0,

    /* U+0076 "v" */
    0x90, 0xc6, 0x18, 0x32, 0x42, 0x30, 0x1a, 0x0,
    0xd0,

    /* U+0077 "w" */
    0x93, 0x4d, 0x8d, 0x22, 0x59, 0x4e, 0x28, 0x2c,
    0xb0, 0x61, 0xc0,

    /* U+0078 "x" */
    0x61, 0x82, 0x70, 0xd, 0x0, 0xe0, 0x33, 0x6,
    0x8,

    /* U+0079 "y" */
    0x90, 0xc2, 0x18, 0x32, 0x2, 0x70, 0xe, 0x0,
    0xc0, 0xc, 0x3, 0x40,

    /* U+007A "z" */
    0x7f, 0x40, 0xc0, 0xc0, 0x60, 0x30, 0x2f, 0xd0,

    /* U+007B "{" */
    0xd, 0x14, 0x24, 0x24, 0xb0, 0x20, 0x24, 0x24,
    0x14, 0x9,

    /* U+007C "|" */
    0xff, 0xff, 0xf4,

    /* U+007D "}" */
    0xb0, 0x20, 0x20, 0x24, 0xd, 0x14, 0x20, 0x20,
    0x20, 0xa0,

    /* U+007E "~" */
    0x0, 0x0, 0xb8, 0x11, 0x1e, 0x0, 0x0,

    /* U+00B0 "°" */
    0x18, 0x22, 0x22, 0x18,

    /* U+00C4 "Ä" */
    0x9, 0x40, 0x0, 0x0, 0xa0, 0x3, 0xc0, 0xa,
    0x40, 0x92, 0x3, 0xc, 0xf, 0xf4, 0x50, 0x63,
    0x0, 0xc0,

    /* U+00D6 "Ö" */
    0x8, 0xc0, 0x0, 0x0, 0xfc, 0xd, 0xc, 0x60,
    0x25, 0x40, 0x65, 0x1, 0x98, 0x9, 0x34, 0x30,
    0x3f, 0x0,

    /* U+00DC "Ü" */
    0xd, 0x80, 0x0, 0x20, 0x32, 0x3, 0x20, 0x32,
    0x3, 0x20, 0x32, 0x2, 0x30, 0xa1, 0xf8,

    /* U+00DF "ß" */
    0x1f, 0x3, 0x8, 0x22, 0x42, 0x50, 0x23, 0x2,
    0xc, 0x20, 0x82, 0xb8,

    /* U+00E4 "ä" */
    0x23, 0x0, 0x3, 0xe0, 0x6, 0x2f, 0x98, 0x25,
    0x18, 0xaa,

    /* U+00F6 "ö" */
    0x27, 0x0, 0x0, 0x2f, 0x2, 0x8, 0x50, 0xc5,
    0xc, 0x20, 0x82, 0xf0,

    /* U+00FC "ü" */
    0x27, 0x0, 0x2, 0xc, 0x83, 0x20, 0xc8, 0x32,
    0xc, 0xab
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 46, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 0, .adv_w = 58, .box_w = 2, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 4, .adv_w = 66, .box_w = 4, .box_h = 3, .ofs_x = 0, .ofs_y = 5},
    {.bitmap_index = 7, .adv_w = 121, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 21, .adv_w = 92, .box_w = 5, .box_h = 10, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 34, .adv_w = 137, .box_w = 9, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 52, .adv_w = 112, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 66, .adv_w = 40, .box_w = 2, .box_h = 3, .ofs_x = 0, .ofs_y = 5},
    {.bitmap_index = 68, .adv_w = 56, .box_w = 3, .box_h = 10, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 76, .adv_w = 56, .box_w = 3, .box_h = 10, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 84, .adv_w = 72, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 91, .adv_w = 121, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 104, .adv_w = 46, .box_w = 2, .box_h = 3, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 106, .adv_w = 52, .box_w = 3, .box_h = 1, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 107, .adv_w = 46, .box_w = 2, .box_h = 2, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 108, .adv_w = 49, .box_w = 4, .box_h = 9, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 117, .adv_w = 92, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 129, .adv_w = 92, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 139, .adv_w = 92, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 149, .adv_w = 92, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 159, .adv_w = 92, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 171, .adv_w = 92, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 181, .adv_w = 92, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 193, .adv_w = 92, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 203, .adv_w = 92, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 215, .adv_w = 92, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 227, .adv_w = 49, .box_w = 1, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 229, .adv_w = 49, .box_w = 2, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 233, .adv_w = 121, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 246, .adv_w = 121, .box_w = 7, .box_h = 3, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 252, .adv_w = 121, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 265, .adv_w = 76, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 275, .adv_w = 144, .box_w = 9, .box_h = 10, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 298, .adv_w = 98, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 312, .adv_w = 99, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 324, .adv_w = 101, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 336, .adv_w = 111, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 350, .adv_w = 91, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 362, .adv_w = 83, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 372, .adv_w = 112, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 386, .adv_w = 108, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 398, .adv_w = 42, .box_w = 2, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 402, .adv_w = 42, .box_w = 3, .box_h = 10, .ofs_x = -1, .ofs_y = -2},
    {.bitmap_index = 410, .adv_w = 94, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 424, .adv_w = 80, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 434, .adv_w = 124, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 448, .adv_w = 108, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 460, .adv_w = 113, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 474, .adv_w = 87, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 486, .adv_w = 113, .box_w = 7, .box_h = 9, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 502, .adv_w = 100, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 514, .adv_w = 91, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 526, .adv_w = 88, .box_w = 7, .box_h = 8, .ofs_x = -1, .ofs_y = 0},
    {.bitmap_index = 540, .adv_w = 105, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 552, .adv_w = 98, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 566, .adv_w = 142, .box_w = 9, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 584, .adv_w = 99, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 596, .adv_w = 88, .box_w = 7, .box_h = 8, .ofs_x = -1, .ofs_y = 0},
    {.bitmap_index = 610, .adv_w = 99, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 622, .adv_w = 56, .box_w = 3, .box_h = 10, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 630, .adv_w = 49, .box_w = 4, .box_h = 9, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 639, .adv_w = 56, .box_w = 3, .box_h = 10, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 647, .adv_w = 121, .box_w = 7, .box_h = 3, .ofs_x = 0, .ofs_y = 5},
    {.bitmap_index = 653, .adv_w = 72, .box_w = 6, .box_h = 1, .ofs_x = -1, .ofs_y = -3},
    {.bitmap_index = 655, .adv_w = 72, .box_w = 3, .box_h = 2, .ofs_x = 0, .ofs_y = 7},
    {.bitmap_index = 657, .adv_w = 88, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 665, .adv_w = 91, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 677, .adv_w = 79, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 685, .adv_w = 91, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 695, .adv_w = 89, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 704, .adv_w = 51, .box_w = 4, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 712, .adv_w = 91, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 722, .adv_w = 91, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 732, .adv_w = 40, .box_w = 2, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 736, .adv_w = 40, .box_w = 3, .box_h = 10, .ofs_x = -1, .ofs_y = -2},
    {.bitmap_index = 744, .adv_w = 83, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 756, .adv_w = 40, .box_w = 2, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 760, .adv_w = 140, .box_w = 8, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 772, .adv_w = 91, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 780, .adv_w = 88, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 789, .adv_w = 91, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 801, .adv_w = 91, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 811, .adv_w = 59, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 818, .adv_w = 75, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 826, .adv_w = 56, .box_w = 4, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 834, .adv_w = 91, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 842, .adv_w = 85, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 851, .adv_w = 118, .box_w = 7, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 862, .adv_w = 85, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 871, .adv_w = 85, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 883, .adv_w = 76, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 891, .adv_w = 92, .box_w = 4, .box_h = 10, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 901, .adv_w = 49, .box_w = 1, .box_h = 11, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 904, .adv_w = 92, .box_w = 4, .box_h = 10, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 914, .adv_w = 121, .box_w = 7, .box_h = 4, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 921, .adv_w = 72, .box_w = 4, .box_h = 4, .ofs_x = 0, .ofs_y = 4},
    {.bitmap_index = 925, .adv_w = 98, .box_w = 7, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 943, .adv_w = 113, .box_w = 7, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 961, .adv_w = 105, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 976, .adv_w = 91, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 988, .adv_w = 88, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 998, .adv_w = 88, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1010, .adv_w = 91, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = 0}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_1[] = {
    0x0, 0x14, 0x26, 0x2c, 0x2f, 0x34, 0x46, 0x4c
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 95, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    },
    {
        .range_start = 176, .range_length = 77, .glyph_id_start = 96,
        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, .list_length = 8, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 2,
    .bpp = 2,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t katmed_font_10 = {
#else
lv_font_t katmed_font_10 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 13,          /*The maximum line height required by the font*/
    .base_line = 3,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = 0,
    .underline_thickness = 0,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if KATMED_FONT_10*/
