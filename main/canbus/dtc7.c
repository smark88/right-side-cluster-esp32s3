// OBD-II trouble codes. See dtc7.h.

#include "dtc7.h"
#include "canbus.h"
#include "obd_poll.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "DTC";

// Window to collect answers after each request. Modules answer within tens of
// ms; a multi-frame answer of a dozen codes is still well under this.
#define COLLECT_MS 600

#define N_MOD 8                    // 0x7E8..0x7EF

enum { CMD_READ = 1, CMD_CLEAR };

// Reassembly of one module's answer (ISO 15765-2 single / first+consecutive).
typedef struct {
    uint8_t buf[3 + 4 * 64];          // a 0x19 answer: 4 bytes per code
    int     len, got;
    uint8_t next_sn;
    bool    active;
} isotp_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;
static dtc_result_t      s_res;

// Owned by the CAN receive task while a request is out; the dtc task reads it
// only after the collect window, once s_expect is cleared again.
static volatile uint8_t  s_expect;  // positive response service (0x43/0x47/0x44), 0 = idle
static isotp_t           s_rx[N_MOD];
static uint8_t           s_done_mask, s_neg_mask, s_neg_code[N_MOD];
static uint8_t           s_payload[N_MOD][sizeof(((isotp_t *)0)->buf)];
static int               s_payload_len[N_MOD];

const char *dtc_module_name(uint16_t id, char *buf, int n)
{
    if (id == 0x7E8) return "ECM";
    if (id == 0x7EA) return "TCM";
    snprintf(buf, n, "%03X", id);
    return buf;
}

static void send(uint32_t id, const uint8_t d[8])
{
    twai_message_t m = { .identifier = id, .data_length_code = 8 };
    memcpy(m.data, d, 8);
    twai_transmit(&m, pdMS_TO_TICKS(20));
}

static void finish_module(int k, const uint8_t *p, int len)
{
    memcpy(s_payload[k], p, len);
    s_payload_len[k] = len;
    s_done_mask |= 1u << k;
}

bool dtc_handle_frame(uint32_t id, const uint8_t *d, uint8_t dlc)
{
    uint8_t want = s_expect;
    if (!want || id < 0x7E8 || id > 0x7EF || dlc < 3) return false;
    int k = id - 0x7E8;
    isotp_t *r = &s_rx[k];
    uint8_t pci = d[0] >> 4;

    if (pci == 0) {                                     // single frame
        int len = d[0] & 0x0F;
        if (d[1] == want) { finish_module(k, d + 1, len); return true; }
        if (d[1] == 0x7F && d[2] == (want - 0x40)) {    // refused
            s_neg_mask |= 1u << k;
            s_neg_code[k] = d[3];
            return true;
        }
        return false;                                   // a poll answer
    }
    if (pci == 1 && d[2] == want) {                     // first frame
        r->len = ((d[0] & 0x0F) << 8) | d[1];
        if (r->len > (int)sizeof r->buf) r->len = sizeof r->buf;
        memcpy(r->buf, d + 2, 6);
        r->got = 6;
        r->next_sn = 1;
        r->active = true;
        // Flow control to that module's physical id: send everything, no gap.
        const uint8_t fc[8] = { 0x30, 0x00, 0x00, 0, 0, 0, 0, 0 };
        send(id - 8, fc);
        return true;
    }
    if (pci == 2 && r->active) {                        // consecutive frame
        if ((d[0] & 0x0F) != (r->next_sn & 0x0F)) { r->active = false; return true; }
        r->next_sn++;
        int n = r->len - r->got;
        if (n > 7) n = 7;
        memcpy(r->buf + r->got, d + 1, n);
        r->got += n;
        if (r->got >= r->len) {
            r->active = false;
            finish_module(k, r->buf, r->len);
        }
        return true;
    }
    return false;
}

