// SPDX-License-Identifier: MIT
// Host test for the SAFETY-CRITICAL sensor-calibration clamp. The offset is a
// pure, header-inline function (pb_ntc_clamp_offset_c) so it can be verified
// without the ADC/NVS backend. The firmware applies this exact clamp on every
// set AND on every NVS load, so a stored value can never exceed ±5 °C — which is
// what bounds how far the fixed over-temp cutoffs can shift.
#include "pb_ntc.h"

#include <limits.h>
#include <math.h>
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
    // The bound is exactly ±5 °C — the cutoffs can shift by at most this much.
    CHECK(PB_NTC_OFFSET_MAX_C == 5.0f);

    // In-range values pass through unchanged (incl. the endpoints).
    CHECK(pb_ntc_clamp_offset_c(0.0f)  == 0.0f);
    CHECK(pb_ntc_clamp_offset_c(2.5f)  == 2.5f);
    CHECK(pb_ntc_clamp_offset_c(-3.0f) == -3.0f);
    CHECK(pb_ntc_clamp_offset_c(5.0f)  == 5.0f);
    CHECK(pb_ntc_clamp_offset_c(-5.0f) == -5.0f);

    // Out-of-range values clamp to the bound — never applied raw.
    CHECK(pb_ntc_clamp_offset_c(5.1f)    == 5.0f);
    CHECK(pb_ntc_clamp_offset_c(100.0f)  == 5.0f);
    CHECK(pb_ntc_clamp_offset_c(-5.1f)   == -5.0f);
    CHECK(pb_ntc_clamp_offset_c(-999.0f) == -5.0f);

    // Non-finite inputs are neutralized: NaN -> 0, ±Inf -> ±bound. A corrupt
    // stored value can therefore never disable/skew a fault.
    CHECK(pb_ntc_clamp_offset_c(NAN)       == 0.0f);
    CHECK(pb_ntc_clamp_offset_c(INFINITY)  == 5.0f);
    CHECK(pb_ntc_clamp_offset_c(-INFINITY) == -5.0f);

    // --- NVS-LOAD path -------------------------------------------------------
    // Offsets are stored as centi-°C; pb_ntc_load_calibration() re-clamps every
    // loaded value through pb_ntc_clamp_offset_centi() (the SAME bound the setter
    // uses) so a corrupt / hand-edited / out-of-range STORED value clamps to
    // ±5 °C on load and can never be applied raw. Exercise that shared helper.
    CHECK(PB_NTC_OFFSET_CENTI_MAX == 500);

    // In-range stored centi values pass through unchanged (incl. endpoints).
    CHECK(pb_ntc_clamp_offset_centi(0)    == 0);
    CHECK(pb_ntc_clamp_offset_centi(250)  == 250);
    CHECK(pb_ntc_clamp_offset_centi(-300) == -300);
    CHECK(pb_ntc_clamp_offset_centi(500)  == 500);
    CHECK(pb_ntc_clamp_offset_centi(-500) == -500);

    // Out-of-range STORED values clamp to ±5 °C (±500 centi) on load — never raw.
    CHECK(pb_ntc_clamp_offset_centi(501)     == 500);
    CHECK(pb_ntc_clamp_offset_centi(10000)   == 500);
    CHECK(pb_ntc_clamp_offset_centi(-501)    == -500);
    CHECK(pb_ntc_clamp_offset_centi(-10000)  == -500);
    CHECK(pb_ntc_clamp_offset_centi(INT_MAX) == 500);
    CHECK(pb_ntc_clamp_offset_centi(INT_MIN) == -500);

    printf("pb_ntc calibration clamp checks: PASS\n");
    return 0;
}
