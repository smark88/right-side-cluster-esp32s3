#include "settings7.h"
#include "theme7.h"
#include "nav7.h"
#include "dash_demo7.h"
#include "obd_poll.h"

#include "esp_log.h"

static const char *TAG = "SETTINGS";

#define C_AMBER 0xF2A900

// Not saved anywhere on purpose: a power cycle always comes back live.
static bool s_demo = DASH_DEMO_MODE;

static lv_obj_t *s_state, *s_btn, *s_btn_lbl;
static lv_obj_t *s_confirm, *s_confirm_title, *s_confirm_msg, *s_confirm_yes_lbl;

bool settings7_demo(void) { return s_demo; }

static void show_state(void)
{
    t7_set_text_if(s_state, s_demo ? "ON  -  simulated values" : "OFF  -  live data from the car");
    lv_obj_set_style_text_color(s_state, lv_color_hex(s_demo ? C_AMBER : C_GREEN), 0);
    if (s_btn) {
        t7_set_text_if(s_btn_lbl, s_demo ? "TURN DEMO OFF" : "TURN DEMO ON");
        lv_obj_set_style_border_color(s_btn, lv_color_hex(s_demo ? C_GREEN : C_AMBER), 0);
    }
}

static void set_demo(bool on)
{
    s_demo = on;
    if (on) dash_demo7_start();      // restart the simulated drive from idle
    obd_poll_hold(on);
    ESP_LOGI(TAG, "demo mode %s", on ? "ON" : "OFF, back to the car");
    show_state();
}

static void ask_cb(lv_event_t *e)
{
    bool turning_on = !s_demo;
    t7_set_text_if(s_confirm_title, turning_on ? "Turn demo mode on?" : "Turn demo mode off?");
    t7_set_text_if(s_confirm_msg, turning_on
        ? "The gauges will show simulated values,\nnot the car. Nothing is recorded.\nLive data returns at the next power-on."
        : "The gauges go back to live data\nfrom the car.");
    t7_set_text_if(s_confirm_yes_lbl, turning_on ? "DEMO ON" : "DEMO OFF");
    lv_obj_clear_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
}

static void no_cb(lv_event_t *e)  { lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_HIDDEN); }
static void yes_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
    set_demo(!s_demo);
}

static void build_confirm(lv_obj_t *parent)
{
    // Covers the page, so nothing behind it can be tapped meanwhile.
    s_confirm = lv_obj_create(parent);
    lv_obj_remove_style_all(s_confirm);
    lv_obj_set_size(s_confirm, 800, 480);
    lv_obj_set_style_bg_color(s_confirm, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_80, 0);
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_confirm, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(s_confirm);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 560, 290);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, 14, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(C_TILE_BG), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(C_AMBER), 0);
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    s_confirm_title = t7_label(box, "", &lv_font_montserrat_28, C_WHITE);
    lv_obj_align(s_confirm_title, LV_ALIGN_TOP_MID, 0, 24);
    s_confirm_msg = t7_label(box, "", &lv_font_montserrat_20, C_MUTED);
    lv_obj_set_style_text_align(s_confirm_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_confirm_msg, LV_ALIGN_TOP_MID, 0, 72);

    lv_obj_t *no  = t7_button(box, "CANCEL", 40,  200, 220, 64, C_TILE_LINE);
    lv_obj_t *yes = t7_button(box, "",       300, 200, 220, 64, C_AMBER);
    s_confirm_yes_lbl = lv_obj_get_child(yes, 0);
    lv_obj_add_event_cb(no,  no_cb,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(yes, yes_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *settings7_create(int index)
{
    lv_obj_t *scr = t7_screen();

    lv_obj_t *t = t7_label(scr, "SETTINGS", &lv_font_montserrat_28, C_WHITE);
    lv_obj_set_pos(t, 20, 14);

    lv_obj_t *cap = t7_label(scr, "DEMO MODE", &lv_font_montserrat_20, C_MUTED);
    lv_obj_set_pos(cap, 20, 90);
    s_state = t7_label(scr, "", &lv_font_montserrat_28, C_GREEN);
    lv_obj_set_pos(s_state, 20, 122);

#if DASH_DEMO_MODE
    // Bench build: CAN never starts, so there is nothing to switch back to.
    s_btn = NULL;
    lv_obj_t *n = t7_label(scr,
        "This is a bench (Demo run mode) build: CAN is off.\n"
        "For the car, set menuconfig -> Dash (7in) -> Run mode -> Car.",
        &lv_font_montserrat_20, C_MUTED);
    lv_obj_set_pos(n, 20, 180);
#else
    s_btn = t7_button(scr, "", 20, 180, 320, 70, C_AMBER);
    s_btn_lbl = lv_obj_get_child(s_btn, 0);
    lv_obj_add_event_cb(s_btn, ask_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *n = t7_label(scr,
        "Demo is never saved: the gauge always powers up on live data.",
        &lv_font_montserrat_20, C_MUTED);
    lv_obj_set_pos(n, 20, 274);
#endif

    show_state();
    nav7_dots(scr, index);
    build_confirm(scr);
    return scr;
}
