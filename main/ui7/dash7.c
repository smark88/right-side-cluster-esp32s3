#include "dash7.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "esp_timer.h"
#include "fastbar.h"

LV_FONT_DECLARE(ui_font_rpm_96);   // 96px digits, same as the round gauges
LV_FONT_DECLARE(ui_font_gear_72);  // 72px digits + R + -, uncompressed
LV_IMG_DECLARE(c5r_blue_boot);

// ---- theme: identical palette to the round P4 gauges ----------------------
#define C_FACE       0x070707
#define C_RED        0xE01010
#define C_GREEN      0x28FF00
#define C_TRACK      0x232323
#define C_MUTED      0xB4BAC2   // captions: light enough to read at a glance
#define C_TILE_LINE  0x2A2E33
#define C_TILE_BG    0x0B0D0F
#define C_GEAR_ON_BG 0x1C2026
#define C_WHITE      0xFFFFFF
#define C_SHIFT      0x38B8FF   // RPM bar at/after the shift point (bright blue)

// Shift light: from DASH7_SHIFT_RPM the bar blinks blue/off at this period.
// Fast enough to catch in peripheral vision. Each toggle repaints the whole
// bar, which is affordable at 10 Hz but not every frame.
#define SHIFT_BLINK_MS  100

static void shift_cb(lv_timer_t *t);

#define TILE_W            210
#define TILE_H             96
#define TILE_BORDER_W       2
#define TILE_WARN_BORDER_W  4
#define TILE_WARN_RING_W    4

#define BAR_Y              10    // RPM bar: the only tach, so it is tall
#define BAR_H              54
#define TILE_Y0           100    // first tile row, clear of the bar's numbers

#define SEL_BOX            36    // PRNDM box size
#define SEL_GAP             8
#define SEL_Y             368    // just above the fuel bar at 424

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
static lv_obj_t *s_rpm_bar, *s_mph, *s_gear, *s_no_can;
static bool      s_no_can_shown;
static lv_obj_t *s_fuel_bar, *s_fuel_val, *s_eth_val, *s_odo_val;
static lv_obj_t *s_sel_box[5], *s_sel_lbl[5];
static int       s_sel = -2;          // force first paint
static bool      s_flash_phase;
static int       s_rpm_last = -1, s_fuel_last = -1;
static bool      s_shift, s_shift_on, s_fuel_red;

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

    lv_obj_t *c = label(t, cap, &lv_font_montserrat_20, C_MUTED);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 6);

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


