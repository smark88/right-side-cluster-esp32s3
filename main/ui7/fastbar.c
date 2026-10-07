#include "fastbar.h"

typedef struct {
    int      value, max, radius;
    lv_color_t track, fill;
} fastbar_t;

// x of the fill edge, in absolute screen coordinates.
static lv_coord_t edge_x(lv_obj_t *o, const fastbar_t *b, int value)
{
    lv_area_t a; lv_obj_get_coords(o, &a);
    int w = lv_area_get_width(&a);
    if (value < 0)      value = 0;
    if (value > b->max) value = b->max;
    return a.x1 + (lv_coord_t)((int64_t)w * value / b->max);
}

static void draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    fastbar_t *b = lv_obj_get_user_data(o);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);

    lv_area_t a; lv_obj_get_coords(o, &a);

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = b->radius;

    d.bg_color = b->track;
    lv_draw_rect(ctx, &d, &a);

    lv_coord_t x = edge_x(o, b, b->value);
    if (x > a.x1) {
        lv_area_t f = a;
        f.x2 = x - 1;
        d.bg_color = b->fill;
        lv_draw_rect(ctx, &d, &f);
    }
}

static void free_cb(lv_event_t *e)
{
    lv_mem_free(lv_obj_get_user_data(lv_event_get_target(e)));
}

lv_obj_t *fastbar_create(lv_obj_t *parent, int x, int y, int w, int h,
                         int max, uint32_t track, uint32_t fill, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    fastbar_t *b = lv_mem_alloc(sizeof *b);
    *b = (fastbar_t){ .value = 0, .max = max > 0 ? max : 1, .radius = radius,
                      .track = lv_color_hex(track), .fill = lv_color_hex(fill) };
    lv_obj_set_user_data(o, b);
    lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(o, free_cb, LV_EVENT_DELETE, NULL);
    return o;
}

void fastbar_set_value(lv_obj_t *o, int value)
{
    fastbar_t *b = lv_obj_get_user_data(o);
    if (value == b->value) return;

    lv_coord_t x_old = edge_x(o, b, b->value);
    lv_coord_t x_new = edge_x(o, b, value);
    b->value = value;
    if (x_old == x_new) return;          // moved less than a pixel

    lv_area_t a; lv_obj_get_coords(o, &a);
    lv_area_t d = a;
    // Widen by the corner radius: the rounded end of the fill reaches that far
    // back from the edge, and would otherwise leave a stale corner behind.
    d.x1 = LV_MIN(x_old, x_new) - b->radius - 1;
    d.x2 = LV_MAX(x_old, x_new) + b->radius + 1;
    if (d.x1 < a.x1) d.x1 = a.x1;
    if (d.x2 > a.x2) d.x2 = a.x2;
    lv_obj_invalidate_area(o, &d);
}

void fastbar_set_fill(lv_obj_t *o, uint32_t fill)
{
    fastbar_t *b = lv_obj_get_user_data(o);
    b->fill = lv_color_hex(fill);
    lv_obj_invalidate(o);
}
