// Combined 800x480 dash: the two round gauges merged onto one screen.
//
//   RPM bar across the top, MPH + PRNDM + gear in the centre, six tiles split
//   left and right, fuel and ethanol along the bottom.
#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// WARNING THRESHOLDS -- carried over unchanged from the two round gauges.
// A tile flashes red while outside these; it clears only once the reading has
// recovered past the CLEAR value, so a value sitting on the limit does not
// strobe; and once tripped it stays lit at least WARN_MIN_HOLD_MS, because a
// pressure dip can be over in 100ms -- too brief to notice otherwise.
// ---------------------------------------------------------------------------
#define WARN_WATER_MAX       245.0f
#define WARN_WATER_CLEAR     240.0f
#define WARN_OIL_TEMP_MAX    290.0f
#define WARN_OIL_TEMP_CLEAR  285.0f
#define WARN_OIL_PSI_MIN      15.0f
#define WARN_OIL_PSI_CLEAR    18.0f
#define WARN_OIL_PSI_MIN_RPM 400       // a stopped engine has no oil pressure
#define WARN_TRANS_MAX       260.0f
#define WARN_TRANS_CLEAR     255.0f
#define WARN_IAT_MAX         170.0f
#define WARN_IAT_CLEAR       165.0f

#define WARN_MIN_HOLD_MS    2000
#define WARN_FLASH_MS        450

#define DASH7_RPM_MAX       8000
#define DASH7_REDLINE       6500

// Every value is NAN when there is no reading; the dash shows "--".
typedef struct {
    float rpm;
    float mph;
    float fuel_pct;
    float ethanol_pct;
    float water_f;
    float oil_temp_f;
    float oil_psi;
    float trans_f;
    float iat_f;
    float boost_psi;
    char  prndl;        // 'P' 'R' 'N' 'D' 'M', or 0 for none
    int   gear;         // 1..8, -1 reverse, 0 none
} dash7_values_t;

// Shows the boot logo, then switches to the dash after boot_ms.
void dash7_create(uint32_t boot_ms);

// Call from an lv_timer. Redraws only what changed.
void dash7_update(const dash7_values_t *v);
