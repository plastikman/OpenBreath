// SPDX-License-Identifier: MIT
// pb_ntc — chamber + PTC-element temperature, reproducing the stock firmware's
// exact ADC->temperature conversion (reverse-engineered; see docs/NTC_CONVERSION.md).
//
//   Rntc_kOhm = Rref * V / (Vsupply - V)    low-side divider, Vsupply ~= 3.3 V
//   temp_C    = interpolate(114-entry R/T table)   (stock uses nearest-entry)
//
// Rref (82 or 33 kOhm) is selected by the GPIO19 strap at init. (The RE report's
// "Vrail = 0.1 V" was wrong; hardware confirmed ~3.3 V — see PB_VSUPPLY_V in the .c.)
#pragma once

#include "esp_err.h"

typedef enum {
    PB_NTC_OK = 0,      // reading valid
    PB_NTC_SHORT,       // raw under-range (thermistor shorted / very hot)
    PB_NTC_OPEN,        // raw over-range (thermistor open / disconnected)
    PB_NTC_UNINIT,      // pb_ntc_init not called / failed
} pb_ntc_status_t;

typedef enum {
    PB_NTC_CHAMBER = 0,
    PB_NTC_PTC = 1,
} pb_ntc_channel_t;

// Initialize ADC1 oneshot + curve-fit calibration for both channels and latch
// Rref from the board strap. Returns ESP_OK on success.
esp_err_t pb_ntc_init(void);

// Take one calibrated reading of the given channel. Returns the status; on
// PB_NTC_OK *out_c holds the INSTANTANEOUS temperature in C (NAN on any fault),
// so safety cutoffs act on the freshest sample. A successful read also feeds the
// moving-average filter behind pb_ntc_smoothed_c().
pb_ntc_status_t pb_ntc_read(pb_ntc_channel_t ch, float *out_c);

// Status of the most recent pb_ntc_read() for the channel (PB_NTC_UNINIT before
// the first read). Lets callers report/emit sensor state without re-reading.
pb_ntc_status_t pb_ntc_last_status(pb_ntc_channel_t ch);

// Latest smoothed temperature (5-sample moving average, for display/telemetry).
// NAN if no valid reading yet. Fed by pb_ntc_read on each OK sample.
float pb_ntc_smoothed_c(pb_ntc_channel_t ch);

// --- Per-channel temperature calibration (SAFETY-BOUNDED) -------------------
// A user-settable offset (°C) added to every calibrated reading so a mis-reading
// sensor can be corrected. The SAME calibrated value is returned to display,
// control regulation, AUTO, and the over-temp cutoffs — one consistent number.
//
// The offset is HARD-CLAMPED to [-PB_NTC_OFFSET_MAX_C, +PB_NTC_OFFSET_MAX_C] on
// every set AND on NVS load. Because the bound is ±5 °C, the fixed cutoffs can
// shift by at most 5 °C (chamber trips at ≥80 °C worst case, PTC at ≥100 °C worst
// case); calibration can never disable a fault or defeat the hardware backstops
// (see docs/SAFETY.md). The clamp is defined here as a pure function so it can be
// unit-tested and reused by both the read path and the NVS loader.
#define PB_NTC_OFFSET_MAX_C  5.0f

static inline float pb_ntc_clamp_offset_c(float v)
{
    if (v != v) return 0.0f;                                 // NaN -> no offset
    if (v < -PB_NTC_OFFSET_MAX_C) return -PB_NTC_OFFSET_MAX_C;  // also traps -Inf
    if (v >  PB_NTC_OFFSET_MAX_C) return  PB_NTC_OFFSET_MAX_C;  // also traps +Inf
    return v;
}

// Load persisted per-channel offsets from NVS (namespace app_nvs). MUST be called
// AFTER nvs_init(). Every loaded value is passed through the ±5 °C clamp, so a
// corrupt / hand-edited / out-of-range stored value clamps — it is never applied
// raw. Missing keys default to 0.
void pb_ntc_load_calibration(void);

// Set a channel's calibration offset (°C). The value is clamped to
// [-PB_NTC_OFFSET_MAX_C, +PB_NTC_OFFSET_MAX_C] and then persisted to NVS.
// Returns ESP_ERR_INVALID_ARG for an unknown channel.
esp_err_t pb_ntc_set_offset_c(pb_ntc_channel_t ch, float offset_c);

// Current (clamped) calibration offset for the channel, in °C.
float pb_ntc_get_offset_c(pb_ntc_channel_t ch);

#ifdef CONFIG_PB_HIL_DEVBOARD
// Dev-board HIL backend. Production builds do not expose or compile this API.
void pb_ntc_hil_set(pb_ntc_channel_t ch, pb_ntc_status_t status, float temp_c);
void pb_ntc_hil_reset(void);
#endif
