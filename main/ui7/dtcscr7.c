#include "dtcscr7.h"
#include "theme7.h"
#include "nav7.h"
#include "dtc7.h"

static lv_obj_t *s_scr, *s_status, *s_list, *s_confirm;
static uint32_t  s_shown_version = UINT32_MAX;

static void read_cb(lv_event_t *e)  { dtc_request_read(); }

static void confirm_show(lv_event_t *e) { lv_obj_clear_flag(s_confirm, LV_OBJ_FLAG_HIDDEN); }
static void confirm_no(lv_event_t *e)   { lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_HIDDEN); }
static void confirm_yes(lv_event_t *e)
{
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
    dtc_request_clear();
}

static void refresh_cb(lv_timer_t *t)
{
    if (lv_scr_act() != s_scr) return;
    static dtc_result_t r;
    dtc_get(&r);
    if (r.version == s_shown_version) return;
    s_shown_version = r.version;

    t7_set_text_if(s_status, r.version ? r.msg : "Key on. Tap READ CODES.");

    static char buf[DTC_MAX * 40 + 1];
    int n = 0;
    buf[0] = 0;
    char nb[8];
    for (int i = 0; i < r.count && n < (int)sizeof buf; i++)
        n += snprintf(buf + n, sizeof buf - n, "%s   %s%s\n", r.codes[i].code,
                      dtc_module_name(r.codes[i].module, nb, sizeof nb),
                      r.codes[i].pending ? "   pending" : "");
    t7_set_text_if(s_list, buf);
}

static lv_obj_t *build_confirm(lv_obj_t *parent)
{
    // Covers the screen, so nothing behind it can be tapped meanwhile.
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 800, 480);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_80, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(o);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 560, 280);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, 14, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(C_TILE_BG), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(C_RED), 0);
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = t7_label(box, "Clear all trouble codes?", &lv_font_montserrat_28, C_WHITE);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_t *m = t7_label(box,
        "Turns off the check engine light and resets\n"
        "the emissions readiness monitors.\n"
        "Key on, engine off.",
        &lv_font_montserrat_20, C_MUTED);
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 72);

    lv_obj_t *no  = t7_button(box, "CANCEL", 40,  190, 220, 64, C_TILE_LINE);
    lv_obj_t *yes = t7_button(box, "CLEAR",  300, 190, 220, 64, C_RED);
    lv_obj_add_event_cb(no,  confirm_no,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(yes, confirm_yes, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

lv_obj_t *dtcscr7_create(int index)
{
    s_scr = t7_screen();

    lv_obj_t *t = t7_label(s_scr, "TROUBLE CODES", &lv_font_montserrat_28, C_WHITE);
    lv_obj_set_pos(t, 20, 14);

    // Two separate buttons, well apart, so a read can't become a clear.
    lv_obj_t *rd = t7_button(s_scr, "READ CODES",  20,  60, 300, 70, C_MUTED);
    lv_obj_t *cl = t7_button(s_scr, "CLEAR CODES", 480, 60, 300, 70, C_RED);
    lv_obj_add_event_cb(rd, read_cb,      LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(cl, confirm_show, LV_EVENT_CLICKED, NULL);

    s_status = t7_label(s_scr, "", &lv_font_montserrat_20, C_MUTED);
    lv_obj_set_width(s_status, 760);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_status, 20, 146);

    // The list scrolls vertically only, so a sideways swipe on it still
    // changes page.
    lv_obj_t *box = lv_obj_create(s_scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 20, 200);
    lv_obj_set_size(box, 760, 250);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    s_list = t7_label(box, "", &lv_font_montserrat_28, C_WHITE);
    lv_obj_set_width(s_list, 740);

    nav7_dots(s_scr, index);
    s_confirm = build_confirm(s_scr);
    lv_timer_create(refresh_cb, 200, NULL);
    return s_scr;
}
