#include "dash_demo7.h"

#include <math.h>
#include "esp_timer.h"

#define SWEEP_SEC 8.0f      // idle -> redline -> idle
#define FUEL_SEC  45.0f     // full -> empty
#define IDLE_RPM  800.0f
#define MAX_RPM   7000.0f
#define WARN_SEC 10.0f      // seconds per warning phase: long enough for the 5s takeover

static int64_t s_t0;

void dash_demo7_start(void) { s_t0 = esp_timer_get_time(); }

void dash_demo7_sample(dash7_values_t *o)
{
    float t = (esp_timer_get_time() - s_t0) / 1e6f;
    float ph = fmodf(t, SWEEP_SEC) / SWEEP_SEC;
    float ramp = ph < 0.5f ? ph * 2.0f : 1.0f - (ph - 0.5f) * 2.0f;
    float load = ramp;

    o->rpm         = IDLE_RPM + ramp * (MAX_RPM - IDLE_RPM);
    o->mph         = o->rpm / 50.0f;
    o->fuel_pct    = 100.0f - fmodf(t, FUEL_SEC) / FUEL_SEC * 100.0f;
    o->ethanol_pct = 10.0f;

    float warm = 1.0f - expf(-t / 20.0f);
    float wob  = sinf(t * 0.6f) * 3.0f;
    o->water_f    = 100.0f + warm * 95.0f  + wob;
    o->oil_temp_f = 100.0f + warm * 115.0f + wob;
    o->trans_f    = 100.0f + warm * 80.0f  + wob;
    o->iat_f      = 90.0f  + warm * 50.0f  + wob;
    o->oil_psi    = 25.0f + load * 45.0f;
    o->boost_psi  = -8.0f + load * 18.0f;
    o->iat2_f     = o->iat_f + 15.0f + load * 40.0f;
    o->knock_deg  = load > 0.85f ? (load - 0.85f) * 20.0f : 0.0f;

    static const char sel[] = "PRNDM";
    o->prndl = sel[((int)(t / 3.0f)) % 5];
    int step = ((int)(t / 2.0f)) % 9;
    o->gear  = step == 8 ? -1 : step + 1;

#if DEMO_EXERCISE_WARNINGS
    switch (((int)(t / WARN_SEC)) % 6) {
        case 1: o->water_f    = WARN_WATER_MAX + 10.0f;    break;
        case 2: o->oil_temp_f = WARN_OIL_TEMP_MAX + 10.0f; break;
        case 3: o->oil_psi    = WARN_OIL_PSI_MIN - 8.0f;   break;
        case 4: o->trans_f    = WARN_TRANS_MAX + 10.0f;    break;
        case 5: o->iat_f      = WARN_IAT_MAX + 15.0f;      break;
        default: break;                                    // all in range
    }
#endif
}
