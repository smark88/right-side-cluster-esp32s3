// A horizontal bar that redraws only the slice that changed.
//
// lv_bar invalidates its whole area on every value change, so a 760x54 tach
// that moves 25 rpm repaints ~41k pixels per frame. On the S3, where LVGL
// draws everything in software, that was most of the frame budget. This bar
// invalidates only the strip between the old and new fill edge -- a few
// pixels wide for a typical step -- and LVGL clips the redraw to it.
#pragma once

#include "lvgl.h"

lv_obj_t *fastbar_create(lv_obj_t *parent, int x, int y, int w, int h,
                         int max, uint32_t track, uint32_t fill, int radius);
void fastbar_set_value(lv_obj_t *bar, int value);
// Recolours the fill. Repaints the whole bar, so call only on a real change.
void fastbar_set_fill(lv_obj_t *bar, uint32_t fill);
