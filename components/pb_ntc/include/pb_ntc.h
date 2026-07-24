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

#ifdef CONFIG_PB_HIL_DEVBOARD
// Dev-board HIL backend. Production builds do not expose or compile this API.
void pb_ntc_hil_set(pb_ntc_channel_t ch, pb_ntc_status_t status, float temp_c);
void pb_ntc_hil_reset(void);
#endif
