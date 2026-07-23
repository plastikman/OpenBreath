// SPDX-License-Identifier: MIT
// pb_heater — chamber heater control with defense-in-depth over-temp safety.
//
// Hardware backstops that exist REGARDLESS of this firmware (see docs/SAFETY.md):
//   1. Bonded thermal cutoff in the PTC's mains lead (opens on element over-temp,
//      upstream of the SSR — covers a welded-shut SSR).
//   2. PTC ceramic self-limiting (Curie point) — cannot thermally run away.
// This component is the third (soft) layer: correct set-point control + an
// element/chamber over-temp cutoff + a comms-loss watchdog.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// --- Safety limits ----------------------------------------------------------
// Fixed hardware-safety cutoffs — NEVER user-configurable.
#define PB_HEATER_PTC_CUTOFF_C   105.0f   // element over-temp -> force off (stock parity)
#define PB_HEATER_CHAMBER_MAX_C  85.0f    // chamber over-temp -> force off
#define PB_HEATER_HYSTERESIS_C   1.0f
// Settable set-point ceiling: default + absolute cap + floor. Raising the
// production cap above 70 C is a separate hw-validation item (80 C leaves only
// 5 C below the fixed 85 C chamber trip — not enough margin without measured
// worst-case lag/overshoot), so ABS_MAX stays at 70.
#define PB_HEATER_MAX_TARGET_C_DEFAULT  70.0f
#define PB_HEATER_ABS_MAX_TARGET_C      70.0f
#define PB_HEATER_MIN_TARGET_C          30.0f
// Comms deadman: no controller for this long while heating -> latch off. Runtime-
// configurable within [MIN, MAX]; never disabled or extended beyond MAX (5 min).
#define PB_HEATER_COMMS_TIMEOUT_MS_DEFAULT  (5 * 60 * 1000)
#define PB_HEATER_COMMS_TIMEOUT_MS_MIN      (10 * 1000)
#define PB_HEATER_COMMS_TIMEOUT_MS_MAX      (5 * 60 * 1000)

// Bring up the SSR GPIO in a guaranteed-OFF state. Call before anything can
// request heat. Idempotent.
esp_err_t pb_heater_init(void);

// Set the desired chamber temperature (clamped to the runtime ceiling returned
// by pb_heater_get_max_target_c(), never above PB_HEATER_ABS_MAX_TARGET_C).
// 0 (or below) disables heating. Returns:
//   ESP_OK             - accepted
//   ESP_ERR_INVALID_ARG   - non-finite (NaN/Inf) target, rejected
//   ESP_ERR_INVALID_STATE - a positive target while a safety fault is latched
//                           (clear it with pb_heater_clear_fault first); maps to
//                           HTTP 409. Heat is never queued behind a fault latch.
esp_err_t pb_heater_set_target_c(float target_c);
float pb_heater_get_target_c(void);

// --- Runtime-configurable, persisted settings (pb_heater is the sole owner) ---
// Load persisted settings from NVS (namespace app_nvs). MUST be called AFTER
// nvs_init() — pb_heater_init() only sets conservative defaults (nvs isn't up
// yet). Values are clamped on read.
void  pb_heater_load_config(void);
// Settable set-point ceiling. Setter clamps to [MIN_TARGET_C, ABS_MAX_TARGET_C],
// persists it, and pulls a live target down if it now exceeds the cap.
esp_err_t pb_heater_set_max_target_c(float max_c);
float     pb_heater_get_max_target_c(void);
// Comms deadman timeout. Setter clamps to [MS_MIN, MS_MAX] (never disabled or
// extended past 5 min) and persists it.
esp_err_t pb_heater_set_comms_timeout_ms(uint32_t ms);
uint32_t  pb_heater_get_comms_timeout_ms(void);

// Feed the comms watchdog: call whenever a live controller link is confirmed
// (Moonraker connected, or a fresh command). If not called within
// pb_heater_get_comms_timeout_ms() while heating, the heater latches off.
void pb_heater_notify_link_alive(void);

// Periodic control tick (call at ~1-2 Hz). Reads chamber + PTC temps, enforces
// all safety cutoffs, and drives the SSR with hysteresis around the set-point.
void pb_heater_tick(void);

// Immediate, latching shutoff. Clears the target and latches off. Heat stays off
// until pb_heater_clear_fault() is called AND the condition has cleared.
void pb_heater_emergency_off(const char *reason);

// Explicitly clear a latched safety fault (over-temp / sensor fault / comms loss).
// Setting a new target does NOT auto-clear it — this is a deliberate reset. The
// next tick re-evaluates safety and re-latches if the fault still holds.
// A permanent inhibit (pb_heater_inhibit) is NOT cleared by this.
void pb_heater_clear_fault(void);

// PERMANENT inhibit: force the heater off and refuse all heat until reboot. Unlike
// a latched fault this is NOT clearable by pb_heater_clear_fault()/API clear_fault —
// only a power cycle lifts it. Use for conditions under which the heater must
// never run again this boot (e.g. the control-loop watchdog could not be armed,
// so a hung loop would go undetected). Reports as a fault in API v2 state.
void pb_heater_inhibit(const char *reason);

// True once pb_heater_inhibit() has been called (reboot-only).
bool pb_heater_is_inhibited(void);

// True if a safety trip has latched the heater off (needs an explicit clear).
bool pb_heater_is_faulted(void);

// The reason string from the most recent trip (NULL if none / after clear).
const char *pb_heater_fault_reason(void);

// True if the SSR is currently commanded on (momentary bang-bang state).
bool pb_heater_is_on(void);

// True in "heat mode": a target is armed and no fault is latched. Steady across
// the SSR's bang-bang cycling — use this (not pb_heater_is_on) for the fan/LED.
bool pb_heater_heat_mode(void);
