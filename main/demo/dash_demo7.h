// Bench demo for the 7in dash: a plausible engine with no car attached.
#pragma once

#include "dash7.h"

// 1 = simulated engine, CAN never starts. 0 = real data from the car.
#define DASH_DEMO_MODE 0

// Walk each tile past its warning threshold in turn so the red flashes can be
// checked on the bench. 0 = plausible values only, nothing ever alarms.
#define DEMO_EXERCISE_WARNINGS 1

void dash_demo7_start(void);
void dash_demo7_sample(dash7_values_t *out);
