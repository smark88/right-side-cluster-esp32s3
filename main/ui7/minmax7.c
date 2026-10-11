#include "minmax7.h"
#include "theme7.h"
#include "nav7.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

static const char *TAG = "MINMAX";

#define MM_NS          "minmax"
#define MM_KEY_CUR     "cur"
#define MM_KEY_LAST    "last"
#define MM_MAGIC       0x4D4D3031u       // "MM01": bump if the layout changes
// A warm-up moves the maxima every few seconds; this keeps NVS writes to a
// couple a minute. Losing up to this much at key-off is fine.
#define MINMAX_SAVE_MS 30000

enum { S_WATER, S_OIL_TEMP, S_OIL_PSI, S_IAT1, S_IAT2, S_TRANS, S_BOOST, S_KNOCK, S_N };

static const struct { const char *name; const char *fmt; } SIG[S_N] = {
    [S_WATER]    = { "WATER",    "%.0f" },
    [S_OIL_TEMP] = { "OIL TEMP", "%.0f" },
    [S_OIL_PSI]  = { "OIL PSI",  "%.0f" },
    [S_IAT1]     = { "IAT 1",    "%.0f" },
    [S_IAT2]     = { "IAT 2",    "%.0f" },
    [S_TRANS]    = { "TRANS",    "%.0f" },
    [S_BOOST]    = { "BOOST",    "%.1f" },
    [S_KNOCK]    = { "KNOCK",    "%.1f" },
};

typedef struct {
    uint32_t magic;
    float    min[S_N], max[S_N];      // NAN = never seen
} record_t;

static record_t s_cur, s_last;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_dirty;

static lv_obj_t *s_scr;
static lv_obj_t *s_v[S_N][4];         // this min, this max, last min, last max

// Oil pressure only counts once the engine has been running a moment;
// otherwise every drive's minimum is the 0 psi before start.
#define OIL_RUN_RPM 400
#define OIL_RUN_MS  3000

static void blank(record_t *r)
{
    r->magic = MM_MAGIC;
    for (int i = 0; i < S_N; i++) r->min[i] = r->max[i] = NAN;
}

static bool has_data(const record_t *r)
{
    if (r->magic != MM_MAGIC) return false;
    for (int i = 0; i < S_N; i++)
        if (!isnan(r->max[i])) return true;
    return false;
}

static bool load(nvs_handle_t h, const char *key, record_t *r)
{
    size_t n = sizeof *r;
    return nvs_get_blob(h, key, r, &n) == ESP_OK && n == sizeof *r && r->magic == MM_MAGIC;
}

