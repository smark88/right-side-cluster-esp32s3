// 800x480 RGB565 parallel panel on the Waveshare ESP32-S3-Touch-LCD-7.
#pragma once

#include "esp_lcd_panel_ops.h"

#define LCD7_H_RES  800
#define LCD7_V_RES  480

// Pixel clock. Frame rate the panel can physically show is
//     PCLK / ((800 + 4 + 8 + 8) * (480 + 4 + 8 + 8)) = PCLK / 410000
// so 21 MHz is ~51 Hz and 16 MHz ~39 Hz. Waveshare's own benchmark runs 21.
// Above ~21 MHz the PSRAM can no longer feed the panel and lines drift; see
// https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/lcd.html
#define LCD7_PCLK_HZ  (21 * 1000 * 1000)

esp_lcd_panel_handle_t lcd7_init(void);
