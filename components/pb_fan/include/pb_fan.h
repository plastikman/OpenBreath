// SPDX-License-Identifier: MIT
// pb_fan — AC blower ON/OFF control (matches stock firmware; see docs/FAN_DRIVE.md).
//
// Stock drives the fan gate as a HELD level switched at zero-cross (HIGH = full
// on, LOW = off) — no phase-angle, no PWM, no variable speed. The gate must
// NEVER be PWM'd (free-running switching destroys the TRIAC). This driver mirrors
// stock: on/off only.
#pragma once

#include <stdint.h>
#include "esp_err.h"

// Configure the gate output (idle low) + the zero-cross ISR (clean AC switching).
esp_err_t pb_fan_init(void);

// Fan on/off. 0 = off; any non-zero = on (no variable speed — intermediate values
// are treated as "on"). Off takes effect immediately; on is applied at the next
// zero crossing.
void pb_fan_set_level(uint8_t percent);
uint8_t pb_fan_get_level(void);

// Diagnostics: total zero-cross interrupts since boot, and microseconds between
// the last two edges (mains cycle period). interval 0 = no zero-cross seen yet
// (ZCD not working / no mains) — lets us verify the detector without firing.
void pb_fan_zc_diag(uint32_t *count_out, uint32_t *interval_us_out);

#ifdef CONFIG_PB_HIL_DEVBOARD
// Inject one or more zero-cross events into the dev-board backend.
void pb_fan_hil_zero_cross(uint32_t count, uint32_t interval_us);
#endif