const char *dtc_kind_name(dtc_kind_t k)
{
    switch (k) {
        case DTC_CURRENT: return "current";
        case DTC_PENDING: return "pending";
        case DTC_HISTORY: return "history";
        default:          return "stored";
    }
}

// Sends one functional request (service plus up to 3 argument bytes) and
// waits out the collect window.
static void exchange_args(uint8_t service, const uint8_t *args, int nargs)
{
    memset(s_rx, 0, sizeof s_rx);
    s_done_mask = s_neg_mask = 0;
    s_expect = service + 0x40;
    uint8_t req[8] = { (uint8_t)(1 + nargs), service, 0, 0, 0, 0, 0, 0 };
    memcpy(req + 2, args, nargs);
    send(OBD_REQ_ID, req);
    vTaskDelay(pdMS_TO_TICKS(COLLECT_MS));
    s_expect = 0;
}

static void exchange(uint8_t service) { exchange_args(service, NULL, 0); }

static bool have(const dtc_result_t *res, uint16_t module, const char *code5)
{
    for (int i = 0; i < res->count; i++)
        if (res->codes[i].module == module && !strncmp(res->codes[i].code, code5, 5))
            return true;
    return false;
}

static void fmt_code(char *out, int n, uint8_t a, uint8_t b)
{
    static const char sys[] = "PCBU";
    snprintf(out, n, "%c%u%X%X%X", sys[a >> 6], (a >> 4) & 3, a & 0x0F, b >> 4, b & 0x0F);
}

// 59 02 payload: [59][02][availability mask] then [hi][lo][FTB][status] per
// code. Status bits (ISO 14229): 0 failing now, 2 pending, 3 confirmed.
static void decode19(dtc_result_t *res)
{
    for (int k = 0; k < N_MOD; k++) {
        if (!(s_done_mask & (1u << k))) continue;
        const uint8_t *p = s_payload[k];
        int len = s_payload_len[k];
        for (int i = 3; i + 3 < len && res->count < DTC_MAX; i += 4) {
            uint8_t a = p[i], b = p[i + 1], ftb = p[i + 2], st = p[i + 3];
            if (!a && !b) continue;
            dtc_entry_t *e = &res->codes[res->count++];
            char c5[6];
            fmt_code(c5, sizeof c5, a, b);
            snprintf(e->code, sizeof e->code, "%s-%02X", c5, ftb);
            e->module = 0x7E8 + k;
            e->kind = (st & 0x01) ? DTC_CURRENT
                    : (st & 0x04) ? DTC_PENDING
                    : (st & 0x08) ? DTC_HISTORY : DTC_STORED;
        }
    }
}

// 43/47 payload on CAN: [service][count][hi lo]... Codes 0x19 already
// reported for that module are not repeated.
static void decode(dtc_result_t *res, bool pending)
{
    for (int k = 0; k < N_MOD; k++) {
        if (!(s_done_mask & (1u << k))) continue;
        const uint8_t *p = s_payload[k];
        int len = s_payload_len[k];
        for (int i = 2; i + 1 < len && res->count < DTC_MAX; i += 2) {
            uint8_t a = p[i], b = p[i + 1];
            if (!a && !b) continue;                     // padding
            char c5[6];
            fmt_code(c5, sizeof c5, a, b);
            if (have(res, 0x7E8 + k, c5)) continue;
            dtc_entry_t *e = &res->codes[res->count++];
            snprintf(e->code, sizeof e->code, "%s", c5);
            e->module = 0x7E8 + k;
            e->kind   = pending ? DTC_PENDING : DTC_STORED;
        }
    }
}

static int popcount8(uint8_t m) { int n = 0; while (m) { n += m & 1; m >>= 1; } return n; }

static void publish(const dtc_result_t *r)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t v = s_res.version + 1;
    s_res = *r;
    s_res.version = v;
    xSemaphoreGive(s_lock);
}

