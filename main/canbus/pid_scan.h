// GM mode 22 PID sweep -- for finding a value HP Tuners shows but no
// published PID list has (ethanol %, the real oil temp sensor, ...).
//
// It asks the ECM (0x7E0) for every mode 22 PID in PID_SCAN_FIRST..LAST, one
// at a time, and logs every positive answer with its raw bytes. Read-only:
// mode 22 is "read data by identifier", nothing is written to the car.
//
// HOW TO USE
//   1. menuconfig -> Dash (7in) -> Run mode -> PID scan, build, flash.
//   2. In the car: key on, engine running (so temps are real), HP Tuners
//      showing the value you are hunting. Laptop on the UART port with
//      idf.py monitor if you want the full log; the screen shows the hits.
//   3. Wait for "DONE" (~3 min). Note the candidates.
//   4. Set the run mode back to Car and reflash -- the dash does not run
//      while scanning.
//
// The screen flags answers whose first byte matches PID_SCAN_TARGETS: the
// raw byte a value would have if it uses the usual encodings.

#pragma once

#include "sdkconfig.h"

// Set in menuconfig -> Dash (7in). 1 = sweep mode 22 and show the results, no
// dash. 0 = normal dash.
#ifdef CONFIG_DASH_MODE_PID_SCAN
#define PID_SCAN_MODE 1
// 0x00xx is GM serving the standard mode 01 PIDs through mode 22.
#define PID_SCAN_FIRST CONFIG_DASH_PID_SCAN_FIRST
#define PID_SCAN_LAST  CONFIG_DASH_PID_SCAN_LAST
#else
#define PID_SCAN_MODE 0
#define PID_SCAN_FIRST 0x0000
#define PID_SCAN_LAST  0x2FFF
#endif

void pid_scan_start(void);
