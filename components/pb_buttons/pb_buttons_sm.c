// SPDX-License-Identifier: MIT
#include "pb_buttons_sm.h"

// Fold the electrical level and the button's polarity into "is it pressed now".
static bool raw_is_pressed(const pb_btn_sm_t *b, int raw_level)
{
    return b->active_low ? (raw_level == 0) : (raw_level != 0);
}

void pb_buttons_sm_seed(pb_btn_sm_t *b, int raw_level)
{
    b->last_raw = raw_level;
    b->settle = 0;
    b->hold_ticks = 0;
    if (raw_is_pressed(b, raw_level)) {
        // Already down at startup: adopt it as a hold whose long-press has
        // already been consumed, so releasing it produces no spurious event.
        b->pressed = true;
        b->long_fired = true;
    } else {
        b->pressed = false;
        b->long_fired = false;
    }
}

pb_btn_event_t pb_buttons_sm_step(pb_btn_sm_t *b, int raw_level)
{
    // Debounce: a level must hold for PB_BTN_DEBOUNCE_TICKS samples before the
    // new state is accepted.
    if (raw_level != b->last_raw) {
        b->settle = PB_BTN_DEBOUNCE_TICKS;
        b->last_raw = raw_level;
        return PB_BTN_EV_NONE;
    }
    if (b->settle > 0) {
        b->settle--;
        if (b->settle > 0) return PB_BTN_EV_NONE;
    }

    bool pressed_now = raw_is_pressed(b, raw_level);

    if (pressed_now && !b->pressed) {
        b->pressed = true;
        b->hold_ticks = 0;
        b->long_fired = false;
    } else if (!pressed_now && b->pressed) {
        // Release. Suppress the short if the long already fired this hold.
        b->pressed = false;
        if (!b->long_fired) return PB_BTN_EV_SHORT;
    } else if (pressed_now) {
        b->hold_ticks++;
        if (!b->long_fired &&
            b->hold_ticks * PB_BTN_TICK_MS >= PB_BTN_LONG_PRESS_MS) {
            b->long_fired = true;
            return PB_BTN_EV_LONG;
        }
    }
    return PB_BTN_EV_NONE;
}
