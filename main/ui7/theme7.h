// Palette and small label helpers shared by every 7in screen.
#pragma once

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"

// Identical palette to the round P4 gauges.
#define C_FACE       0x070707
#define C_RED        0xE01010
#define C_GREEN      0x28FF00
#define C_TRACK      0x232323
#define C_MUTED      0xB4BAC2   // captions: light enough to read at a glance
#define C_DIM        0x6A7079   // secondary values (last session)
#define C_TILE_LINE  0x2A2E33
#define C_TILE_BG    0x0B0D0F
#define C_GEAR_ON_BG 0x1C2026
#define C_WHITE      0xFFFFFF
#define C_SHIFT      0x38B8FF   // RPM bar at/after the shift point (bright blue)

static inline lv_obj_t *t7_label(lv_obj_t *p, const char *txt, const lv_font_t *f,
                                 uint32_t color)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

// Only touch LVGL when the text actually changes -- setting identical text
// still invalidates the area and costs a redraw, which on the S3 is the frame
// budget.
static inline void t7_set_text_if(lv_obj_t *l, const char *txt)
{
    if (strcmp(lv_label_get_text(l), txt) != 0)
        lv_label_set_text(l, txt);
}

static inline void t7_set_num(lv_obj_t *l, float v, const char *fmt)
{
    char b[16];
    if (isnan(v)) snprintf(b, sizeof b, "--");
    else          snprintf(b, sizeof b, fmt, v);
    t7_set_text_if(l, b);
}

// A plain black screen, not scrollable, so a horizontal drag anywhere on it
// is a swipe rather than a scroll.
static inline lv_obj_t *t7_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_hex(C_FACE), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

// Flat themed button: dark tile, light outline, caption centred.
static inline lv_obj_t *t7_button(lv_obj_t *p, const char *txt, int x, int y,
                                  int w, int h, uint32_t line)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_TILE_BG), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_GEAR_ON_BG), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(line), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_t *l = t7_label(b, txt, &lv_font_montserrat_20, C_WHITE);
    lv_obj_center(l);
    return b;
}
