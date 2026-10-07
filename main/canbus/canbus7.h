// 7in-only additions to canbus.h (which stays byte-identical to gauge one's).
#pragma once
#include <stdint.h>

// esp_timer time of the last OBD reply, 0 if none yet. Tells the dash whether
// the car is answering at all.
int64_t canbus_last_obd_us(void);
