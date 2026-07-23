// SPDX-License-Identifier: MIT
#include "pb_heater.h"
#include "pb_board.h"
#include "pb_ntc.h"

#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

static const char *TAG = "pb_heater";

// Heater state is touched by BOTH the control task (pb_heater_tick, via the
// 2 Hz loop) and the HTTP task (set_target / notify_link_alive / clear_fault /
// getters). All of it is serialized under this spinlock — in particular the
// 64-bit s_last_link_us would tear on the 32-bit RISC-V core otherwise.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static float       s_target_c;      // guarded by s_mux
static bool        s_latched_off;   // guarded by s_mux (set by a safety trip)
static bool        s_inhibited;     // guarded by s_mux (PERMANENT; reboot-only)
static int64_t     s_last_link_us;  // guarded by s_mux
static const char *s_fault_reason;  // guarded by s_mux (points at a string literal)
static bool        s_on;            // written only by the control task; atomic read
static float       s_max_target_c;      // guarded by s_mux — settable set-point ceiling
static int64_t     s_comms_timeout_us;  // guarded by s_mux — comms deadman (microseconds)

// Persisted settings live in the shared app_nvs namespace (centi-°C / ms u32).
#define NVS_NS             "app_nvs"
#define KEY_HEAT_MAX_C     "heat_max_c"      // u32 centi-°C
#define KEY_HEAT_COMMS_MS  "heat_comms_ms"   // u32 ms

static float centi_to_c(uint32_t v) { return (float)v / 100.0f; }
static uint32_t c_to_centi(float c)
{
    if (c < 0.0f)   c = 0.0f;
    if (c > 200.0f) c = 200.0f;
    return (uint32_t)(c * 100.0f + 0.5f);
}

static void ssr_set(bool on)        // control-task context only
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
    taskENTER_CRITICAL(&s_mux);
    s_target_c = 0.0f;
    s_latched_off = false;
    s_fault_reason = NULL;
    s_last_link_us = esp_timer_get_time();
    // Conservative defaults ONLY — nvs isn't up yet; pb_heater_load_config()
    // applies persisted values later (called after nvs_init in app_main).
    s_max_target_c = PB_HEATER_MAX_TARGET_C_DEFAULT;
    s_comms_timeout_us = (int64_t)PB_HEATER_COMMS_TIMEOUT_MS_DEFAULT * 1000;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "init: SSR forced OFF");
    return ESP_OK;
}

esp_err_t pb_heater_set_target_c(float target_c)
{
    if (!isfinite(target_c)) {           // reject NaN / +-Inf defensively
        ESP_LOGW(TAG, "rejected non-finite target");
        return ESP_ERR_INVALID_ARG;
    }
    if (target_c < 0.0f) target_c = 0.0f;

    esp_err_t r = ESP_OK;
    taskENTER_CRITICAL(&s_mux);
    if (target_c > s_max_target_c) target_c = s_max_target_c;   // clamp to the live ceiling
    if ((s_latched_off || s_inhibited) && target_c > 0.0f) {
        r = ESP_ERR_INVALID_STATE;       // never queue heat behind a fault/inhibit
    } else {
        s_target_c = target_c;
    }
    taskEXIT_CRITICAL(&s_mux);

    if (r == ESP_OK)
        ESP_LOGI(TAG, "target set to %.1f C", target_c);
    else
        ESP_LOGW(TAG, "target %.1f rejected: fault latched (clear fault, then issue a fresh command)", target_c);
    return r;
}

float pb_heater_get_target_c(void)
{
    taskENTER_CRITICAL(&s_mux);
    float t = s_target_c;
    taskEXIT_CRITICAL(&s_mux);
    return t;
}

