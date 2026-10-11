#include "nav7.h"
#include "theme7.h"

#define MAX_PAGES 4
// Short: the slide redraws the full 800x480 every frame, which is the one
// thing this panel is slow at, so keep it brief.
#define SLIDE_MS  220

static lv_obj_t *s_pages[MAX_PAGES];
static int       s_n;

static int index_of(lv_obj_t *scr)
{
    for (int i = 0; i < s_n; i++)
        if (s_pages[i] == scr) return i;
    return -1;
}

static void gesture_cb(lv_event_t *e)
{
    int i = index_of(lv_event_get_current_target(e));
    if (i < 0) return;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (d == LV_DIR_LEFT && i + 1 < s_n)
        lv_scr_load_anim(s_pages[i + 1], LV_SCR_LOAD_ANIM_MOVE_LEFT, SLIDE_MS, 0, false);
    else if (d == LV_DIR_RIGHT && i > 0)
        lv_scr_load_anim(s_pages[i - 1], LV_SCR_LOAD_ANIM_MOVE_RIGHT, SLIDE_MS, 0, false);
}

void nav7_add(lv_obj_t *scr)
{
    if (s_n >= MAX_PAGES) return;
    s_pages[s_n++] = scr;
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);
}

void nav7_dots(lv_obj_t *scr, int index)
{
    const int n = 3, d = 10, gap = 12;
    int x0 = 400 - (n * d + (n - 1) * gap) / 2;
    for (int i = 0; i < n; i++) {
        lv_obj_t *o = lv_obj_create(scr);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, d, d);
        lv_obj_set_pos(o, x0 + i * (d + gap), 462);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(i == index ? C_WHITE : C_TILE_LINE), 0);
    }
}
