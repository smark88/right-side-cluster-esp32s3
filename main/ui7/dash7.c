#include "dash7.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "esp_timer.h"

LV_FONT_DECLARE(ui_font_rpm_96);   // 96px digits, same as the round gauges
LV_IMG_DECLARE(c5r_blue_boot);

// ---- theme: identical palette to the round P4 gauges ----------------------
#define C_FACE       0x070707
#define C_RED        0xE01010
#define C_GREEN      0x28FF00
#define C_TRACK      0x232323
#define C_MUTED      0x8A9099
#define C_TILE_LINE  0x2A2E33
#define C_TILE_BG    0x0B0D0F
#define C_GEAR_ON_BG 0x1C2026
#define C_WHITE      0xFFFFFF

#define TILE_W            210
#define TILE_H             96
#define TILE_BORDER_W       2
#define TILE_WARN_BORDER_W  4
#define TILE_WARN_RING_W    4

// ---- tiles and their alarm state ------------------------------------------
enum { T_WATER = 0, T_OIL_TEMP, T_IAT, T_OIL_PSI, T_TRANS, T_BOOST, T_COUNT };

typedef struct {
    lv_obj_t *tile;
    lv_obj_t *val;
    bool      alarm;
    int64_t   hold_until;   // ms; earliest the alarm may clear
} tile_t;

static tile_t    s_t[T_COUNT];
static lv_obj_t *s_dash, *s_boot;
static lv_obj_t *s_rpm_bar, *s_rpm_val, *s_mph, *s_gear;
static lv_obj_t *s_fuel_bar, *s_fuel_val, *s_eth_val;
static lv_obj_t *s_sel_box[5], *s_sel_lbl[5];
static int       s_sel = -2;          // force first paint
static bool      s_flash_phase;
static int       s_rpm_last = -1, s_fuel_last = -1;
static bool      s_rpm_red, s_fuel_red;

static const char SEL_LETTERS[] = "PRNDM";

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// ---- helpers ---------------------------------------------------------------
static lv_obj_t *label(lv_obj_t *p, const char *txt, const lv_font_t *f,
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
static void set_text_if(lv_obj_t *l, const char *txt)
{
    if (strcmp(lv_label_get_text(l), txt) != 0)
        lv_label_set_text(l, txt);
}

static void set_num(lv_obj_t *l, float v, const char *fmt)
{
    char b[16];
    if (isnan(v)) snprintf(b, sizeof b, "--");
    else          snprintf(b, sizeof b, fmt, v);
    set_text_if(l, b);
}

static void make_tile(int slot, lv_obj_t *p, const char *cap, int x, int y)
{
    lv_obj_t *t = lv_obj_create(p);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, TILE_W, TILE_H);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_style_bg_color(t, lv_color_hex(C_TILE_BG), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t, 10, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(C_TILE_LINE), 0);
    lv_obj_set_style_border_width(t, TILE_BORDER_W, 0);
    // Warning ring, invisible until the tile alarms.
    lv_obj_set_style_outline_width(t, TILE_WARN_RING_W, 0);
    lv_obj_set_style_outline_pad(t, 0, 0);
    lv_obj_set_style_outline_color(t, lv_color_hex(C_RED), 0);
    lv_obj_set_style_outline_opa(t, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *c = label(t, cap, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *v = label(t, "--", &lv_font_montserrat_44, C_WHITE);
    lv_obj_align(v, LV_ALIGN_BOTTOM_MID, 0, -8);

    s_t[slot] = (tile_t){ .tile = t, .val = v };
}

// Latch the alarm; the flash timer does the drawing. Callers pass the
// hysteresis-aware condition (compare against CLEAR while already alarmed).
static void set_alarm(int slot, bool on)
{
    int64_t now = now_ms();
    if (on)
        s_t[slot].hold_until = now + WARN_MIN_HOLD_MS;
    else if (s_t[slot].alarm && now < s_t[slot].hold_until)
        return;                                   // still inside the hold
    if (s_t[slot].alarm == on) return;
    s_t[slot].alarm = on;
    if (!on) {
        lv_obj_set_style_outline_opa(s_t[slot].tile, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_t[slot].tile, TILE_BORDER_W, 0);
        lv_obj_set_style_border_color(s_t[slot].tile, lv_color_hex(C_TILE_LINE), 0);
    }
}

static void flash_cb(lv_timer_t *tm)
{
    s_flash_phase = !s_flash_phase;
    for (int i = 0; i < T_COUNT; i++) {
        if (!s_t[i].alarm) continue;
        bool red = s_flash_phase;
        lv_obj_set_style_outline_opa(s_t[i].tile, red ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_t[i].tile,
                                      red ? TILE_WARN_BORDER_W : TILE_BORDER_W, 0);
        lv_obj_set_style_border_color(s_t[i].tile,
                                      lv_color_hex(red ? C_RED : C_TILE_LINE), 0);
    }
}

static lv_obj_t *make_bar(lv_obj_t *p, int x, int y, int w, int h, int max)
{
    lv_obj_t *b = lv_bar_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_bar_set_range(b, 0, max);
    lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_GREEN), LV_PART_INDICATOR);
    // No animation: a tweened bar redraws on every frame of the tween even
    // when the value has not moved.
    lv_obj_set_style_anim_time(b, 0, 0);
    return b;
}