// ---- screens ---------------------------------------------------------------
static void build_dash(void)
{
    s_dash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_dash, lv_color_hex(C_FACE), 0);
    lv_obj_set_style_bg_opa(s_dash, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_dash, LV_OBJ_FLAG_SCROLLABLE);

    // RPM: a straight bar across the top. A wide arc would sweep a much larger
    // dirty rectangle every frame; a bar only redraws the strip that moved.
    s_rpm_bar = fastbar_create(s_dash, 20, BAR_Y, 760, BAR_H, DASH7_RPM_MAX,
                               C_TRACK, C_GREEN, 6);
    // Redline marker under the bar.
    lv_obj_t *rl = lv_obj_create(s_dash);
    lv_obj_remove_style_all(rl);
    int x0 = 20 + 760 * DASH7_SHIFT_RPM / DASH7_RPM_MAX;
    lv_obj_set_pos(rl, x0, BAR_Y + BAR_H + 2);
    lv_obj_set_size(rl, 780 - x0, 4);
    lv_obj_set_style_bg_color(rl, lv_color_hex(C_RED), 0);
    lv_obj_set_style_bg_opa(rl, LV_OPA_COVER, 0);
    for (int k = 0; k <= DASH7_RPM_MAX / 1000; k++) {
        char t[4]; snprintf(t, sizeof t, "%d", k);
        lv_obj_t *l = label(s_dash, t, &lv_font_montserrat_20, C_MUTED);
        int last = DASH7_RPM_MAX / 1000;
        lv_obj_set_pos(l, 20 + 760 * k / last - (k == last ? 12 : 5), BAR_Y + BAR_H + 8);
    }
    // Over the bar, which is empty anyway when the car is not answering.
    s_no_can = label(s_dash, "NO CAN", &lv_font_montserrat_28, C_RED);
    lv_obj_align(s_no_can, LV_ALIGN_TOP_MID, 0, BAR_Y + (BAR_H - 28) / 2);
    lv_obj_add_flag(s_no_can, LV_OBJ_FLAG_HIDDEN);

    // Tiles.
    make_tile(T_WATER,    s_dash, "WATER",    20, TILE_Y0);
    make_tile(T_OIL_TEMP, s_dash, "OIL TEMP", 20, TILE_Y0 + 108);
    make_tile(T_IAT,      s_dash, "IAT",      20, TILE_Y0 + 216);
    make_tile(T_OIL_PSI,  s_dash, "OIL PSI", 570, TILE_Y0);
    make_tile(T_TRANS,    s_dash, "TRANS",   570, TILE_Y0 + 108);
    make_tile(T_BOOST,    s_dash, "BOOST",   570, TILE_Y0 + 216);

    // Centre column.
    // The bar is the only tach -- no digital RPM.
    s_mph = label(s_dash, "0", &ui_font_rpm_96, C_WHITE);
    lv_obj_align(s_mph, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_t *mc = label(s_dash, "mph", &lv_font_montserrat_20, C_MUTED);
    lv_obj_align(mc, LV_ALIGN_TOP_MID, 0, 194);

    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = lv_obj_create(s_dash);
        lv_obj_remove_style_all(b);
        // Small, along the bottom of the centre column just above the fuel
        // bar: the selector is glanced at, not read.
        lv_obj_set_size(b, SEL_BOX, SEL_BOX);
        lv_obj_set_pos(b, 400 - (5 * SEL_BOX + 4 * SEL_GAP) / 2 + i * (SEL_BOX + SEL_GAP), SEL_Y);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_TILE_BG), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(C_TILE_LINE), 0);
        lv_obj_set_style_border_width(b, TILE_BORDER_W, 0);
        char t[2] = { SEL_LETTERS[i], 0 };
        lv_obj_t *l = label(b, t, &lv_font_montserrat_20, C_MUTED);
        lv_obj_center(l);
        s_sel_box[i] = b; s_sel_lbl[i] = l;
    }

    lv_obj_t *gc = label(s_dash, "GEAR", &lv_font_montserrat_20, C_MUTED);
    lv_obj_align(gc, LV_ALIGN_TOP_MID, 0, 244);
    // 72px: Montserrat's largest built-in is 48, and the 96px MPH font has no
    // 'R' for reverse, so this one is generated with just digits, R and -.
    s_gear = label(s_dash, "--", &ui_font_gear_72, C_WHITE);
    lv_obj_align(s_gear, LV_ALIGN_TOP_MID, 0, 268);

    // Bottom strip: fuel bar, ethanol, odometer. The perf monitor (when
    // enabled) sits in the bottom-right corner, right of the odometer.
    lv_obj_t *fc = label(s_dash, "FUEL", &lv_font_montserrat_20, C_MUTED);
    lv_obj_set_pos(fc, 20, 430);
    s_fuel_bar = fastbar_create(s_dash, 80, 424, 280, 34, 100, C_TRACK, C_GREEN, 6);
    s_fuel_val = label(s_dash, "--", &lv_font_montserrat_20, C_WHITE);
    lv_obj_set_pos(s_fuel_val, 370, 430);
    lv_obj_t *ec = label(s_dash, "ETH", &lv_font_montserrat_20, C_MUTED);
    lv_obj_set_pos(ec, 432, 430);
    s_eth_val = label(s_dash, "--", &lv_font_montserrat_20, C_WHITE);
    lv_obj_set_pos(s_eth_val, 478, 430);
    lv_obj_t *oc = label(s_dash, "ODO", &lv_font_montserrat_20, C_MUTED);
    lv_obj_set_pos(oc, 554, 430);
    s_odo_val = label(s_dash, "--", &lv_font_montserrat_20, C_WHITE);
    lv_obj_set_pos(s_odo_val, 624, 430);

    lv_timer_create(flash_cb, WARN_FLASH_MS, NULL);
    lv_timer_create(shift_cb, SHIFT_BLINK_MS, NULL);
}

