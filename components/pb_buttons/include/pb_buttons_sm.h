// SPDX-License-Identifier: MIT
// pb_buttons_sm — the pure debounce + short/long-press state machine, with no
// dependency on FreeRTOS, ESP-IDF, or GPIO. It is fed one raw pin level per
// poll tick and returns at most one event. Split out from pb_buttons.c so the
// timing-sensitive logic is exercised directly by a host unit test.
#pragma once

#include <stdbool.h>

// Poll/timing constants. The driver task must poll at PB_BTN_TICK_MS.
#define PB_BTN_TICK_MS        10
#define PB_BTN_DEBOUNCE_TICKS  2      // 20 ms settle after a level change
#define PB_BTN_LONG_PRESS_MS   2000   // hold threshold for the long-press event

typedef enum {
    PB_BTN_EV_NONE = 0,
    PB_BTN_EV_SHORT,   // pressed and released before the long threshold
    PB_BTN_EV_LONG,    // held past the threshold (fires once, at the threshold)
} pb_btn_event_t;

// Per-button debounce state. Zero-initialize, then set active_low, then call
// pb_buttons_sm_seed() once with the first live sample before stepping.
typedef struct {
    bool active_low;      // true: electrical low == pressed (all Panda buttons)
    bool pressed;         // debounced logical state (true == actively held)
    int  hold_ticks;      // ticks since the debounced press started
    bool long_fired;      // long-press already dispatched this hold
    int  settle;          // ticks remaining before a level change is trusted
    int  last_raw;        // most recent raw sample (electrical level)
} pb_btn_sm_t;

// Seed the machine from the first live sample. If the button already reads
// pressed (a stuck line, or held through boot), it is treated as an in-progress
// hold whose long-press has already fired — so neither a SHORT nor a LONG is
// emitted until it physically releases. Nothing can fire on the seed tick.
void pb_buttons_sm_seed(pb_btn_sm_t *b, int raw_level);

// Advance one poll tick with a raw electrical level (0 or 1). Returns the event
// produced this tick, or PB_BTN_EV_NONE.
pb_btn_event_t pb_buttons_sm_step(pb_btn_sm_t *b, int raw_level);