// ---- screens ---------------------------------------------------------------
static void build_dash(void)
{
    s_dash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_dash, lv_color_hex(C_FACE), 0);
    lv_obj_set_style_bg_opa(s_dash, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_dash, LV_OBJ_FLAG_SCROLLABLE);

    // RPM: a straight bar across the top. A wide arc would sweep a much larger
    // dirty rectangle every frame; a bar only redraws the strip that moved.
    s_rpm_bar = make_bar(s_dash, 20, 14, 760, 34, DASH7_RPM_MAX);
    // Redline marker under the bar.
    lv_obj_t *rl = lv_obj_create(s_dash);
    lv_obj_remove_style_all(rl);
    int x0 = 20 + 760 * DASH7_REDLINE / DASH7_RPM_MAX;
    lv_obj_set_pos(rl, x0, 50);
    lv_obj_set_size(rl, 780 - x0, 4);
    lv_obj_set_style_bg_color(rl, lv_color_hex(C_RED), 0);
    lv_obj_set_style_bg_opa(rl, LV_OPA_COVER, 0);
    for (int k = 0; k <= 8; k++) {
        char t[4]; snprintf(t, sizeof t, "%d", k);
        lv_obj_t *l = label(s_dash, t, &lv_font_montserrat_14, C_MUTED);
        lv_obj_set_pos(l, 20 + 760 * k / 8 - (k == 8 ? 10 : 4), 56);
    }

    // Tiles.
    make_tile(T_WATER,    s_dash, "WATER",    20,  90);
    make_tile(T_OIL_TEMP, s_dash, "OIL TEMP", 20, 198);
    make_tile(T_IAT,      s_dash, "IAT",      20, 306);
    make_tile(T_OIL_PSI,  s_dash, "OIL PSI", 570,  90);
    make_tile(T_TRANS,    s_dash, "TRANS",   570, 198);
    make_tile(T_BOOST,    s_dash, "BOOST",   570, 306);

    // Centre column.
    lv_obj_t *rc = label(s_dash, "RPM", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(rc, LV_ALIGN_TOP_MID, -50, 96);
    s_rpm_val = label(s_dash, "0", &lv_font_montserrat_28, C_WHITE);
    lv_obj_align(s_rpm_val, LV_ALIGN_TOP_MID, 20, 88);

    s_mph = label(s_dash, "0", &ui_font_rpm_96, C_WHITE);
    lv_obj_align(s_mph, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_t *mc = label(s_dash, "mph", &lv_font_montserrat_20, C_MUTED);
    lv_obj_align(mc, LV_ALIGN_TOP_MID, 0, 232);

    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = lv_obj_create(s_dash);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 48, 48);
        lv_obj_set_pos(b, 400 - (5 * 48 + 4 * 10) / 2 + i * 58, 276);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_TILE_BG), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(C_TILE_LINE), 0);
        lv_obj_set_style_border_width(b, TILE_BORDER_W, 0);
        char t[2] = { SEL_LETTERS[i], 0 };
        lv_obj_t *l = label(b, t, &lv_font_montserrat_28, C_MUTED);
        lv_obj_center(l);
        s_sel_box[i] = b; s_sel_lbl[i] = l;
    }

    lv_obj_t *gc = label(s_dash, "GEAR", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(gc, LV_ALIGN_TOP_MID, 0, 338);
    s_gear = label(s_dash, "--", &lv_font_montserrat_44, C_WHITE);
    lv_obj_align(s_gear, LV_ALIGN_TOP_MID, 0, 356);

    // Bottom strip: fuel bar and ethanol. The perf monitor sits bottom right.
    lv_obj_t *fc = label(s_dash, "FUEL", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(fc, 20, 432);
    s_fuel_bar = make_bar(s_dash, 70, 424, 470, 34, 100);
    s_fuel_val = label(s_dash, "--", &lv_font_montserrat_20, C_WHITE);
    lv_obj_set_pos(s_fuel_val, 550, 430);
    lv_obj_t *ec = label(s_dash, "ETH", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(ec, 612, 432);
    s_eth_val = label(s_dash, "--", &lv_font_montserrat_20, C_WHITE);
    lv_obj_set_pos(s_eth_val, 646, 430);

    lv_timer_create(flash_cb, WARN_FLASH_MS, NULL);
}

static void boot_done_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    // No fade: a fade redraws all 384k pixels on every step.
    lv_scr_load(s_dash);
    lv_obj_del(s_boot);          // the logo is 500 KB of draw state; free it
    s_boot = NULL;
}

void dash7_create(uint32_t boot_ms)
{
    build_dash();

    s_boot = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_boot, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_boot, LV_OPA_COVER, 0);
    lv_obj_t *img = lv_img_create(s_boot);
    lv_img_set_src(img, &c5r_blue_boot);
    lv_obj_center(img);
    lv_scr_load(s_boot);

    lv_timer_create(boot_done_cb, boot_ms, NULL);
}

