// SPDX-License-Identifier: MIT
#include "pb_heater.h"
#include "pb_board.h"
#include "pb_ntc.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

static const char *TAG = "pb_heater";

static float   s_target_c;
static bool    s_on;
static bool    s_latched_off;        // set by a safety trip; blocks heat until re-armed
static int64_t s_last_link_us;

static void ssr_set(bool on)
{
    gpio_set_level(PB_GPIO_RELAY, on ? 1 : 0);
    s_on = on;
}

esp_err_t pb_heater_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << PB_GPIO_RELAY),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;
    ssr_set(false);                  // guaranteed OFF before any request
    s_target_c = 0.0f;
    s_latched_off = false;
    s_last_link_us = esp_timer_get_time();
    ESP_LOGI(TAG, "init: SSR forced OFF");
    return ESP_OK;
}

void pb_heater_set_target_c(float target_c)
{
    if (target_c < 0.0f) target_c = 0.0f;
    if (target_c > PB_HEATER_MAX_TARGET_C) target_c = PB_HEATER_MAX_TARGET_C;
    s_target_c = target_c;
    if (target_c > 0.0f) s_latched_off = false;   // a new heat request re-arms
    ESP_LOGI(TAG, "target set to %.1f C", s_target_c);
}

float pb_heater_get_target_c(void) { return s_target_c; }

void pb_heater_notify_link_alive(void) { s_last_link_us = esp_timer_get_time(); }

void pb_heater_emergency_off(const char *reason)
{
    ssr_set(false);
    s_target_c = 0.0f;
    s_latched_off = true;
    ESP_LOGW(TAG, "EMERGENCY OFF: %s", reason ? reason : "(unspecified)");
}

bool pb_heater_is_on(void) { return s_on; }

void pb_heater_tick(void)
{
    // Heating-active indicator LED: steady ON while a chamber target is set and
    // not safety-tripped (i.e. "heat mode on"), regardless of the momentary SSR
    // cycling. TODO: confirm which physical button LED is the "heating" light
    // (K1=GPIO6, K2=GPIO5, K3=GPIO4) and remap if needed.
    gpio_set_level(PB_GPIO_LED_K1, (s_target_c > 0.0f && !s_latched_off) ? 1 : 0);

    // --- Safety cutoffs first, unconditionally ---
    float ptc_c = 0.0f, chamber_c = 0.0f;
    pb_ntc_status_t ps = pb_ntc_read(PB_NTC_PTC, &ptc_c);
    pb_ntc_status_t cs = pb_ntc_read(PB_NTC_CHAMBER, &chamber_c);

    if (ps == PB_NTC_OK && ptc_c >= PB_HEATER_PTC_CUTOFF_C) {
        pb_heater_emergency_off("PTC element over-temp");
        return;
    }
    if (cs == PB_NTC_OK && chamber_c >= PB_HEATER_CHAMBER_MAX_C) {
        pb_heater_emergency_off("chamber over-temp");
        return;
    }
    // A sensor fault while heating is a fail-closed condition.
    if (s_on && (cs == PB_NTC_OPEN || cs == PB_NTC_SHORT)) {
        pb_heater_emergency_off("chamber sensor fault");
        return;
    }
    // Comms-loss watchdog (only relevant while trying to heat).
    if (s_target_c > 0.0f &&
        (esp_timer_get_time() - s_last_link_us) > (int64_t)PB_HEATER_COMMS_TIMEOUT_MS * 1000) {
        pb_heater_emergency_off("controller link lost");
        return;
    }

    if (s_latched_off || s_target_c <= 0.0f || cs != PB_NTC_OK) {
        if (s_on) ssr_set(false);
        return;
    }

    // --- Bang-bang with hysteresis around the set-point ---
    if (!s_on && chamber_c < (s_target_c - PB_HEATER_HYSTERESIS_C)) {
        ssr_set(true);
    } else if (s_on && chamber_c >= s_target_c) {
        ssr_set(false);
    }
    // TODO: optional PID for tighter regulation; bang-bang is safe and adequate
    //       for a chamber given the PTC's slow thermal mass.
}