void pb_heater_load_config(void)
{
    // Read NVS OUTSIDE the lock (it can block), then clamp + assign under s_mux.
    float    max_c    = PB_HEATER_MAX_TARGET_C_DEFAULT;
    uint32_t comms_ms = PB_HEATER_COMMS_TIMEOUT_MS_DEFAULT;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t v;
        if (nvs_get_u32(h, KEY_HEAT_MAX_C, &v) == ESP_OK)    max_c    = centi_to_c(v);
        if (nvs_get_u32(h, KEY_HEAT_COMMS_MS, &v) == ESP_OK) comms_ms = v;
        nvs_close(h);
    }
    // Clamp to the safe envelope regardless of what NVS held (defends against a
    // corrupted/hand-edited store — the ceiling can never exceed the ABS max).
    if (max_c < PB_HEATER_MIN_TARGET_C)     max_c = PB_HEATER_MIN_TARGET_C;
    if (max_c > PB_HEATER_ABS_MAX_TARGET_C) max_c = PB_HEATER_ABS_MAX_TARGET_C;
    if (comms_ms < PB_HEATER_COMMS_TIMEOUT_MS_MIN) comms_ms = PB_HEATER_COMMS_TIMEOUT_MS_MIN;
    if (comms_ms > PB_HEATER_COMMS_TIMEOUT_MS_MAX) comms_ms = PB_HEATER_COMMS_TIMEOUT_MS_MAX;
    taskENTER_CRITICAL(&s_mux);
    s_max_target_c = max_c;
    s_comms_timeout_us = (int64_t)comms_ms * 1000;
    if (s_target_c > s_max_target_c) s_target_c = s_max_target_c;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "config: max_target=%.1fC comms_timeout=%ums", max_c, (unsigned)comms_ms);
}

esp_err_t pb_heater_set_max_target_c(float max_c)
{
    if (!isfinite(max_c)) return ESP_ERR_INVALID_ARG;
    if (max_c < PB_HEATER_MIN_TARGET_C)     max_c = PB_HEATER_MIN_TARGET_C;
    if (max_c > PB_HEATER_ABS_MAX_TARGET_C) max_c = PB_HEATER_ABS_MAX_TARGET_C;   // never past 70
    taskENTER_CRITICAL(&s_mux);
    s_max_target_c = max_c;
    if (s_target_c > s_max_target_c) s_target_c = s_max_target_c;   // pull a live target down
    taskEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;                                                  // persist outside the lock
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, KEY_HEAT_MAX_C, c_to_centi(max_c));
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "max_target set to %.1f C", max_c);
    return ESP_OK;
}

float pb_heater_get_max_target_c(void)
{
    taskENTER_CRITICAL(&s_mux);
    float m = s_max_target_c;
    taskEXIT_CRITICAL(&s_mux);
    return m;
}

esp_err_t pb_heater_set_comms_timeout_ms(uint32_t ms)
{
    if (ms < PB_HEATER_COMMS_TIMEOUT_MS_MIN) ms = PB_HEATER_COMMS_TIMEOUT_MS_MIN;
    if (ms > PB_HEATER_COMMS_TIMEOUT_MS_MAX) ms = PB_HEATER_COMMS_TIMEOUT_MS_MAX;  // never > 5 min
    taskENTER_CRITICAL(&s_mux);
    s_comms_timeout_us = (int64_t)ms * 1000;
    taskEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, KEY_HEAT_COMMS_MS, ms);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "comms_timeout set to %u ms", (unsigned)ms);
    return ESP_OK;
}

uint32_t pb_heater_get_comms_timeout_ms(void)
{
    taskENTER_CRITICAL(&s_mux);
    int64_t us = s_comms_timeout_us;
    taskEXIT_CRITICAL(&s_mux);
    return (uint32_t)(us / 1000);
}

void pb_heater_notify_link_alive(void)
{
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    s_last_link_us = now;
    taskEXIT_CRITICAL(&s_mux);
}

void pb_heater_emergency_off(const char *reason)   // control-task context
{
    ssr_set(false);
    taskENTER_CRITICAL(&s_mux);
    s_target_c = 0.0f;
    s_latched_off = true;
    s_fault_reason = reason ? reason : "unspecified";
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "EMERGENCY OFF: %s", reason ? reason : "(unspecified)");
}

