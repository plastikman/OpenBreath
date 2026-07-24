// SPDX-License-Identifier: MIT
// Host test for the SAFETY-CRITICAL heater safety-trip decision
// (pb_heater_eval_trip) — a pure header inline that encodes the exact, load-bearing
// priority ladder pb_heater_tick() runs every control cycle. Verified without the
// ADC/SSR/RTOS backend. This is what guarantees the heater fails CLOSED: over-temp
// on either sensor, a non-OK (open/short/uninit) thermistor while armed, or a
// comms-loss deadman timeout must each latch the heater OFF with the right cause.
#include "pb_heater.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

// Convenience: the tick treats a sensor read as OK/valid or not; a non-OK read
// also carries a NAN temperature (see pb_ntc_read), which these cases model.
#define OK   true
#define BAD  false

int main(void)
{
    // --- All clear: nothing armed, temps nominal -> no trip. --------------------
    CHECK(pb_heater_eval_trip(OK, 25.0f, OK, 25.0f, /*armed=*/false, /*link=*/false)
          == PB_FAULT_NONE);
    // Armed and healthy and link alive -> safe to run the bang-bang loop.
    CHECK(pb_heater_eval_trip(OK, 40.0f, OK, 50.0f, /*armed=*/true, /*link=*/false)
          == PB_FAULT_NONE);

    // --- Over-temp cutoffs fire even when NOT armed (unconditional). ------------
    // PTC at/above 105 °C.
    CHECK(pb_heater_eval_trip(OK, PB_HEATER_PTC_CUTOFF_C, OK, 25.0f, false, false)
          == PB_FAULT_PTC_OVERTEMP);
    CHECK(pb_heater_eval_trip(OK, 200.0f, OK, 25.0f, true, false)
          == PB_FAULT_PTC_OVERTEMP);
    // Chamber at/above 85 °C.
    CHECK(pb_heater_eval_trip(OK, 25.0f, OK, PB_HEATER_CHAMBER_MAX_C, false, false)
          == PB_FAULT_CHAMBER_OVERTEMP);
    // Just below the cutoff is NOT a trip (boundary).
    CHECK(pb_heater_eval_trip(OK, PB_HEATER_PTC_CUTOFF_C - 0.1f, OK,
                              PB_HEATER_CHAMBER_MAX_C - 0.1f, true, false)
          == PB_FAULT_NONE);

    // --- Priority: PTC over-temp outranks chamber over-temp. -------------------
    CHECK(pb_heater_eval_trip(OK, 110.0f, OK, 90.0f, true, false)
          == PB_FAULT_PTC_OVERTEMP);
    // Chamber over-temp outranks a (later-checked) sensor fault / comms loss.
    CHECK(pb_heater_eval_trip(OK, 25.0f, OK, 90.0f, true, /*link=*/true)
          == PB_FAULT_CHAMBER_OVERTEMP);

    // --- Fail-closed sensor faults: ONLY while armed. --------------------------
    // A dead chamber sensor while armed -> blind heater -> latch (chamber first).
    CHECK(pb_heater_eval_trip(OK, 25.0f, BAD, NAN, /*armed=*/true, false)
          == PB_FAULT_CHAMBER_SENSOR);
    // A dead PTC sensor while armed (chamber OK) -> unmonitored element -> latch.
    CHECK(pb_heater_eval_trip(BAD, NAN, OK, 25.0f, /*armed=*/true, false)
          == PB_FAULT_PTC_SENSOR);
    // Both bad while armed -> chamber-sensor wins the ladder.
    CHECK(pb_heater_eval_trip(BAD, NAN, BAD, NAN, true, false)
          == PB_FAULT_CHAMBER_SENSOR);
    // A non-OK sensor carries NAN temp: the *_ok gate must keep that NAN from
    // masking OR fabricating an over-temp trip. Not armed -> no trip at all.
    CHECK(pb_heater_eval_trip(BAD, NAN, BAD, NAN, /*armed=*/false, false)
          == PB_FAULT_NONE);
    // Not armed but a VALID over-temp still trips even with the other sensor dead.
    CHECK(pb_heater_eval_trip(OK, 120.0f, BAD, NAN, /*armed=*/false, false)
          == PB_FAULT_PTC_OVERTEMP);

    // --- Comms-loss watchdog: only while armed, lowest priority. ---------------
    CHECK(pb_heater_eval_trip(OK, 25.0f, OK, 25.0f, /*armed=*/true, /*link=*/true)
          == PB_FAULT_LINK_LOST);
    // Link expired but NOT armed -> no watchdog trip (idle heater is not heating).
    CHECK(pb_heater_eval_trip(OK, 25.0f, OK, 25.0f, /*armed=*/false, /*link=*/true)
          == PB_FAULT_NONE);
    // A sensor fault outranks the comms watchdog when both hold while armed.
    CHECK(pb_heater_eval_trip(OK, 25.0f, BAD, NAN, /*armed=*/true, /*link=*/true)
          == PB_FAULT_CHAMBER_SENSOR);

    puts("pb_heater safety-trip checks: PASS");
    return 0;
}
