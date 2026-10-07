// Combined 7in dash on the Waveshare ESP32-S3-Touch-LCD-7.
//
// Replaces both round P4 gauges: talks to the car itself over the onboard CAN
// transceiver (OBD mode 01 + GM mode 22, ported from gauge one), and shows
// everything on one 800x480 screen.

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ch422g.h"
#include "lcd7.h"
#include "lvgl7.h"
#include "dash7.h"
#include "dash_demo7.h"
#include "canbus.h"
#include "obd_poll.h"
#include "odometer.h"
#include "canbus7.h"

static const char *TAG = "MAIN7";

// Matches LVGL's refresh period so each frame sees fresh data, but only
// changed values cost anything -- dash7_update redraws nothing that held.
#define UPDATE_MS 16

#define BOOT_LOGO_MS 1500

// Below this the speed reading is noise, not motion (same as gauge one).
#define SPEED_MIN_VALID_MPH 3.0f

// No OBD reply for this long and the dash shows NO CAN. The poller asks
// ~50 times a second, so 2s of silence means wiring or the car, not luck.
#define NO_CAN_AFTER_US (2 * 1000 * 1000)

static float s_rpm_disp;

// Unused in demo mode, but kept compiling so it cannot rot unnoticed.
static void __attribute__((unused)) fill_from_car(dash7_values_t *v)
{
    // RPM and speed feed running state, where one NAN would latch forever (see
    // the tach fix on gauge one), so a missing value is 0 there. Tiles keep NAN
    // so they show "--".
    v->rpm         = isnan(can_data.rpm)   ? 0.0f : can_data.rpm;
    v->mph         = isnan(can_data.speed) ? 0.0f : can_data.speed;
    v->fuel_pct    = can_data.fuel_level;
    v->ethanol_pct = can_data.fuel_comp;
    v->water_f     = can_data.coolant_temp;
    v->oil_temp_f  = can_data.oil_temp;
    v->oil_psi     = can_data.oil_pressure;
    v->trans_f     = can_data.trans_temp;
    v->iat_f       = can_data.air_temp;
    v->boost_psi   = can_data.boost;

    // gear_sel arrives as 0 P, 1 N, 2 D, 3 R (obd_poll maps the TCM's codes).
    static const char prndl[] = { 'P', 'N', 'D', 'R' };
    float s = can_data.gear_sel;
    int si = isnan(s) ? -1 : (int)s;
    v->prndl = (si >= 0 && si < 4) ? prndl[si] : 0;

    float g = can_data.gear_num;
    v->gear = isnan(g) ? 0 : (int)g;
}

static void update_cb(lv_timer_t *t)
{
    dash7_values_t v;
#if DASH_DEMO_MODE
    dash_demo7_sample(&v);
#else
    fill_from_car(&v);
#endif
    // Odometer: distance = mph * elapsed hours. Demo speed is not real
    // driving, so in demo mode it only displays the stored mileage.
#if !DASH_DEMO_MODE
    static int64_t last_odo_us;
    int64_t now_us = esp_timer_get_time();
    if (last_odo_us != 0 && v.mph >= SPEED_MIN_VALID_MPH)
        odometer_add_miles(v.mph * (double)(now_us - last_odo_us) / 3600000000.0);
    last_odo_us = now_us;
#endif
    v.odo_miles = odometer_get_miles();

#if DASH_DEMO_MODE
    v.no_can = false;
#else
    v.no_can = esp_timer_get_time() - canbus_last_obd_us() > NO_CAN_AFTER_US;
#endif

    // Smooth the RPM so the bar sweeps rather than steps at OBD's ~8 Hz.
    s_rpm_disp += 0.35f * (v.rpm - s_rpm_disp);
    v.rpm = s_rpm_disp;
    dash7_update(&v);
}

#if !DASH_DEMO_MODE
// Writes to NVS once 100 m have built up since the last save.
static void odo_save_task(void *arg)
{
    for (;;) {
        odometer_periodic_save();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
#endif

void app_main(void)
{
    odometer_init();

    // CAN mode drives USB_SEL high, which disconnects native USB; in demo mode
    // leave it low so both USB-C ports still work on the bench.
    ESP_ERROR_CHECK(ch422g_init(!DASH_DEMO_MODE));

    esp_lcd_panel_handle_t panel = lcd7_init();
    lvgl7_init(panel);
    dash7_create(BOOT_LOGO_MS);
    lv_timer_create(update_cb, UPDATE_MS, NULL);
    lvgl7_start();

    // First frame (the logo) is drawn with the backlight off; then light it.
    vTaskDelay(pdMS_TO_TICKS(60));
    ch422g_set(CH422G_DISP, true);

#if DASH_DEMO_MODE
    dash_demo7_start();
    ESP_LOGI(TAG, "demo mode -- CAN not started");
#else
    canbus_init();
    xTaskCreatePinnedToCore(canbus_task, "can_rx", 4096, NULL, 10, NULL, 0);
    obd_poll_start();
    xTaskCreatePinnedToCore(odo_save_task, "odo_save", 4096, NULL, 4, NULL, 0);
#endif
}
