// SPDX-License-Identifier: MIT
#include "pb_policy.h"
#include "pb_heater.h"
#include "pb_fan.h"
#include "pb_ntc.h"
#include "pb_leds.h"

#include "esp_log.h"
#include <math.h>

static const char *TAG = "pb_policy";

// Post-print cooldown: after the heater has run this session, keep the fan going
// until the chamber falls below this threshold. NOT a temperature trigger on its
// own — it is gated by s_heated_this_session (see below).
#define PB_COOLDOWN_TEMP_C 40.0f

static uint8_t s_requested_fan;

// Set true the moment the heater enters heat mode this power cycle; this (not the
// chamber temperature) is what arms the post-print cooldown. Deliberately RAM-only
// / never persisted: a power cycle clears it, so a reboot-while-hot must NOT spin
// the fan. The fan only ever *continues* cooling after an actual heating session —
// it never auto-starts on temperature alone.
static bool s_heated_this_session;

esp_err_t pb_policy_init(void)
{
    s_requested_fan = 0;
    s_heated_this_session = false;
    ESP_LOGI(TAG, "init");
    return ESP_OK;
}

void pb_policy_apply(const pb_policy_input_t *in)
{
    if (!in) return;
    if (in->link_alive) pb_heater_notify_link_alive();
    pb_heater_set_target_c(in->chamber_target_c);
    s_requested_fan = in->fan_percent;
}

void pb_policy_tick(void)
{
    pb_heater_tick();

    bool heat = pb_heater_heat_mode();
    // Latch that the heater ran this session — this (not the chamber temp) arms
    // the post-print cooldown, so the fan never auto-starts on temperature alone.
    if (heat) s_heated_this_session = true;

    // Post-print cooldown: once the heater has run, keep airflow going after it
    // turns off until the chamber cools below the threshold, then disarm (don't
    // re-trigger until the next heating session). Only trust the smoothed temp if
    // the latest read was OK — on a sensor fault pb_ntc_smoothed_c() keeps
    // returning the last (stale) moving average, NOT NaN, which could otherwise
    // pin the fan on forever. On fault/no-reading, disarm; the heater's own fault
    // path (pb_heater_is_faulted) still drives airflow if it latches.
    bool cooldown = false;
    if (!heat && s_heated_this_session) {
        float chamber_c = pb_ntc_smoothed_c(PB_NTC_CHAMBER);
        if (pb_ntc_last_status(PB_NTC_CHAMBER) == PB_NTC_OK
                && !isnan(chamber_c) && chamber_c > PB_COOLDOWN_TEMP_C) {
            cooldown = true;
        } else {
            s_heated_this_session = false;
        }
    }

    // Ensure airflow while in heat mode (steady across the SSR's bang-bang
    // cycling — not pb_heater_is_on(), which chatters), while a fault is latched
    // (keep cooling a just-tripped over-temp chamber until the operator resets),
    // and during the post-print cooldown. Otherwise honor the requested fan level.
    bool want_airflow = heat || pb_heater_is_faulted() || cooldown;
    if (want_airflow && s_requested_fan < 30) {
        pb_fan_set_level(30);
    } else {
        pb_fan_set_level(s_requested_fan);
    }

    // Heating indication, matching the stock panel: solid while heating, blinking
    // on a latched safety fault (visible even while the fan keeps cooling a tripped
    // chamber), off otherwise. pb_policy owns all LED indication; pb_heater drives
    // no GPIO LED.
    //   - "Power" (GPIO21) is the stock heating indicator — driven only in release
    //     builds (CONFIG_PB_POWER_LED); in dev builds that pin is the console TX and
    //     pb_leds skips it, so this call is a harmless no-op there.
    //   - "On" (GPIO5) is driven the same way as an interim mode indicator until the
    //     Phase B mode state machine owns Auto/On/Dry.
    pb_led_pattern_t heat_pat = (pb_heater_is_faulted() || pb_heater_is_inhibited())
                                    ? PB_LED_BLINK
                                    : heat ? PB_LED_SOLID : PB_LED_OFF;
    pb_leds_set(PB_LED_POWER, heat_pat);
    pb_leds_set(PB_LED_ON, heat_pat);
}
