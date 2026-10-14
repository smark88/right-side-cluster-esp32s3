#include "dash7.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "esp_timer.h"
#include "fastbar.h"
#include "theme7.h"

LV_FONT_DECLARE(ui_font_mph_150);  // 150px digits and '-': the speed
LV_FONT_DECLARE(ui_font_gear_104); // 104px digits, R and '-'
LV_FONT_DECLARE(ui_font_unit_16);  // °F, psi, %, mi -- the built-ins have no °
LV_FONT_DECLARE(ui_font_rpm_96);   // the takeover's value
LV_IMG_DECLARE(c5r_blue_boot);

// Tile state colours, plus the shift dots' bands.
#define C_COLD     0x4AA8FF
#define C_WARM     0xF2A900
#define C_ODO      0xC9CDD3
#define C_DOT_OFF  0x121418
#define C_DOT_Y    0xFFE500
#define C_DOT_O    0xFF8A00
#define C_DOT_R    0xFF2020

// Shift light: from DASH7_SHIFT_RPM every dot blinks blue/off at this period.
// Fast enough to catch in peripheral vision; 15 small dots are cheap to redraw.
#define SHIFT_BLINK_MS  100

static void shift_cb(lv_timer_t *t);

// ---- layout (800 x 480), matching the "Revised" mockup ------------------------
#define DOT_N        15
#define DOT_D        30
#define DOT_Y        17
#define DOT_LEVELS    8               // 7 mirrored pairs + the centre dot

#define GEAR_CX     105               // left column centre
#define RIGHT_X     615               // right column, 160 wide
#define RIGHT_W     160

#define SEL_BOX      26               // PRNDM box size
#define SEL_GAP       6
#define SEL_Y       240

#define TILE_ROW_Y  352
#define TILE_N        6

// ---- tiles and their alarm state ------------------------------------------
// Left to right, grouped: engine | trans | intake.
enum { T_OIL_PSI = 0, T_OIL_TEMP, T_WATER, T_TRANS, T_IAT, T_BOOST, T_COUNT };
enum { ST_NONE, ST_COLD, ST_OK, ST_WARM };   // ST_NONE: no reading, white

typedef struct {
    lv_obj_t *tile, *strip, *cap, *val, *unit;
    int       state;        // ST_* colour, ignored while alarmed
    bool      alarm;
    int64_t   hold_until;   // ms; earliest the alarm may clear
    int64_t   since;        // ms; when the alarm came on
    bool      acked;        // takeover dismissed; re-arms when the alarm clears
    float     last;         // latest value, for the takeover
} tile_t;

static const struct { const char *cap, *unit; bool group; } TILE_DEF[T_COUNT] = {
    [T_OIL_PSI]  = { "OIL PSI",  "psi", false },
    [T_OIL_TEMP] = { "OIL TEMP", "°F",  false },
    [T_WATER]    = { "WATER",    "°F",  false },
    [T_TRANS]    = { "TRANS",    "°F",  true  },   // heavier divider before
    [T_IAT]      = { "IAT",      "°F",  true  },
    [T_BOOST]    = { "BOOST",    "psi", false },
};

static tile_t    s_t[T_COUNT];
static lv_obj_t *s_dash, *s_boot;
static lv_obj_t *s_dot[DOT_N];
static lv_obj_t *s_mph, *s_gear, *s_no_can, *s_demo;
static bool      s_no_can_shown, s_demo_shown;
static lv_obj_t *s_fuel_bar, *s_fuel_val, *s_eth_val, *s_odo_val;
static lv_obj_t *s_sel_box[5], *s_sel_lbl[5];
static int       s_sel = -2;          // force first paint
static bool      s_flash_phase;
static int       s_lit = -1, s_fuel_last = -1;
static bool      s_shift, s_shift_on, s_fuel_red;

