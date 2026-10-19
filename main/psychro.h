// psychro.h — dew point from temperature + relative humidity, and the airing-out hint.
// Magnus formula (Alduchov & Eskridge 1996) over liquid water, the meteorological convention (same as
// Open-Meteo); error < ~0.4 % from -40 to +50 °C. All temperatures in °C: convert only to display.
#pragma once

#include <math.h>
#include <stdbool.h>

#define PSY_A 17.625f
#define PSY_B 243.04f   // °C
#define PSY_C 0.61094f  // kPa

// Airing-out hint from dTd = dew point inside - dew point outside (°C), with hysteresis: a sensor's
// humidity is ~±3 %, about ±0.5 °C of dew point, so ~±0.7 °C on a difference of two sensors.
// "Yes" also needs the inside humidity at the limit or above (mould risk; until 5 % under it): drier
// air outside alone isn't a reason to cool the house
#define PSY_VENT_YES_ENTER_C 3.0f
#define PSY_VENT_YES_EXIT_C  2.5f
#define PSY_VENT_NO_ENTER_C  1.0f
#define PSY_VENT_NO_EXIT_C   1.5f
#define PSY_VENT_RH_DEFAULT  60     // inside humidity (%) from which airing out is worth it
#define PSY_VENT_RH_EXIT     5      // "yes" holds until the humidity is this far under the limit

typedef enum { PSY_VENT_NEUTRAL = 0, PSY_VENT_YES, PSY_VENT_NO } psy_vent_t;

static inline float psy_dew_point_c(float t_c, float rh)
{
    rh = rh < 1.0f ? 1.0f : rh > 100.0f ? 100.0f : rh;   // avoids logf(0)
    float ea = PSY_C * expf(PSY_A * t_c / (PSY_B + t_c)) * rh / 100.0f;   // actual vapour pressure, kPa
    float g = logf(ea / PSY_C);
    return PSY_B * g / (PSY_A - g);
}

// rh_in = inside humidity (%), rh_limit = from which it's worth airing out
static inline psy_vent_t psy_vent_next(psy_vent_t prev, float dtd_c, float rh_in, int rh_limit)
{
    bool yes = prev == PSY_VENT_YES ? dtd_c >= PSY_VENT_YES_EXIT_C && rh_in >= rh_limit - PSY_VENT_RH_EXIT
                                    : dtd_c >= PSY_VENT_YES_ENTER_C && rh_in >= rh_limit;
    if (yes) {
        return PSY_VENT_YES;
    }
    if (prev == PSY_VENT_NO ? dtd_c <= PSY_VENT_NO_EXIT_C : dtd_c <= PSY_VENT_NO_ENTER_C) {
        return PSY_VENT_NO;   // the air outside holds as much water or more: airing out won't dry
    }
    return PSY_VENT_NEUTRAL;
}
