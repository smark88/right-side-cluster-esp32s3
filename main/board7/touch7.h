// GT911 capacitive touch on the 7in board, as an LVGL pointer.
//
// It shares the CH422G's I2C bus (new i2c_master API -- the round firmware's
// GT911 driver uses the legacy API, which cannot coexist with it), and its
// reset line is CH422G output EXIO1.
#pragma once

#include "esp_err.h"

// Call after ch422g_init and lvgl7_init, before lvgl7_start. A missing or
// silent controller is logged and skipped; the dash runs without touch.
esp_err_t touch7_init(void);
