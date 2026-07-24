// SPDX-License-Identifier: MIT
// pb_buttons — the four front-panel buttons (Power/Auto/On/Dry), active-low with
// internal pull-ups, polled at 10 ms with debounce and short/long-press
// detection. This component is deliberately decoupled from heater/policy: it
// only emits events through a single callback. app_main wires that callback to
// pb_policy_on_button().
//
// A short press fires on release; a long press (2 s) fires once at the threshold
// and suppresses the trailing short. A button held at power-on (or a shorted
// line) is ignored until it releases — see pb_buttons_sm.h.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    PB_BUTTON_POWER = 0,   // GPIO9  (⚠ ROM download-mode strap)
    PB_BUTTON_AUTO,        // GPIO8  (⚠ strap)
    PB_BUTTON_ON,          // GPIO10 (no strap)
    PB_BUTTON_DRY,         // GPIO2  (⚠ strap)
    PB_BUTTON_COUNT,
} pb_button_id_t;

typedef enum {
    PB_BUTTON_SHORT,
    PB_BUTTON_LONG,
} pb_button_event_t;

typedef void (*pb_button_cb_t)(pb_button_id_t id, pb_button_event_t ev);

// Register the event callback and start the poll task. Idempotent-guarded: a
// second call returns ESP_ERR_INVALID_STATE. The callback runs on the button
// task — keep it short and non-blocking.
esp_err_t pb_buttons_start(pb_button_cb_t cb);

#ifdef CONFIG_PB_HIL_DEVBOARD
// HIL only: inject a raw electrical level (0/1) for a button, standing in for
// gpio_get_level() so scenarios exercise the real debounce/long-press timing.
void pb_buttons_hil_set_level(pb_button_id_t id, int level);
// Debounced logical state, reported back in the HIL state snapshot.
bool pb_buttons_hil_pressed(pb_button_id_t id);
#endif
