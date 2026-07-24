// SPDX-License-Identifier: MIT
// Host test for the SAFETY-CRITICAL NTC sample classifier (pb_ntc_classify) — a
// pure header inline that maps a raw ADC count + pin voltage to OK / OPEN / SHORT.
// This is the EXACT ladder pb_ntc_read() runs, so an open (disconnected), shorted,
// or rail-pinned thermistor is reported as a fault status; the heater then treats
// any non-OK status as fail-closed while armed (see pb_heater_eval_trip). Verified
// without the ADC backend.
#include "pb_ntc.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

int main(void)
{
    // Threshold sanity — the classifier is only as good as these constants.
    CHECK(PB_NTC_RAW_OPEN_MAX  == 0xFFD);
    CHECK(PB_NTC_RAW_SHORT_MIN == 0x14);
    CHECK(PB_NTC_VSUPPLY_V     == 3.3f);

    // --- Nominal mid-range sample -> OK. ---------------------------------------
    // A mid-scale count with a mid-rail voltage is a good reading.
    CHECK(pb_ntc_classify(2048, 1.65f) == PB_NTC_OK);
    CHECK(pb_ntc_classify(0x15, 0.1f)  == PB_NTC_OK);   // one above SHORT floor
    CHECK(pb_ntc_classify(0xFFD, 3.2f) == PB_NTC_OK);   // exactly at OPEN ceiling

    // --- OPEN: raw over-range (thermistor disconnected). -----------------------
    CHECK(pb_ntc_classify(0xFFE, 3.29f) == PB_NTC_OPEN); // one past the ceiling
    CHECK(pb_ntc_classify(0xFFF, 3.29f) == PB_NTC_OPEN); // full-scale
    CHECK(pb_ntc_classify(4095, 2.0f)   == PB_NTC_OPEN); // raw wins over an OK-ish v

    // --- SHORT: raw under-range (thermistor shorted / very hot). ---------------
    CHECK(pb_ntc_classify(0x14, 0.05f) == PB_NTC_SHORT); // exactly at the floor
    CHECK(pb_ntc_classify(0x00, 0.0f)  == PB_NTC_SHORT); // zero count
    CHECK(pb_ntc_classify(0x13, 1.5f)  == PB_NTC_SHORT); // raw wins over an OK-ish v

    // --- Rail-range guard: raw in-band but voltage pinned high/low -> OPEN. -----
    // Even with a plausible raw count, a pin voltage at/below 0 or at/above the
    // supply cannot yield a real divider ratio, so it is treated as OPEN.
    CHECK(pb_ntc_classify(2048, 0.0f)   == PB_NTC_OPEN);   // v <= 0
    CHECK(pb_ntc_classify(2048, -0.1f)  == PB_NTC_OPEN);   // negative rail
    CHECK(pb_ntc_classify(2048, 3.3f)   == PB_NTC_OPEN);   // v >= Vsupply
    CHECK(pb_ntc_classify(2048, 5.0f)   == PB_NTC_OPEN);   // over-rail
    // Just inside the rail band with an in-band raw -> OK.
    CHECK(pb_ntc_classify(2048, 0.01f)  == PB_NTC_OK);
    CHECK(pb_ntc_classify(2048, 3.29f)  == PB_NTC_OK);

    // --- Priority: the raw checks precede the rail-range check. -----------------
    // An out-of-range raw is classified before v is even considered.
    CHECK(pb_ntc_classify(0xFFF, 1.65f) == PB_NTC_OPEN);
    CHECK(pb_ntc_classify(0x00,  1.65f) == PB_NTC_SHORT);

    puts("pb_ntc status-classify checks: PASS");
    return 0;
}