static void shift_cb(lv_timer_t *t)
{
    if (!s_shift) return;
    s_shift_on = !s_shift_on;
    // Off is the track colour, so the bar visibly empties -- a real flash
    // rather than a colour shift.
    fastbar_set_fill(s_rpm_bar, s_shift_on ? C_SHIFT : C_TRACK);
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
    if (v->no_can != s_no_can_shown) {
        s_no_can_shown = v->no_can;
        if (v->no_can) lv_obj_clear_flag(s_no_can, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(s_no_can, LV_OBJ_FLAG_HIDDEN);
    }

    // RPM bar + number. The bar moves in 25 rpm steps so tiny jitter does not
    // trigger a redraw of the whole strip.
    int rpm = isnan(v->rpm) ? 0 : (int)v->rpm;
    int q = (rpm / 25) * 25;
    if (q != s_rpm_last) {
        s_rpm_last = q;
        fastbar_set_value(s_rpm_bar, q);
        // Entering the shift band starts the blink lit; leaving it puts the
        // bar straight back to green. The blink itself runs in shift_cb.
        bool shift = q >= DASH7_SHIFT_RPM;
        if (shift != s_shift) {
            s_shift = shift;
            s_shift_on = shift;
            fastbar_set_fill(s_rpm_bar, shift ? C_SHIFT : C_GREEN);
        }
    }
    char b[16];

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
    // A stopped engine has no oil pressure; only alarm with it turning, past
    // the start-up lag, and once the low reading has persisted.
    {
        static int64_t turning_since, low_since;
        int64_t now = now_ms();
        bool turning = rpm >= WARN_OIL_PSI_MIN_RPM;
        if (!turning)            turning_since = 0;
        else if (!turning_since) turning_since = now;

        bool alarmed = s_t[T_OIL_PSI].alarm;
        bool low = !isnan(v->oil_psi) && turning &&
                   now - turning_since >= WARN_OIL_PSI_START_MS &&
                   v->oil_psi < (alarmed ? WARN_OIL_PSI_CLEAR : WARN_OIL_PSI_MIN);
        if (!low)            low_since = 0;
        else if (!low_since) low_since = now;

        set_alarm(T_OIL_PSI, low && (alarmed || now - low_since >= WARN_OIL_PSI_SUSTAIN_MS));
    }

    set_num(s_t[T_TRANS].val, v->trans_f, "%.0f");
    set_alarm(T_TRANS, !isnan(v->trans_f) &&
        (s_t[T_TRANS].alarm ? v->trans_f > WARN_TRANS_CLEAR : v->trans_f > WARN_TRANS_MAX));

    // Signed: vacuum shows negative (about -9 psi at idle), boost positive.
    set_num(s_t[T_BOOST].val, v->boost_psi, "%.1f");

    // Fuel and ethanol.
    int fuel = isnan(v->fuel_pct) ? 0 : (int)v->fuel_pct;
    if (fuel != s_fuel_last) {
        s_fuel_last = fuel;
        fastbar_set_value(s_fuel_bar, fuel);
        bool red = fuel < 15;
        if (red != s_fuel_red) {
            s_fuel_red = red;
            fastbar_set_fill(s_fuel_bar, red ? C_RED : C_GREEN);
        }
    }
    set_num(s_fuel_val, v->fuel_pct, "%.0f%%");
    // One decimal, as HP Tuners shows it: the ECM reports in ~0.4% steps.
    set_num(s_eth_val, v->ethanol_pct, "%.1f%%");

    // Tenths change every ~6s at 60 mph, so this almost never repaints.
    char ob[16];
    if (isnan(v->odo_miles)) snprintf(ob, sizeof ob, "--");
    else                     snprintf(ob, sizeof ob, "%.1f", v->odo_miles);
    set_text_if(s_odo_val, ob);
}
