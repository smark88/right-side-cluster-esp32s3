// GM mode 22 PID sweep. See pid_scan.h.

#include "pid_scan.h"
#include "canbus.h"
#include "obd_poll.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "PIDSCAN";

// How long to wait for the ECM. It answers a known PID or rejects an unknown
// one (7F 22 31) within a few ms; silence past this means skip it.
#define REPLY_TIMEOUT_MS   40
// 7F 22 78 is "busy, answer coming": allow this much longer.
#define PENDING_TIMEOUT_MS 300

// Raw first-byte windows worth flagging on screen. HP Tuners on the car:
//   ethanol 9.8 %  -> A * 100/255 = 9.8 -> A = 25
//   oil temp 180 F -> 82 C, A - 40     -> A = 122 (drifts as it warms)
// The calc oil temp (0x1154, 190 F) is A = 128, outside the window.
typedef struct { const char *name; uint8_t lo, hi; } target_t;
static const target_t TARGETS[] = {
    { "ETHANOL 9.8%",   24,  26 },
    { "OIL TEMP 180F", 118, 126 },
};
#define N_TARGETS (sizeof(TARGETS) / sizeof(TARGETS[0]))

#define MAX_SHOWN 48
typedef struct { uint16_t pid; uint8_t a, b; } hit_t;

// Written by the scan task, read by the LVGL timer. Counters only ever grow,
// so a stale read just shows the previous frame's numbers.
static volatile uint16_t s_cur = PID_SCAN_FIRST;
static volatile int      s_answers, s_eth52 = -1;   // -1 unknown, 0 no, 1 yes
static volatile bool     s_done;
static hit_t             s_hits[N_TARGETS][MAX_SHOWN];
static volatile int      s_nhits[N_TARGETS];

static lv_obj_t *s_label;

static void send(uint32_t id, const uint8_t *d)
{
    twai_message_t m = { .identifier = id, .data_length_code = 8 };
    memcpy(m.data, d, 8);
    twai_transmit(&m, pdMS_TO_TICKS(20));
}

// Next frame from the ECM, or false on timeout. Everything else on the bus
// (broadcast traffic) is dropped.
static bool recv_ecm(twai_message_t *m, int64_t deadline_us)
{
    for (;;) {
        int64_t left_ms = (deadline_us - esp_timer_get_time()) / 1000;
        if (left_ms <= 0) return false;
        if (twai_receive(m, pdMS_TO_TICKS(left_ms)) != ESP_OK) return false;
        if (!m->extd && !m->rtr && m->identifier == OBD_ECU_ID) return true;
    }
}

// Mode 01 PID 0x40: which of 0x41..0x60 the ECM supports. Answers whether the
// standard ethanol PID 0x52 is there at all.
static void check_mode01_52(void)
{
    const uint8_t req[8] = { 0x02, 0x01, 0x40, 0, 0, 0, 0, 0 };
    send(OBD_ECM_REQ, req);
    twai_message_t m;
    int64_t dl = esp_timer_get_time() + 200 * 1000;
    while (recv_ecm(&m, dl)) {
        if (m.data[1] == 0x41 && m.data[2] == 0x40) {
            // 0x52 is the 18th PID after 0x40: byte B, bit 6.
            s_eth52 = (m.data[4] >> 6) & 1;
            ESP_LOGI(TAG, "mode 01 0x40 bitmap %02X %02X %02X %02X -> PID 52 %s",
                     m.data[3], m.data[4], m.data[5], m.data[6],
                     s_eth52 ? "SUPPORTED" : "not supported");
            return;
        }
    }
    s_eth52 = 0;
    ESP_LOGW(TAG, "mode 01 0x40: no answer");
}

static void record(uint16_t pid, const uint8_t *val, int n)
{
    s_answers++;
    char hex[3 * 8 + 1] = "";
    for (int i = 0; i < n && i < 8; i++)
        sprintf(hex + 3 * i, "%02X ", val[i]);
    ESP_LOGI(TAG, "HIT 22 %04X  len %d  %s", pid, n, hex);

    if (n < 1) return;
    for (int t = 0; t < N_TARGETS; t++) {
        if (val[0] < TARGETS[t].lo || val[0] > TARGETS[t].hi) continue;
        ESP_LOGI(TAG, "   ^ candidate for %s", TARGETS[t].name);
        int k = s_nhits[t];
        if (k < MAX_SHOWN) {
            s_hits[t][k] = (hit_t){ pid, val[0], n > 1 ? val[1] : 0 };
            s_nhits[t] = k + 1;
        }
    }
}

