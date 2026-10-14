#include "lvgl7.h"

#include "lcd7.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LVGL7";

// Draw buffers live in INTERNAL RAM, two of them. LVGL renders into one while
// the other is being copied out to the PSRAM framebuffer, and rendering into
// internal RAM is several times faster than rendering into PSRAM, which on the
// S3 is the single scarcest resource -- the panel is already streaming from it
// continuously. 40 lines x 800 px x 2 B = 64 KB each.
#define DRAW_LINES  40

// LVGL is given the whole of core 1. CAN, OBD and everything else run on
// core 0, so a burst of bus traffic never stalls a frame.
#define LVGL_CORE        1
#define LVGL_TASK_PRIO   5
#define LVGL_STACK       (8 * 1024)

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;

// Frames LVGL actually rendered, for the FPS readout.
static volatile uint32_t s_frames;
static lv_obj_t         *s_perf;
static bool              s_perf_on;

static void monitor_cb(lv_disp_drv_t *drv, uint32_t time_ms, uint32_t px)
{
    s_frames++;
}

static void perf_cb(lv_timer_t *t)
{
    static uint32_t last;
    uint32_t f = s_frames, fps = f - last;
    last = f;
    if (!s_perf_on) return;
    lv_label_set_text_fmt(s_perf, "%u FPS   %u%% CPU", (unsigned)fps,
                          (unsigned)(100 - lv_timer_get_idle()));
}

void lvgl7_perf_show(bool on)
{
    if (on == s_perf_on || !s_perf) return;
    s_perf_on = on;
    if (on) lv_obj_clear_flag(s_perf, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(s_perf, LV_OBJ_FLAG_HIDDEN);
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px)
{
    esp_lcd_panel_handle_t panel = drv->user_data;
    // For an RGB panel this is a memcpy into the framebuffer; the DMA picks it
    // up on its next pass.
    esp_lcd_panel_draw_bitmap(panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
    lv_disp_flush_ready(drv);
}

static void tick_cb(void *arg)
{
    lv_tick_inc(1);
}

static void lvgl_task(void *arg)
{
    while (1) {
        uint32_t next = lv_timer_handler();
        // At least one tick, so a busy screen still yields to idle tasks.
        if (next < 1)  next = 1;
        if (next > 20) next = 20;
        vTaskDelay(pdMS_TO_TICKS(next));
    }
}

void lvgl7_init(esp_lcd_panel_handle_t panel)
{
    lv_init();

    size_t bytes = LCD7_H_RES * DRAW_LINES * sizeof(lv_color_t);
    void *buf1 = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    void *buf2 = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf1 || !buf2) {
        // Fall back to PSRAM rather than fail outright -- slower, but it runs.
        ESP_LOGW(TAG, "internal RAM short, draw buffers falling back to PSRAM");
        if (!buf1) buf1 = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        if (!buf2) buf2 = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    }
    lv_disp_draw_buf_init(&s_draw_buf, buf1, buf2, LCD7_H_RES * DRAW_LINES);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res   = LCD7_H_RES;
    s_disp_drv.ver_res   = LCD7_V_RES;
    s_disp_drv.flush_cb  = flush_cb;
    s_disp_drv.draw_buf  = &s_draw_buf;
    s_disp_drv.user_data = panel;
    s_disp_drv.monitor_cb = monitor_cb;
    lv_disp_drv_register(&s_disp_drv);

    // On the system layer so it sits over every page; 14 px tall at y 1,
    // clear of the shift dots that start at y 17.
    s_perf = lv_label_create(lv_layer_sys());
    lv_label_set_text(s_perf, "");
    lv_obj_set_style_text_font(s_perf, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_perf, lv_color_hex(0x8A9099), 0);
    lv_obj_align(s_perf, LV_ALIGN_TOP_RIGHT, -26, 1);
    lv_obj_add_flag(s_perf, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(perf_cb, 1000, NULL);

    const esp_timer_create_args_t t = { .callback = tick_cb, .name = "lv_tick" };
    esp_timer_handle_t h;
    ESP_ERROR_CHECK(esp_timer_create(&t, &h));
    ESP_ERROR_CHECK(esp_timer_start_periodic(h, 1000));

    ESP_LOGI(TAG, "draw buffers 2 x %u KB, internal free %u KB",
             (unsigned)(bytes / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

void lvgl7_start(void)
{
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", LVGL_STACK, NULL,
                            LVGL_TASK_PRIO, NULL, LVGL_CORE);
}