static void dtc_task(void *arg)
{
    static dtc_result_t r;
    for (;;) {
        uint32_t cmd = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        memset(&r, 0, sizeof r);
        r.state = DTC_BUSY;
        snprintf(r.msg, sizeof r.msg, cmd == CMD_CLEAR ? "Clearing..." : "Reading...");
        publish(&r);

        obd_poll_pause(true);
        vTaskDelay(pdMS_TO_TICKS(60));                 // let the last poll answer land

        if (cmd == CMD_READ) {
            // All codes, any status, from every module that speaks UDS.
            static const uint8_t by_status[2] = { 0x02, 0xFF };
            exchange_args(0x19, by_status, 2);
            uint8_t answered = s_done_mask, uds = s_done_mask;
            decode19(&r);
            ESP_LOGI(TAG, "0x19: answered 0x%02X refused 0x%02X", uds, s_neg_mask);

            exchange(0x03);
            answered |= s_done_mask;
            decode(&r, false);
            exchange(0x07);
            answered |= s_done_mask;
            decode(&r, true);
            if (!answered)
                snprintf(r.msg, sizeof r.msg, "No module answered. Key on?");
            else {
                int n = r.count
                    ? snprintf(r.msg, sizeof r.msg, "%d code%s", r.count, r.count == 1 ? "" : "s")
                    : snprintf(r.msg, sizeof r.msg, "No codes");
                // Who answered the full (0x19) read, so an empty list can be
                // told apart from a module that only gave its emissions codes.
                char nb[8];
                n += snprintf(r.msg + n, sizeof r.msg - n, "  --  full read:");
                for (int k = 0; k < N_MOD; k++)
                    if (answered & (1u << k))
                        n += snprintf(r.msg + n, sizeof r.msg - n, " %s %s",
                                      dtc_module_name(0x7E8 + k, nb, sizeof nb),
                                      (uds & (1u << k)) ? "yes" : "no");
            }
        } else {
            // UDS clear-all first (the non-emissions and history codes), then
            // OBD mode 04. Outcome reported from mode 04, which every OBD
            // module answers.
            static const uint8_t all_groups[3] = { 0xFF, 0xFF, 0xFF };
            exchange_args(0x14, all_groups, 3);
            ESP_LOGI(TAG, "0x14: answered 0x%02X refused 0x%02X", s_done_mask, s_neg_mask);
            exchange(0x04);
            int n = 0;
            char nb[8];
            n += snprintf(r.msg + n, sizeof r.msg - n, "Clear: ");
            if (!s_done_mask && !s_neg_mask)
                n += snprintf(r.msg + n, sizeof r.msg - n, "no module answered. Key on?");
            for (int k = 0; k < N_MOD; k++) {
                const char *nm = dtc_module_name(0x7E8 + k, nb, sizeof nb);
                if (s_done_mask & (1u << k))
                    n += snprintf(r.msg + n, sizeof r.msg - n, "%s cleared  ", nm);
                else if (s_neg_mask & (1u << k))
                    // 0x22 conditionsNotCorrect: most ECMs refuse with the engine running.
                    n += snprintf(r.msg + n, sizeof r.msg - n, "%s refused%s  ", nm,
                                  s_neg_code[k] == 0x22 ? " (engine must be off)" : "");
            }
        }
        obd_poll_pause(false);
        r.state = DTC_DONE;
        publish(&r);
        ESP_LOGI(TAG, "%s", r.msg);
    }
}

void dtc_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(dtc_task, "dtc", 4096, NULL, 6, &s_task, 0);
}

void dtc_request_read(void)  { if (s_task && s_res.state != DTC_BUSY) xTaskNotify(s_task, CMD_READ,  eSetValueWithOverwrite); }
void dtc_request_clear(void) { if (s_task && s_res.state != DTC_BUSY) xTaskNotify(s_task, CMD_CLEAR, eSetValueWithOverwrite); }

void dtc_get(dtc_result_t *out)
{
    if (!s_lock) { memset(out, 0, sizeof *out); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_res;
    xSemaphoreGive(s_lock);
}
