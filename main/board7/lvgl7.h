// LVGL v8 on the 7in RGB panel, tuned for frame rate rather than memory.
#pragma once

#include "lvgl.h"
#include "esp_lcd_panel_ops.h"

// Two steps because LVGL is not thread safe. lvgl7_init sets LVGL up without
// running it, so the UI can be built from app_main with nothing racing it.
// lvgl7_start then hands LVGL to its own task; after that, touch LVGL objects
// only from lv_timer callbacks, which run inside that task.
void lvgl7_init(esp_lcd_panel_handle_t panel);
void lvgl7_start(void);
