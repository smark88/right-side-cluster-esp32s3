// GM mode 22 PID sweep -- for finding a value HP Tuners shows but no
// published PID list has (ethanol %, the real oil temp sensor, ...).
//
// It asks the ECM (0x7E0) for every mode 22 PID in PID_SCAN_FIRST..LAST, one
// at a time, and logs every positive answer with its raw bytes. Read-only:
// mode 22 is "read data by identifier", nothing is written to the car.
//
// HOW TO USE
//   1. Set PID_SCAN_MODE to 1 below, build, flash.
//   2. In the car: key on, engine running (so temps are real), HP Tuners
//      showing the value you are hunting. Laptop on the UART port with
//      idf.py monitor if you want the full log; the screen shows the hits.
//   3. Wait for "DONE" (~3 min). Note the candidates.
//   4. Set PID_SCAN_MODE back to 0 and reflash -- the dash does not run
//      while scanning.
//
// The screen flags answers whose first byte matches PID_SCAN_TARGETS: the
// raw byte a value would have if it uses the usual encodings.

#pragma once

// 1 = sweep mode 22 and show the results, no dash. 0 = normal dash.
#define PID_SCAN_MODE 0

// 0x00xx is GM serving the standard mode 01 PIDs through mode 22.
#define PID_SCAN_FIRST 0x0000
#define PID_SCAN_LAST  0x2FFF

void pid_scan_start(void);
