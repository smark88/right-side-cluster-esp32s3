#include "lcd7.h"

#include "esp_log.h"
#include "esp_lcd_panel_rgb.h"

static const char *TAG = "LCD7";

// Bounce buffers: the panel DMA reads from two small buffers in internal RAM,
// refilled from the PSRAM framebuffer by the CPU. Without them the DMA reads
// PSRAM directly and any competing PSRAM traffic (LVGL drawing, the flush
// copy) starves it, which shows as the whole image drifting sideways. 20 lines
// is what Waveshare's demo and the reference projects settled on.
#define BOUNCE_LINES 20

esp_lcd_panel_handle_t lcd7_init(void)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = LCD7_H_RES * BOUNCE_LINES,
        .psram_trans_align = 64,
        .hsync_gpio_num = 46,
        .vsync_gpio_num = 3,
        .de_gpio_num = 5,
        .pclk_gpio_num = 7,
        .disp_gpio_num = -1,          // backlight is on the CH422G, not a GPIO
        // D0..D15 = B3..B7, G2..G7, R3..R7
        .data_gpio_nums = { 14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40 },
        .timings = {
            .pclk_hz = LCD7_PCLK_HZ,
            .h_res = LCD7_H_RES,
            .v_res = LCD7_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = 1,
        },
        .flags.fb_in_psram = 1,
    };

    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    ESP_LOGI(TAG, "800x480 RGB565 at %d MHz -> ~%d Hz panel refresh",
             LCD7_PCLK_HZ / 1000000, LCD7_PCLK_HZ / 410000);
    return panel;
}
