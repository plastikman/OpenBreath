// SPDX-License-Identifier: MIT
// pb_leds — the four button-indicator LEDs (active-high via 1K on GPIO6/5/4,
// plus Power on GPIO21). One 50 ms-tick task drives them all from an atomic
// per-LED pattern, so any task can set a pattern without touching GPIO or
// blocking. pb_heater no longer drives any LED; pb_policy owns the indication:
// Power = device alive (blink on fault), On/Auto/Dry = the active mode.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Physical panel labels (top→bottom on the board: Power, Dry, On, Auto), mapped to
// their driving GPIO. Confirmed by stock-firmware RE: 4 direct active-high outputs,
// no matrix. "Power" is on GPIO21 (also UART0-TX) and is only driven when
// CONFIG_PB_POWER_LED is set — otherwise that pin stays the serial console.
typedef enum {
    PB_LED_K1 = 0,   // GPIO6  = "Auto"
    PB_LED_K2 = 1,   // GPIO5  = "On"
    PB_LED_K3 = 2,   // GPIO4  = "Dry"
    PB_LED_K4 = 3,   // GPIO21 = "Power" (heating indicator; console-TX pin)
    PB_LED_COUNT = 4,
    // Semantic aliases (use these; the K-names are the physical button positions).
    PB_LED_AUTO  = PB_LED_K1,
    PB_LED_ON    = PB_LED_K2,
    PB_LED_DRY   = PB_LED_K3,
    PB_LED_POWER = PB_LED_K4,
} pb_led_id_t;

typedef enum {
    PB_LED_OFF = 0,
    PB_LED_SOLID,
    PB_LED_BLINK,        // ~2.5 Hz
    PB_LED_BLINK_SLOW,   // ~0.5 Hz
    PB_LED_CODE,         // N short pulses, gap, repeat (count via pb_leds_set_code)
} pb_led_pattern_t;

// Configure the LED GPIOs (driven low) and start the driver task. Idempotent-
// guarded: a second call returns ESP_ERR_INVALID_STATE.
esp_err_t pb_leds_start(void);

// Set a LED's pattern (atomic; safe from any task).
void pb_leds_set(pb_led_id_t id, pb_led_pattern_t pattern);

// Set a LED to the CODE pattern blinking `pulses` short pulses per cycle.
void pb_leds_set_code(pb_led_id_t id, uint8_t pulses);

// Current logical pattern, including when dev-board HIL compiles GPIO out.
pb_led_pattern_t pb_leds_get(pb_led_id_t id);

// --- Status-LED master enable (NVS-backed, namespace app_nvs, default ON) -----
// When disabled the driver task leaves every panel LED physically off; pb_policy
// still computes patterns (pb_leds just no-ops the GPIO output). The pattern
// state is unaffected, so re-enabling resumes the current indication instantly.
//
// Load the persisted flag from NVS. MUST be called AFTER nvs_init(); before it,
// the flag defaults to ON (enabled). Value is coerced to a strict bool on load.
void pb_leds_load_config(void);
// Enable/disable the physical LED output; persists the flag to NVS. When
// disabling, the driver task drops the LEDs off within one 50 ms tick.
esp_err_t pb_leds_set_enabled(bool enabled);
// Current master-enable flag (true = LEDs are driven).
bool pb_leds_get_enabled(void);
