// SPDX-License-Identifier: MIT
// pb_board — single source of truth for Panda Breath V1.0.1 GPIO assignments.
//
// Hardware: BIGTREETECH Panda Breath V1.0.1, ESP32-C3-MINI-1-H4X, mains-powered
// PTC chamber heater + AC blower fan. Pinout reverse-engineered from the board
// and schematic (credit: Justin Hayes / klipper-esp32 RE). Values marked
// INFERRED should be continuity-tested before the first flash to real hardware.
#pragma once

#include "driver/gpio.h"
#include "hal/adc_types.h"

// -------- Heater (safety-critical) -----------------------------------------
// GPIO18 -> Q3 NPN -> MGR-GJ-5-L solid-state relay coil -> PTC heater AC switch.
// Simple on/off (the SSR is zero-cross DC-controlled; no phase control here).
#define PB_GPIO_RELAY        GPIO_NUM_18   // RLY_MOSFET (INFERRED, pad 26)

// -------- Fan (AC, phase-angle TRIAC) --------------------------------------
// GPIO3 gate -> MOC3021 random-phase opto-triac -> BT136-800E TRIAC -> FAN.
// Synced to the zero-cross detector (MB6S bridge + TLP785 opto) on GPIO7.
#define PB_GPIO_FAN_GATE     GPIO_NUM_3    // TRIAC gate (CONFIRMED, IO03)
#define PB_GPIO_ZERO_CROSS   GPIO_NUM_7    // ZCD input  (CONFIRMED, IO07)
                                           // NOTE: shared with K1 button in HW;
                                           // dedicated to the ZCD ISR here.

// -------- Thermistors (ADC1) ------------------------------------------------
// Both NTCs are read on ADC1 via adc_oneshot in the stock firmware.
#define PB_GPIO_NTC_CHAMBER  GPIO_NUM_0    // TH0 (INFERRED, pad 12)
#define PB_GPIO_NTC_PTC      GPIO_NUM_1    // TH1 (INFERRED, pad 13)
#define PB_ADC_UNIT          ADC_UNIT_1
#define PB_ADC_CH_CHAMBER    ADC_CHANNEL_0 // GPIO0 = ADC1_CH0
#define PB_ADC_CH_PTC        ADC_CHANNEL_1 // GPIO1 = ADC1_CH1

// -------- NTC divider strap -------------------------------------------------
// Read once at boot: selects the fixed series resistor value used to solve for
// the thermistor resistance (see pb_ntc). level 0 -> 82 kOhm, level 1 -> 33 kOhm.
#define PB_GPIO_RREF_STRAP   GPIO_NUM_19

// -------- Front-panel LEDs (active-high, direct push-pull; NOT a matrix) -----
// Confirmed by stock-firmware RE (byte-identical across stock 1.0.1-1.0.4).
// K1/K2/K3 are the three mode LEDs; the "Power" LED is on GPIO21 — which is also
// the UART0 console TX pin, so it is only driven when CONFIG_PB_POWER_LED is set
// (release builds); otherwise GPIO21 stays the serial console. See pb_leds.
#define PB_GPIO_LED_K1       GPIO_NUM_6    // "Auto"
#define PB_GPIO_LED_K2       GPIO_NUM_5    // "On"
#define PB_GPIO_LED_K3       GPIO_NUM_4    // "Dry"
#define PB_GPIO_LED_POWER    GPIO_NUM_21   // "Power" (shared with UART0-TX)

// -------- Buttons (not implemented in v0) -----------------------------------
#define PB_GPIO_BTN_K2       GPIO_NUM_0    // shared with TH0/chamber NTC
#define PB_GPIO_BTN_K3       GPIO_NUM_2

// -------- Console UART0 (CH340K USB-C bridge) -------------------------------
#define PB_GPIO_UART_TX      GPIO_NUM_21
#define PB_GPIO_UART_RX      GPIO_NUM_20

// Configure the LED pins as outputs (driven low). Safe to call once at boot.
// Does NOT touch the heater/fan pins — those are owned by pb_heater/pb_fan,
// which must bring them up in a known-OFF state themselves.
void pb_board_init(void);

// Reads the Rref strap (GPIO19). Returns 82 or 33 (kOhm). See pb_ntc.
int pb_board_rref_kohm(void);