void pb_heater_clear_fault(void)
{
    taskENTER_CRITICAL(&s_mux);
    // A permanent inhibit is NOT clearable — leave everything latched.
    if (s_inhibited) {
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "clear ignored: heater permanently inhibited (reboot required)");
        return;
    }
    bool was = s_latched_off;
    s_latched_off = false;
    s_target_c = 0.0f;          // reset leaves the target at ZERO — a fresh
    s_fault_reason = NULL;      // target command is required to resume heating.
    taskEXIT_CRITICAL(&s_mux);
    if (was) ESP_LOGW(TAG, "fault latch cleared; target reset to 0 (send a fresh target to resume)");
}

void pb_heater_inhibit(const char *reason)
{
    ssr_set(false);
    taskENTER_CRITICAL(&s_mux);
    s_inhibited = true;
    s_latched_off = true;
    s_target_c = 0.0f;
    s_fault_reason = reason ? reason : "inhibited";
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGE(TAG, "HEATER INHIBITED (reboot-only): %s", reason ? reason : "(unspecified)");
}

bool pb_heater_is_inhibited(void) { return s_inhibited; }   // atomic bool read
bool pb_heater_is_faulted(void) { return s_latched_off || s_inhibited; }
bool pb_heater_is_on(void) { return s_on; }                 // atomic bool read

const char *pb_heater_fault_reason(void)
{
    taskENTER_CRITICAL(&s_mux);
    const char *r = s_fault_reason;
    taskEXIT_CRITICAL(&s_mux);
    return r;
}

bool pb_heater_heat_mode(void)     // "armed and not tripped" (independent of SSR cycling)
{
    taskENTER_CRITICAL(&s_mux);
    bool m = (s_target_c > 0.0f && !s_latched_off);
    taskEXIT_CRITICAL(&s_mux);
    return m;
}

void pb_heater_tick(void)          // control-task context; sole writer of s_on
{
    float   target;
    bool    latched;
    int64_t last_link;
    int64_t comms_timeout_us;
    taskENTER_CRITICAL(&s_mux);
    target = s_target_c;
    latched = s_latched_off;
    last_link = s_last_link_us;
    comms_timeout_us = s_comms_timeout_us;
    taskEXIT_CRITICAL(&s_mux);

    const bool armed = (target > 0.0f);
    // LED indication is owned by pb_leds (driven from pb_policy) — pb_heater no
    // longer touches any LED.

    float ptc_c = 0.0f, chamber_c = 0.0f;
    pb_ntc_status_t ps = pb_ntc_read(PB_NTC_PTC, &ptc_c);
    pb_ntc_status_t cs = pb_ntc_read(PB_NTC_CHAMBER, &chamber_c);

    // Over-temp cutoffs — unconditional whenever the sensor reads valid.
    if (ps == PB_NTC_OK && ptc_c >= PB_HEATER_PTC_CUTOFF_C) {
        pb_heater_emergency_off("PTC element over-temp");
        return;
    }
    if (cs == PB_NTC_OK && chamber_c >= PB_HEATER_CHAMBER_MAX_C) {
        pb_heater_emergency_off("chamber over-temp");
        return;
    }
    // While armed, EITHER thermistor being anything other than OK — OPEN, SHORT,
    // or a UNINIT read error — is fail-closed. A blind heater (dead chamber
    // sensor) or an unmonitored element (dead PTC sensor) must latch off.
    if (armed && cs != PB_NTC_OK) {
        pb_heater_emergency_off("chamber sensor fault");
        return;
    }
    if (armed && ps != PB_NTC_OK) {
        pb_heater_emergency_off("PTC sensor fault");
        return;
    }
    // Comms-loss watchdog (only while trying to heat).
    if (armed && (esp_timer_get_time() - last_link) > comms_timeout_us) {
        pb_heater_emergency_off("controller link lost");
        return;
    }

    if (latched || !armed) {
        if (s_on) ssr_set(false);
        return;
    }

    // Here: armed && !latched && cs==OK && ps==OK — bang-bang with hysteresis.
    if (!s_on && chamber_c < (target - PB_HEATER_HYSTERESIS_C)) {
        ssr_set(true);
    } else if (s_on && chamber_c >= target) {
        ssr_set(false);
    }
}