static void scan_one(uint16_t pid)
{
    const uint8_t req[8] = { 0x03, 0x22, pid >> 8, pid & 0xFF, 0, 0, 0, 0 };
    send(OBD_ECM_REQ, req);

    twai_message_t m;
    int64_t dl = esp_timer_get_time() + REPLY_TIMEOUT_MS * 1000;
    while (recv_ecm(&m, dl)) {
        const uint8_t *d = m.data;
        uint8_t pci = d[0] >> 4;

        if (pci == 0 && d[1] == 0x62 && d[2] == (pid >> 8) && d[3] == (pid & 0xFF)) {
            record(pid, d + 4, (d[0] & 0x0F) - 3);           // single frame
            return;
        }
        if (pci == 1 && d[2] == 0x62 && d[3] == (pid >> 8) && d[4] == (pid & 0xFF)) {
            // Long answer. Only the first frame is read (no flow control
            // sent, so the ECM gives up on the rest), which is enough to spot
            // a candidate.
            int total = ((d[0] & 0x0F) << 8 | d[1]) - 3;
            ESP_LOGI(TAG, "HIT 22 %04X  multi-frame, %d bytes total", pid, total);
            record(pid, d + 5, 3);
            return;
        }
        if (pci == 0 && d[1] == 0x7F && d[2] == 0x22) {
            if (d[3] == 0x78) {                               // pending
                dl = esp_timer_get_time() + PENDING_TIMEOUT_MS * 1000;
                continue;
            }
            return;                                           // rejected
        }
    }
}

static void scan_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));          // let the bus settle after start
    check_mode01_52();

    int64_t t0 = esp_timer_get_time();
    for (uint32_t pid = PID_SCAN_FIRST; pid <= PID_SCAN_LAST; pid++) {
        s_cur = pid;
        scan_one(pid);
    }
    s_done = true;

    ESP_LOGI(TAG, "DONE in %llds: %d PIDs answered",
             (esp_timer_get_time() - t0) / 1000000, s_answers);
    for (int t = 0; t < N_TARGETS; t++) {
        ESP_LOGI(TAG, "%s candidates: %d", TARGETS[t].name, s_nhits[t]);
        for (int k = 0; k < s_nhits[t]; k++)
            ESP_LOGI(TAG, "   22 %04X  A=%u B=%u", s_hits[t][k].pid,
                     s_hits[t][k].a, s_hits[t][k].b);
    }
    vTaskDelete(NULL);
}

static void ui_cb(lv_timer_t *t)
{
    static char buf[3072];
    int span = PID_SCAN_LAST - PID_SCAN_FIRST + 1;
    int pct  = (s_cur - PID_SCAN_FIRST + 1) * 100 / span;
    int n = snprintf(buf, sizeof buf,
        "PID SCAN   %s   22 %04X  %d%%   answered %d\n"
        "mode 01 PID 52 (std ethanol): %s\n",
        s_done ? "DONE" : "running", s_cur, pct, s_answers,
        s_eth52 < 0 ? "checking" : s_eth52 ? "supported" : "NOT supported");

    for (int tg = 0; tg < N_TARGETS && n < (int)sizeof buf; tg++) {
        n += snprintf(buf + n, sizeof buf - n, "\n%s  (raw %u-%u): %d\n",
                      TARGETS[tg].name, TARGETS[tg].lo, TARGETS[tg].hi, s_nhits[tg]);
        for (int k = 0; k < s_nhits[tg] && n < (int)sizeof buf; k++)
            n += snprintf(buf + n, sizeof buf - n, "%04X=%u  ",
                          s_hits[tg][k].pid, s_hits[tg][k].a);
        if (n < (int)sizeof buf) n += snprintf(buf + n, sizeof buf - n, "\n");
    }
    if (strcmp(lv_label_get_text(s_label), buf) != 0)
        lv_label_set_text(s_label, buf);
}

void pid_scan_start(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    s_label = lv_label_create(scr);
    lv_label_set_text(s_label, "");
    lv_obj_set_width(s_label, 780);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
    lv_obj_set_pos(s_label, 10, 10);
    lv_scr_load(scr);
    lv_timer_create(ui_cb, 250, NULL);

    xTaskCreatePinnedToCore(scan_task, "pid_scan", 4096, NULL, 10, NULL, 0);
}
