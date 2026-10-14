// Settings page: demo mode on/off at runtime, each way behind a confirm.
//
// Demo is never saved: every power-on starts on live data from the car. While
// it is on the poller stops asking the car, the odometer and min/max do not
// record, and the gauges carry a DEMO badge.
#pragma once

#include <stdbool.h>
#include "lvgl.h"

lv_obj_t *settings7_create(int index);

// True while the gauges should show simulated values. Always true in a bench
// (Demo run mode) build, where CAN never starts.
bool settings7_demo(void);