// ---- update ----------------------------------------------------------------
static void set_selector(char c)
{
    int sel = -1;
    for (int i = 0; i < 5; i++)
        if (SEL_LETTERS[i] == c) { sel = i; break; }
    if (sel == s_sel) return;
    s_sel = sel;
    for (int i = 0; i < 5; i++) {
        bool on = (i == sel);
        lv_obj_set_style_text_color(s_sel_lbl[i], lv_color_hex(on ? C_WHITE : C_MUTED), 0);
        lv_obj_set_style_border_color(s_sel_box[i], lv_color_hex(on ? C_WHITE : C_TILE_LINE), 0);
        lv_obj_set_style_bg_color(s_sel_box[i], lv_color_hex(on ? C_GEAR_ON_BG : C_TILE_BG), 0);
    }
}

void dash7_update(const dash7_values_t *v)
{
    // RPM bar + number. The bar moves in 25 rpm steps so tiny jitter does not
    // trigger a redraw of the whole strip.
    int rpm = isnan(v->rpm) ? 0 : (int)v->rpm;
    int q = (rpm / 25) * 25;
    if (q != s_rpm_last) {
        s_rpm_last = q;
        lv_bar_set_value(s_rpm_bar, q, LV_ANIM_OFF);
        bool red = q >= DASH7_REDLINE;
        if (red != s_rpm_red) {
            s_rpm_red = red;
            lv_obj_set_style_bg_color(s_rpm_bar, lv_color_hex(red ? C_RED : C_GREEN),
                                      LV_PART_INDICATOR);
        }
    }
    char b[16];
    snprintf(b, sizeof b, "%d", (rpm / 50) * 50);
    set_text_if(s_rpm_val, b);

    if (isnan(v->mph)) set_text_if(s_mph, "--");
    else { snprintf(b, sizeof b, "%d", (int)(v->mph + 0.5f)); set_text_if(s_mph, b); }

    set_selector(v->prndl);
    if (v->gear == -1)                  set_text_if(s_gear, "R");
    else if (v->gear >= 1 && v->gear <= 8) { snprintf(b, sizeof b, "%d", v->gear); set_text_if(s_gear, b); }
    else                                set_text_if(s_gear, "--");

    // Tiles, with the same alarm rules as the round gauges.
    set_num(s_t[T_WATER].val, v->water_f, "%.0f");
    set_alarm(T_WATER, !isnan(v->water_f) &&
        (s_t[T_WATER].alarm ? v->water_f > WARN_WATER_CLEAR : v->water_f > WARN_WATER_MAX));

    set_num(s_t[T_OIL_TEMP].val, v->oil_temp_f, "%.0f");
    set_alarm(T_OIL_TEMP, !isnan(v->oil_temp_f) &&
        (s_t[T_OIL_TEMP].alarm ? v->oil_temp_f > WARN_OIL_TEMP_CLEAR
                               : v->oil_temp_f > WARN_OIL_TEMP_MAX));

    set_num(s_t[T_IAT].val, v->iat_f, "%.0f");
    set_alarm(T_IAT, !isnan(v->iat_f) &&
        (s_t[T_IAT].alarm ? v->iat_f > WARN_IAT_CLEAR : v->iat_f > WARN_IAT_MAX));

    set_num(s_t[T_OIL_PSI].val, v->oil_psi, "%.0f");
    // A stopped engine has no oil pressure; only alarm with it turning.
    set_alarm(T_OIL_PSI, !isnan(v->oil_psi) && rpm >= WARN_OIL_PSI_MIN_RPM &&
        (s_t[T_OIL_PSI].alarm ? v->oil_psi < WARN_OIL_PSI_CLEAR
                              : v->oil_psi < WARN_OIL_PSI_MIN));

    set_num(s_t[T_TRANS].val, v->trans_f, "%.0f");
    set_alarm(T_TRANS, !isnan(v->trans_f) &&
        (s_t[T_TRANS].alarm ? v->trans_f > WARN_TRANS_CLEAR : v->trans_f > WARN_TRANS_MAX));

    set_num(s_t[T_BOOST].val, v->boost_psi, "%.1f");

    // Fuel and ethanol.
    int fuel = isnan(v->fuel_pct) ? 0 : (int)v->fuel_pct;
    if (fuel != s_fuel_last) {
        s_fuel_last = fuel;
        lv_bar_set_value(s_fuel_bar, fuel, LV_ANIM_OFF);
        bool red = fuel < 15;
        if (red != s_fuel_red) {
            s_fuel_red = red;
            lv_obj_set_style_bg_color(s_fuel_bar, lv_color_hex(red ? C_RED : C_GREEN),
                                      LV_PART_INDICATOR);
        }
    }
    set_num(s_fuel_val, v->fuel_pct, "%.0f%%");
    set_num(s_eth_val, v->ethanol_pct, "%.0f%%");
}
