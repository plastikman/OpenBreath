// SPDX-License-Identifier: MIT
// Host test for the SAFETY-CRITICAL boot-time fault-restore decision
// (pb_heater_fault_decide) — a pure header inline, verified without an NVS
// backend. This logic is what guarantees a device that latched a safety fault
// comes back up LOCKED after a power cycle, and fails SAFE (latched) whenever the
// persisted fault state cannot be read reliably.
#include "pb_heater.h"

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
    pb_fault_reason_t code;

    // Fresh device: the NVS namespace was never written -> NOT latched.
    CHECK(pb_heater_fault_decide(false, /*ns_not_found=*/true,
                                 false, false, 0, 0, &code) == false);
    CHECK(code == PB_FAULT_NONE);

    // Namespace exists but the latch key was never written -> NOT latched.
    CHECK(pb_heater_fault_decide(true, false,
                                 false, /*latch_not_found=*/true, 0, 0, &code) == false);
    CHECK(code == PB_FAULT_NONE);

    // Latched with a valid stored code -> come up latched with that exact code.
    CHECK(pb_heater_fault_decide(true, false, /*latch_read_ok=*/true, false,
                                 1, PB_FAULT_CHAMBER_OVERTEMP, &code) == true);
    CHECK(code == PB_FAULT_CHAMBER_OVERTEMP);

    // Latch flag == 0 -> not latched even if a stale code sits in NVS.
    CHECK(pb_heater_fault_decide(true, false, true, false,
                                 0, PB_FAULT_PTC_OVERTEMP, &code) == false);
    CHECK(code == PB_FAULT_NONE);

    // Latched but the stored code is out of range (corrupt) -> latch anyway,
    // mapped to a generic cause rather than trusting the garbage byte.
    CHECK(pb_heater_fault_decide(true, false, true, false, 1, 250, &code) == true);
    CHECK(code == PB_FAULT_EMERGENCY);

    // Latched with a NONE(0) code is inconsistent -> also generic, still latched.
    CHECK(pb_heater_fault_decide(true, false, true, false,
                                 1, PB_FAULT_NONE, &code) == true);
    CHECK(code == PB_FAULT_EMERGENCY);

    // FAIL-SAFE: a genuine nvs_open error (not "namespace not found") -> latch.
    CHECK(pb_heater_fault_decide(/*open_ok=*/false, false, false, false,
                                 0, 0, &code) == true);
    CHECK(code == PB_FAULT_NVS_UNREADABLE);

    // FAIL-SAFE: opened OK but the latch key read genuinely failed -> latch.
    CHECK(pb_heater_fault_decide(true, false, /*latch_read_ok=*/false,
                                 /*latch_not_found=*/false, 0, 0, &code) == true);
    CHECK(code == PB_FAULT_NVS_UNREADABLE);

    puts("pb_heater fault-restore checks: PASS");
    return 0;
}