void minmax7_init(void)
{
    blank(&s_cur);
    blank(&s_last);

    nvs_handle_t h;
    if (nvs_open(MM_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable, records will not persist");
        return;
    }
    // Promote the previous drive. An empty one (a bench boot with no car)
    // does not overwrite a real last drive.
    record_t prev;
    if (load(h, MM_KEY_CUR, &prev) && has_data(&prev)) {
        nvs_set_blob(h, MM_KEY_LAST, &prev, sizeof prev);
        nvs_erase_key(h, MM_KEY_CUR);
        nvs_commit(h);
        ESP_LOGI(TAG, "previous drive moved to last");
    }
    if (!load(h, MM_KEY_LAST, &s_last)) blank(&s_last);
    nvs_close(h);
}

static void take(int i, float v)
{
    if (isnan(v)) return;
    if (isnan(s_cur.min[i]) || v < s_cur.min[i]) { s_cur.min[i] = v; s_dirty = true; }
    if (isnan(s_cur.max[i]) || v > s_cur.max[i]) { s_cur.max[i] = v; s_dirty = true; }
}

void minmax7_feed(const dash7_values_t *v)
{
    static int64_t running_since;
    int64_t now = esp_timer_get_time() / 1000;
    bool turning = !isnan(v->rpm) && v->rpm >= OIL_RUN_RPM;
    if (!turning)            running_since = 0;
    else if (!running_since) running_since = now;

    taskENTER_CRITICAL(&s_mux);
    take(S_WATER,    v->water_f);
    take(S_OIL_TEMP, v->oil_temp_f);
    if (turning && now - running_since >= OIL_RUN_MS)
        take(S_OIL_PSI, v->oil_psi);
    take(S_IAT1,     v->iat_f);
    take(S_IAT2,     v->iat2_f);
    take(S_TRANS,    v->trans_f);
    take(S_BOOST,    v->boost_psi);
    take(S_KNOCK,    v->knock_deg);
    taskEXIT_CRITICAL(&s_mux);
}

void minmax7_save(void)
{
    static int64_t last_ms;
    int64_t now = esp_timer_get_time() / 1000;
    if (!s_dirty || now - last_ms < MINMAX_SAVE_MS) return;

    record_t copy;
    taskENTER_CRITICAL(&s_mux);
    copy = s_cur;
    s_dirty = false;
    taskEXIT_CRITICAL(&s_mux);

    nvs_handle_t h;
    if (nvs_open(MM_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, MM_KEY_CUR, &copy, sizeof copy);
    nvs_commit(h);
    nvs_close(h);
    last_ms = now;
}

// ---- screen ----------------------------------------------------------------
#define ROW_Y0  96
#define ROW_H   44
static const int COL_X[4] = { 300, 430, 590, 700 };

static void refresh_cb(lv_timer_t *t)
{
    if (lv_scr_act() != s_scr) return;          // nothing to draw off-screen
    record_t cur;
    taskENTER_CRITICAL(&s_mux);
    cur = s_cur;
    taskEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < S_N; i++) {
        t7_set_num(s_v[i][0], cur.min[i],    SIG[i].fmt);
        t7_set_num(s_v[i][1], cur.max[i],    SIG[i].fmt);
        t7_set_num(s_v[i][2], s_last.min[i], SIG[i].fmt);
        t7_set_num(s_v[i][3], s_last.max[i], SIG[i].fmt);
    }
}

lv_obj_t *minmax7_create(int index)
{
    s_scr = t7_screen();

    lv_obj_t *t = t7_label(s_scr, "MIN / MAX", &lv_font_montserrat_28, C_WHITE);
    lv_obj_set_pos(t, 20, 14);

    lv_obj_t *h = t7_label(s_scr, "THIS DRIVE", &lv_font_montserrat_20, C_MUTED);
    lv_obj_set_pos(h, COL_X[0], 22);
    h = t7_label(s_scr, "LAST DRIVE", &lv_font_montserrat_20, C_DIM);
    lv_obj_set_pos(h, COL_X[2], 22);
    static const char *sub[4] = { "MIN", "MAX", "MIN", "MAX" };
    for (int c = 0; c < 4; c++) {
        h = t7_label(s_scr, sub[c], &lv_font_montserrat_14, c < 2 ? C_MUTED : C_DIM);
        lv_obj_set_pos(h, COL_X[c], 60);
    }

    for (int i = 0; i < S_N; i++) {
        int y = ROW_Y0 + i * ROW_H;
        lv_obj_t *n = t7_label(s_scr, SIG[i].name, &lv_font_montserrat_20, C_MUTED);
        lv_obj_set_pos(n, 20, y + 6);
        for (int c = 0; c < 4; c++) {
            bool now = c < 2;
            s_v[i][c] = t7_label(s_scr, "--",
                                 now ? &lv_font_montserrat_28 : &lv_font_montserrat_20,
                                 now ? C_WHITE : C_DIM);
            lv_obj_set_pos(s_v[i][c], COL_X[c], now ? y : y + 6);
        }
        if (i) {                                  // hairline between rows
            lv_obj_t *ln = lv_obj_create(s_scr);
            lv_obj_remove_style_all(ln);
            lv_obj_set_pos(ln, 20, y - 6);
            lv_obj_set_size(ln, 760, 1);
            lv_obj_set_style_bg_color(ln, lv_color_hex(C_TILE_LINE), 0);
            lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
        }
    }

    nav7_dots(s_scr, index);
    lv_timer_create(refresh_cb, 300, NULL);
    return s_scr;
}
