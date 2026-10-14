// Bench demo for the 7in dash: a plausible engine with no car attached.
#pragma once

#include "dash7.h"

#include "sdkconfig.h"

// Set in menuconfig -> Dash (7in) -> Run mode. 1 = simulated engine, CAN never
// starts. 0 = real data from the car.
#ifdef CONFIG_DASH_MODE_DEMO
#define DASH_DEMO_MODE 1
#else
#define DASH_DEMO_MODE 0
#endif

// Walk each tile past its warning threshold in turn so the red flashes can be
// checked on the bench. 0 = plausible values only, nothing ever alarms.
#ifdef CONFIG_DASH_DEMO_EXERCISE_WARNINGS
#define DEMO_EXERCISE_WARNINGS 1
#else
#define DEMO_EXERCISE_WARNINGS 0
#endif

void dash_demo7_start(void);
void dash_demo7_sample(dash7_values_t *out);