static const char SEL_LETTERS[] = "PRNDM";

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// ---- critical-fault takeover -------------------------------------------------
// A critical alarm that has held for its delay covers the whole display, on
// whichever page is showing, until it clears or is tapped away. Tapping only
// silences that alarm until it clears; it comes back if the fault recurs.
// Highest priority first: oil pressure kills an engine in seconds.
// Oil pressure is quicker than the temperatures: its tile already waits 1s
// of sustained low pressure, so this is ~4.5s from the drop, where a
// temperature has minutes of margin and gets the full 5s.
static const struct { int slot; int delay_ms; const char *title, *unit; } CRIT[] = {
    { T_OIL_PSI , 3500, "LOW OIL PRESSURE", "PSI" },
    { T_WATER   , 5000, "COOLANT HOT",      "F"   },
    { T_OIL_TEMP, 5000, "OIL TEMP HIGH",    "F"   },
    { T_TRANS   , 5000, "TRANS TEMP HIGH",  "F"   },
};
#define N_CRIT (int)(sizeof CRIT / sizeof CRIT[0])
static lv_obj_t *s_to, *s_to_title, *s_to_val, *s_to_unit;
static int       s_to_k = -1;           // CRIT index showing, -1 none

// ---- helpers ---------------------------------------------------------------
#define label       t7_label
#define set_text_if t7_set_text_if
#define set_num     t7_set_num

static lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// A transparent flex row: a big value and its small unit, bottoms aligned.
static lv_obj_t *value_row(lv_obj_t *p, const lv_font_t *vf, lv_obj_t **val,
                           const char *unit, lv_obj_t **unit_out, int gap)
{
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(r, gap, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    *val = label(r, "--", vf, C_WHITE);
    lv_obj_t *u = label(r, unit, &ui_font_unit_16, C_MUTED);
    lv_obj_set_style_pad_bottom(u, 5, 0);   // sit on the big digits' baseline
    if (unit_out) *unit_out = u;
    return r;
}

static lv_obj_t *caption(lv_obj_t *p, const char *txt)
{
    lv_obj_t *c = label(p, txt, &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_style_text_letter_space(c, 2, 0);
    return c;
}

// ---- tiles -----------------------------------------------------------------
static uint32_t state_color(int st)
{
    switch (st) {
        case ST_COLD: return C_COLD;
        case ST_OK:   return C_GREEN;
        case ST_WARM: return C_WARM;
        default:      return C_WHITE;
    }
}

static void paint_tile(int i)
{
    tile_t *t = &s_t[i];
    if (t->alarm) {
        // Whole tile red at once; flash_cb then toggles the background while
        // the text stays white.
        lv_obj_set_style_bg_color(t->tile, lv_color_hex(C_RED), 0);
        lv_obj_set_style_bg_opa(t->tile, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(t->strip, lv_color_hex(C_RED), 0);
        lv_obj_set_style_text_color(t->cap,  lv_color_hex(C_WHITE), 0);
        lv_obj_set_style_text_color(t->val,  lv_color_hex(C_WHITE), 0);
        lv_obj_set_style_text_color(t->unit, lv_color_hex(C_WHITE), 0);
        return;
    }
    uint32_t c = state_color(t->state);
    lv_obj_set_style_bg_opa(t->tile, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(t->strip, lv_color_hex(t->state == ST_NONE ? C_TILE_LINE : c), 0);
    lv_obj_set_style_text_color(t->cap,  lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_color(t->val,  lv_color_hex(c), 0);
    lv_obj_set_style_text_color(t->unit, lv_color_hex(C_MUTED), 0);
}

static void set_state(int i, int st)
{
    if (s_t[i].state == st) return;
    s_t[i].state = st;
    if (!s_t[i].alarm) paint_tile(i);
}

// Three-band colour for a reading: cold below `cold` (NAN = no cold band),
// warm from `warm`.
static int band(float v, float cold, float warm)
{
    if (isnan(v))                    return ST_NONE;
    if (!isnan(cold) && v < cold)    return ST_COLD;
    if (v >= warm)                   return ST_WARM;
    return ST_OK;
}

static void make_tile(int i)
{
    int x0 = 20 + 760 * i / TILE_N, x1 = 20 + 760 * (i + 1) / TILE_N;
    lv_obj_t *t = box(s_dash, x0, TILE_ROW_Y, x1 - x0, 480 - TILE_ROW_Y, C_RED);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);

    lv_obj_t *strip = box(t, 0, 0, x1 - x0, 4, C_TILE_LINE);
    lv_obj_t *cap = caption(t, TILE_DEF[i].cap);
    lv_obj_set_pos(cap, 14, 22);

    lv_obj_t *val, *unit;
    lv_obj_t *row = value_row(t, &lv_font_montserrat_40, &val, TILE_DEF[i].unit, &unit, 4);
    lv_obj_set_pos(row, 14, 54);

    s_t[i] = (tile_t){ .tile = t, .strip = strip, .cap = cap, .val = val,
                       .unit = unit, .state = -1, .last = NAN };
    set_state(i, ST_NONE);

    // Divider on its left: heavier where a new group starts.
    if (i > 0) {
        bool g = TILE_DEF[i].group;
        box(s_dash, x0 - (g ? 1 : 0), TILE_ROW_Y, g ? 3 : 1, 480 - TILE_ROW_Y,
            g ? 0x3A3F46 : C_TILE_LINE);
    }
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
    if (on) s_t[slot].since = now;
    paint_tile(slot);
}

static void flash_cb(lv_timer_t *tm)
{
    s_flash_phase = !s_flash_phase;
    if (s_to_k >= 0)
        lv_obj_set_style_bg_color(s_to, lv_color_hex(s_flash_phase ? C_RED : 0x000000), 0);
    for (int i = 0; i < T_COUNT; i++) {
        if (!s_t[i].alarm) continue;
        lv_obj_set_style_bg_color(s_t[i].tile, lv_color_hex(s_flash_phase ? C_RED : C_TILE_BG), 0);
        lv_obj_set_style_bg_opa(s_t[i].tile, LV_OPA_COVER, 0);
    }
}

// ---- shift dots ----------------------------------------------------------------
static uint32_t dot_color(int i)
{
    static const uint32_t LEVEL[DOT_LEVELS] = {
        C_GREEN, C_GREEN, C_GREEN, C_DOT_Y, C_DOT_Y, C_DOT_O, C_DOT_R, C_DOT_R,
    };
    int level = i < DOT_N - 1 - i ? i : DOT_N - 1 - i;   // 0 at both ends
    return LEVEL[level];
}

static void dot_set(int i, bool on, uint32_t color)
{
    lv_obj_set_style_bg_color(s_dot[i], lv_color_hex(on ? color : C_DOT_OFF), 0);
    lv_obj_set_style_border_width(s_dot[i], on ? 0 : 2, 0);
}

static void paint_dots(int lit)
{
    for (int i = 0; i < DOT_N; i++) {
        int level = i < DOT_N - 1 - i ? i : DOT_N - 1 - i;
        dot_set(i, level < lit, dot_color(i));
    }
}

static void shift_cb(lv_timer_t *t)
{
    if (!s_shift) return;
    s_shift_on = !s_shift_on;
    for (int i = 0; i < DOT_N; i++) dot_set(i, s_shift_on, C_SHIFT);
}

static void update_dots(int rpm)
{
    bool shift = rpm >= DASH7_SHIFT_RPM;
    if (shift != s_shift) {
        s_shift = shift;
        s_shift_on = shift;
        if (shift) {                          // all blue at once; shift_cb blinks
            for (int i = 0; i < DOT_N; i++) dot_set(i, true, C_SHIFT);
            return;
        }
        s_lit = -1;                           // repaint the bands below
    }
    if (shift) return;
    int lit = rpm <= DASH7_DOT_START_RPM ? 0
            : (rpm - DASH7_DOT_START_RPM) * DOT_LEVELS / (DASH7_SHIFT_RPM - DASH7_DOT_START_RPM);
    if (lit > DOT_LEVELS) lit = DOT_LEVELS;
    if (lit != s_lit) {
        s_lit = lit;
        paint_dots(lit);
    }
}

// ---- screens ---------------------------------------------------------------
static void takeover_ack_cb(lv_event_t *e)
{
    if (s_to_k >= 0) s_t[CRIT[s_to_k].slot].acked = true;
}

// On the top layer, so it covers every page and survives page changes.
static void build_takeover(void)
{
    s_to = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_to);
    lv_obj_set_size(s_to, 800, 480);
    lv_obj_set_style_bg_opa(s_to, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_to, lv_color_hex(C_RED), 0);
    lv_obj_add_flag(s_to, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_to, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_to, takeover_ack_cb, LV_EVENT_CLICKED, NULL);

    s_to_title = label(s_to, "", &lv_font_montserrat_44, C_WHITE);
    lv_obj_align(s_to_title, LV_ALIGN_TOP_MID, 0, 70);
    s_to_val = label(s_to, "", &ui_font_rpm_96, C_WHITE);
    lv_obj_align(s_to_val, LV_ALIGN_CENTER, -30, 10);
    s_to_unit = label(s_to, "", &lv_font_montserrat_44, C_WHITE);
    lv_obj_align_to(s_to_unit, s_to_val, LV_ALIGN_OUT_RIGHT_BOTTOM, 16, -10);
    lv_obj_t *h = label(s_to, "TAP TO ACKNOWLEDGE", &lv_font_montserrat_20, C_WHITE);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -30);

    lv_obj_add_flag(s_to, LV_OBJ_FLAG_HIDDEN);
}

static void update_takeover(void)
{
    int64_t now = now_ms();
    int show = -1;
    for (int k = 0; k < N_CRIT; k++) {
        tile_t *t = &s_t[CRIT[k].slot];
        if (!t->alarm) { t->acked = false; continue; }
        if (show < 0 && !t->acked && now - t->since >= CRIT[k].delay_ms) show = k;
    }
    if (show != s_to_k) {
        s_to_k = show;
        if (show < 0) {
            lv_obj_add_flag(s_to, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        set_text_if(s_to_title, CRIT[show].title);
        set_text_if(s_to_unit,  CRIT[show].unit);
        lv_obj_clear_flag(s_to, LV_OBJ_FLAG_HIDDEN);
    }
    if (show >= 0) {
        set_num(s_to_val, s_t[CRIT[show].slot].last, "%.0f");
        lv_obj_align_to(s_to_unit, s_to_val, LV_ALIGN_OUT_RIGHT_BOTTOM, 16, -10);
    }
}

// A black pill over the dots, for NO CAN and DEMO.
static lv_obj_t *badge(const char *txt, uint32_t color)
{
    lv_obj_t *b = label(s_dash, txt, &lv_font_montserrat_28, color);
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_pad_ver(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, DOT_Y + DOT_D / 2 - 19);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    return b;
}

static void build_dash(void)
{
    s_dash = t7_screen();

    // Shift dots: 15 across the top, evenly spaced over 26..774.
    for (int i = 0; i < DOT_N; i++) {
        int x = 26 + (748 - DOT_D) * i / (DOT_N - 1);
        s_dot[i] = box(s_dash, x, DOT_Y, DOT_D, DOT_D, C_DOT_OFF);
        lv_obj_set_style_radius(s_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(s_dot[i], lv_color_hex(C_TILE_LINE), 0);
        lv_obj_set_style_border_width(s_dot[i], 2, 0);
    }
    s_no_can = badge("NO CAN", C_RED);
    // Demo values must never pass for real ones.
    s_demo = badge("DEMO", C_WARM);

    // Left: gear, its caption, P R N D M.
    s_gear = label(s_dash, "--", &ui_font_gear_104, C_WHITE);
    lv_obj_align(s_gear, LV_ALIGN_TOP_MID, GEAR_CX - 400, 126);
    lv_obj_t *gc = caption(s_dash, "GEAR");
    lv_obj_align(gc, LV_ALIGN_TOP_MID, GEAR_CX - 400, 214);
    int sx = GEAR_CX - (5 * SEL_BOX + 4 * SEL_GAP) / 2;
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = box(s_dash, sx + i * (SEL_BOX + SEL_GAP), SEL_Y, SEL_BOX, SEL_BOX, C_TILE_BG);
        lv_obj_set_style_radius(b, 5, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(C_TILE_LINE), 0);
        lv_obj_set_style_border_width(b, 2, 0);
        char t[2] = { SEL_LETTERS[i], 0 };
        lv_obj_t *l = label(b, t, &lv_font_montserrat_14, C_MUTED);
        lv_obj_center(l);
        s_sel_box[i] = b; s_sel_lbl[i] = l;
    }

    // Centre: speed, its caption, the odometer.
    s_mph = label(s_dash, "--", &ui_font_mph_150, C_WHITE);
    lv_obj_align(s_mph, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_t *mc = caption(s_dash, "MPH");
    lv_obj_align(mc, LV_ALIGN_TOP_MID, 0, 236);
    lv_obj_t *odo = lv_obj_create(s_dash);
    lv_obj_remove_style_all(odo);
    lv_obj_set_size(odo, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(odo, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(odo, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(odo, 6, 0);
    lv_obj_clear_flag(odo, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    label(odo, "ODO", &lv_font_montserrat_16, C_MUTED);
    s_odo_val = label(odo, "--", &lv_font_montserrat_16, C_ODO);
    label(odo, "mi", &lv_font_montserrat_16, C_ODO);
    lv_obj_align(odo, LV_ALIGN_TOP_MID, 0, 262);

    // Right: fuel with its bar, ethanol under it.
    lv_obj_t *fc = caption(s_dash, "FUEL");
    lv_obj_set_pos(fc, RIGHT_X, 172);
    lv_obj_t *fr = value_row(s_dash, &lv_font_montserrat_24, &s_fuel_val, "%", NULL, 2);
    lv_obj_align(fr, LV_ALIGN_TOP_RIGHT, -(800 - RIGHT_X - RIGHT_W), 160);
    s_fuel_bar = fastbar_create(s_dash, RIGHT_X, 194, RIGHT_W, 10, 100, C_TRACK, C_GREEN, 4);
    lv_obj_t *ec = caption(s_dash, "ETH");
    lv_obj_set_pos(ec, RIGHT_X, 234);
    lv_obj_t *er = value_row(s_dash, &lv_font_montserrat_24, &s_eth_val, "%", NULL, 2);
    lv_obj_align(er, LV_ALIGN_TOP_RIGHT, -(800 - RIGHT_X - RIGHT_W), 222);

    // Bottom: the six tiles under a hairline.
    box(s_dash, 20, TILE_ROW_Y - 1, 760, 1, C_TILE_LINE);
    for (int i = 0; i < T_COUNT; i++) make_tile(i);

    paint_dots(0);
    lv_timer_create(flash_cb, WARN_FLASH_MS, NULL);
    lv_timer_create(shift_cb, SHIFT_BLINK_MS, NULL);
}

static void boot_done_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    // No fade: a fade redraws all 384k pixels on every step.
    lv_scr_load(s_dash);
    lv_obj_del(s_boot);          // the logo is 500 KB of draw state; free it
    s_boot = NULL;
}

lv_obj_t *dash7_screen(void) { return s_dash; }

void dash7_create(uint32_t boot_ms)
{
    build_dash();
    build_takeover();

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

// 79712.4 -> "79,712.4"
static void fmt_miles(char *out, size_t n, double miles)
{
    char raw[24];
    snprintf(raw, sizeof raw, "%.1f", miles);
    char *dot = strchr(raw, '.');
    int whole = dot ? (int)(dot - raw) : (int)strlen(raw);
    size_t o = 0;
    for (int i = 0; i < whole && o + 2 < n; i++) {
        out[o++] = raw[i];
        int left = whole - 1 - i;
        if (left > 0 && left % 3 == 0) out[o++] = ',';
    }
    snprintf(out + o, n - o, "%s", dot ? dot : "");
}

static void set_tile(int i, float v, const char *fmt, int st)
{
    set_num(s_t[i].val, v, fmt);
    s_t[i].last = v;
    set_state(i, st);
}

void dash7_update(const dash7_values_t *v)
{
    if (v->demo != s_demo_shown) {
        s_demo_shown = v->demo;
        if (v->demo) lv_obj_clear_flag(s_demo, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(s_demo, LV_OBJ_FLAG_HIDDEN);
    }

    if (v->no_can != s_no_can_shown) {
        s_no_can_shown = v->no_can;
        if (v->no_can) lv_obj_clear_flag(s_no_can, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(s_no_can, LV_OBJ_FLAG_HIDDEN);
    }

    int rpm = isnan(v->rpm) ? 0 : (int)v->rpm;
    update_dots(rpm);

    char b[16];
    if (isnan(v->mph)) set_text_if(s_mph, "--");
    else { snprintf(b, sizeof b, "%d", (int)(v->mph + 0.5f)); set_text_if(s_mph, b); }

    set_selector(v->prndl);
    if (v->gear == -1)                  set_text_if(s_gear, "R");
    else if (v->gear >= 1 && v->gear <= 8) { snprintf(b, sizeof b, "%d", v->gear); set_text_if(s_gear, b); }
    else                                set_text_if(s_gear, "--");

    // Tiles: colour band, then the same alarm rules as the round gauges.
    set_tile(T_WATER, v->water_f, "%.0f", band(v->water_f, COL_WATER_COLD, COL_WATER_WARM));
    set_alarm(T_WATER, !isnan(v->water_f) &&
        (s_t[T_WATER].alarm ? v->water_f > WARN_WATER_CLEAR : v->water_f > WARN_WATER_MAX));

    set_tile(T_OIL_TEMP, v->oil_temp_f, "%.0f", band(v->oil_temp_f, COL_OIL_TEMP_COLD, COL_OIL_TEMP_WARM));
    set_alarm(T_OIL_TEMP, !isnan(v->oil_temp_f) &&
        (s_t[T_OIL_TEMP].alarm ? v->oil_temp_f > WARN_OIL_TEMP_CLEAR
                               : v->oil_temp_f > WARN_OIL_TEMP_MAX));

    set_tile(T_IAT, v->iat_f, "%.0f", band(v->iat_f, NAN, COL_IAT_WARM));
    set_alarm(T_IAT, !isnan(v->iat_f) &&
        (s_t[T_IAT].alarm ? v->iat_f > WARN_IAT_CLEAR : v->iat_f > WARN_IAT_MAX));

    // A stopped engine has no oil pressure; only colour or alarm with it
    // turning, past the start-up lag, and alarm once the low reading has
    // persisted.
    {
        static int64_t turning_since, low_since;
        int64_t now = now_ms();
        bool turning = rpm >= WARN_OIL_PSI_MIN_RPM;
        if (!turning)            turning_since = 0;
        else if (!turning_since) turning_since = now;
        bool running = turning && now - turning_since >= WARN_OIL_PSI_START_MS;

        int st = (!running || isnan(v->oil_psi)) ? ST_NONE
               : v->oil_psi < COL_OIL_PSI_OK ? ST_WARM : ST_OK;
        set_tile(T_OIL_PSI, v->oil_psi, "%.0f", st);

        bool alarmed = s_t[T_OIL_PSI].alarm;
        bool low = !isnan(v->oil_psi) && running &&
                   v->oil_psi < (alarmed ? WARN_OIL_PSI_CLEAR : WARN_OIL_PSI_MIN);
        if (!low)            low_since = 0;
        else if (!low_since) low_since = now;

        set_alarm(T_OIL_PSI, low && (alarmed || now - low_since >= WARN_OIL_PSI_SUSTAIN_MS));
    }

    set_tile(T_TRANS, v->trans_f, "%.0f", band(v->trans_f, COL_TRANS_COLD, COL_TRANS_WARM));
    set_alarm(T_TRANS, !isnan(v->trans_f) &&
        (s_t[T_TRANS].alarm ? v->trans_f > WARN_TRANS_CLEAR : v->trans_f > WARN_TRANS_MAX));

    // Signed: vacuum shows negative (about -9 psi at idle), boost with a +.
    set_tile(T_BOOST, v->boost_psi, v->boost_psi > 0.05f ? "+%.1f" : "%.1f",
             band(v->boost_psi, NAN, COL_BOOST_WARM));

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
    set_num(s_fuel_val, v->fuel_pct, "%.0f");
    // One decimal, as HP Tuners shows it: the ECM reports in ~0.4% steps.
    set_num(s_eth_val, v->ethanol_pct, "%.1f");

    // Tenths change every ~6s at 60 mph, so this almost never repaints.
    char ob[24];
    if (isnan(v->odo_miles)) snprintf(ob, sizeof ob, "--");
    else                     fmt_miles(ob, sizeof ob, v->odo_miles);
    set_text_if(s_odo_val, ob);

    update_takeover();
}
