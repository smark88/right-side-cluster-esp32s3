// Swipe navigation between the 7in screens: left for the next page, right
// for the previous one. Pages are in the order they are added.
#pragma once

#include "lvgl.h"

// Gauges, min/max, trouble codes, settings.
#define NAV7_PAGES 4

void nav7_add(lv_obj_t *scr);

// Page dots along the bottom edge, for screens with room for them.
void nav7_dots(lv_obj_t *scr, int index);
