// Diagnostic trouble codes over OBD-II: read (mode 03 stored, mode 07
// pending) and clear (mode 04).
//
// These are the emissions-related codes every OBD-II module must report --
// what a generic scan tool sees and what lights the check engine lamp. GM's
// body/chassis-only codes live behind manufacturer services and do not show.
//
// Requests go out functionally on 0x7DF, so the ECM, TCM and any other OBD
// module answer for themselves. The poller is paused for the duration so a
// routine poll never lands in the middle of a multi-frame answer.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DTC_MAX 32

typedef struct {
    char     code[6];      // "P0301"
    uint16_t module;       // answering CAN id, 0x7E8 = ECM, 0x7EA = TCM
    bool     pending;      // mode 07 (not yet confirmed) rather than mode 03
} dtc_entry_t;

typedef enum { DTC_IDLE, DTC_BUSY, DTC_DONE } dtc_state_t;

typedef struct {
    dtc_state_t state;
    uint32_t    version;   // bumps on every change, so the UI redraws only then
    int         count;
    dtc_entry_t codes[DTC_MAX];
    char        msg[160];  // one-line outcome for the screen
} dtc_result_t;

void dtc_init(void);
void dtc_request_read(void);
void dtc_request_clear(void);

// Copies the latest result. Safe from any task.
void dtc_get(dtc_result_t *out);

// From the CAN receive task, before the poller sees the frame. True when the
// frame belonged to a trouble-code exchange.
bool dtc_handle_frame(uint32_t id, const uint8_t *d, uint8_t dlc);

// "ECM", "TCM", or the raw id.
const char *dtc_module_name(uint16_t id, char *buf, int n);
