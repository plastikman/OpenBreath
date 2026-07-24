// SPDX-License-Identifier: MIT
#include "pb_heater.h"
#include "pb_board.h"
#include "pb_ntc.h"

#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "pb_heater";

// Serializes fault-latch NVS writes (do_latch persist, clear_fault, tick retry)
// so a concurrent clear and a deferred-persist retry can't reorder into a stale
// write that resurrects a just-cleared latch. NULL until pb_heater_init().
static SemaphoreHandle_t s_persist_lock;

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
static pb_fault_reason_t s_fault_code; // guarded by s_mux (machine-readable, persisted)
static bool        s_persist_pending;  // guarded by s_mux — a latch persist not yet committed
                                       // to NVS; retried from pb_heater_tick until it lands
static int64_t     s_persist_retry_us; // guarded by s_mux — last persist-retry timestamp
static bool        s_on;            // written only by the control task; atomic read
static float       s_max_target_c;      // guarded by s_mux — settable set-point ceiling
static int64_t     s_comms_timeout_us;  // guarded by s_mux — comms deadman (microseconds)

// Persisted settings live in the shared app_nvs namespace (centi-°C / ms u32).
#define NVS_NS             "app_nvs"
#define KEY_HEAT_MAX_C     "heat_max_c"      // u32 centi-°C
#define KEY_HEAT_COMMS_MS  "heat_comms_ms"   // u32 ms
#define KEY_FAULT_LATCH    "fault_latch"     // u8 0/1 — persisted safety-fault latch
#define KEY_FAULT_CODE     "fault_code"      // u8 pb_fault_reason_t

// Canonical, stable strings for each fault code (index == code). Kept identical to
// the historical trip strings so the API contract doesn't change for existing
// causes. Any out-of-range/corrupt code maps to a generic latched-fault string.
static const char *const k_fault_str[PB_FAULT__COUNT] = {
    [PB_FAULT_NONE]             = "none",
    [PB_FAULT_PTC_OVERTEMP]     = "PTC element over-temp",
    [PB_FAULT_CHAMBER_OVERTEMP] = "chamber over-temp",
    [PB_FAULT_CHAMBER_SENSOR]   = "chamber sensor fault",
    [PB_FAULT_PTC_SENSOR]       = "PTC sensor fault",
    [PB_FAULT_LINK_LOST]        = "controller link lost",
    [PB_FAULT_PANIC_OFF]        = "panic-off",
    [PB_FAULT_INHIBITED]        = "inhibited",
    [PB_FAULT_EMERGENCY]        = "safety trip",
    [PB_FAULT_NVS_UNREADABLE]   = "persisted fault state unreadable",
};

const char *pb_heater_fault_str(pb_fault_reason_t code)
{
    if ((unsigned)code >= PB_FAULT__COUNT || !k_fault_str[code]) return "latched fault";
    return k_fault_str[code];
}
// pb_heater_fault_decide() is a pure header inline (see pb_heater.h) so the
// boot-time fail-safe logic can be host-tested without an NVS backend.

static float centi_to_c(uint32_t v) { return (float)v / 100.0f; }
static uint32_t c_to_centi(float c)
{
    if (c < 0.0f)   c = 0.0f;
    if (c > 200.0f) c = 200.0f;
    return (uint32_t)(c * 100.0f + 0.5f);
}

static void ssr_set(bool on)        // control-task context only
{
#ifndef CONFIG_PB_HIL_DEVBOARD
    gpio_set_level(PB_GPIO_RELAY, on ? 1 : 0);
#endif
    s_on = on;
}

esp_err_t pb_heater_init(void)
{
    if (!s_persist_lock) s_persist_lock = xSemaphoreCreateMutex();
#ifndef CONFIG_PB_HIL_DEVBOARD
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << PB_GPIO_RELAY),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;
#endif
    ssr_set(false);                  // guaranteed OFF before any request
    taskENTER_CRITICAL(&s_mux);
    s_target_c = 0.0f;
    s_latched_off = false;
    s_fault_reason = NULL;
    s_fault_code = PB_FAULT_NONE;
    s_last_link_us = esp_timer_get_time();
    // Conservative defaults ONLY — nvs isn't up yet; pb_heater_load_config()
    // applies persisted values later (called after nvs_init in app_main).
    s_max_target_c = PB_HEATER_MAX_TARGET_C_DEFAULT;
    s_comms_timeout_us = (int64_t)PB_HEATER_COMMS_TIMEOUT_MS_DEFAULT * 1000;
    taskEXIT_CRITICAL(&s_mux);
#ifdef CONFIG_PB_HIL_DEVBOARD
    ESP_LOGW(TAG, "HIL dev-board backend: relay GPIO compiled out");
#else
    ESP_LOGI(TAG, "init: SSR forced OFF");
#endif
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

// Persist (or clear) the safety-fault latch to NVS. NVS ops MUST run outside
// s_mux (they can block / need interrupts). On latch this is best-effort — RAM is
// authoritative for the session; on CLEAR the return matters so a failed persist
// can be surfaced (HTTP 500) rather than falsely reporting the fault gone.
static esp_err_t persist_fault(bool latched, pb_fault_reason_t code)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "fault persist: nvs_open %s", esp_err_to_name(err)); return err; }
    err = nvs_set_u8(h, KEY_FAULT_LATCH, latched ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_FAULT_CODE, (uint8_t)code);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "fault persist: %s", esp_err_to_name(err));
    return err;
}

// Latch the heater off with a machine-readable code + live reason string. Sets RAM
// under the lock, then (only on the false->true transition) persists it. persist==
// false leaves NVS untouched — used for the reboot-restore path (already in NVS)
// and for the permanent inhibit, whose "a reboot clears it" semantics must not
// survive a power cycle. drive_ssr is CONTROL-TASK ONLY (it writes the GPIO).
static void do_latch(pb_fault_reason_t code, const char *reason,
                     bool inhibit, bool drive_ssr, bool persist)
{
    if (drive_ssr) ssr_set(false);
    bool transition;
    taskENTER_CRITICAL(&s_mux);
    transition = !s_latched_off;                 // first latch of this episode?
    s_target_c = 0.0f;
    s_latched_off = true;
    if (inhibit) s_inhibited = true;
    s_fault_code = code;
    s_fault_reason = reason ? reason : pb_heater_fault_str(code);
    taskEXIT_CRITICAL(&s_mux);
    if (persist && transition) {
        if (s_persist_lock) xSemaphoreTake(s_persist_lock, portMAX_DELAY);
        esp_err_t pe = persist_fault(true, code);
        if (s_persist_lock) xSemaphoreGive(s_persist_lock);
        if (pe != ESP_OK) {
            // The write failed (NVS busy, or a fault before nvs_init on early boot).
            // Mark it pending so pb_heater_tick() keeps retrying until it commits —
            // otherwise a reboot would lose a latch that only ever lived in RAM.
            taskENTER_CRITICAL(&s_mux);
            s_persist_pending = true;
            taskEXIT_CRITICAL(&s_mux);
            ESP_LOGW(TAG, "fault persist deferred (will retry): %s", pb_heater_fault_str(code));
        }
    }
}

// Control-task safety trip: latch (driving the SSR down) + persist + log.
static void trip(pb_fault_reason_t code)
{
    do_latch(code, NULL, /*inhibit=*/false, /*drive_ssr=*/true, /*persist=*/true);
    ESP_LOGW(TAG, "EMERGENCY OFF: %s", pb_heater_fault_str(code));
}

void pb_heater_emergency_off(const char *reason)   // control-task context
{
    do_latch(PB_FAULT_EMERGENCY, reason, /*inhibit=*/false, /*drive_ssr=*/true, /*persist=*/true);
    ESP_LOGW(TAG, "EMERGENCY OFF: %s", reason ? reason : "(unspecified)");
}

void pb_heater_request_panic_off(const char *reason)   // any task
{
    // Latch only — NO GPIO write. Unlike pb_heater_emergency_off(), this is safe
    // to call off the control task (e.g. the button task): it preserves the
    // single-SSR-writer invariant by leaving the actual ssr_set(false) to the
    // next pb_heater_tick() on the control task. Callers that need the SSR down
    // fast should wake the control task immediately after this returns.
    // persist=false: a user panic-off is a manual stop, documented to clear on
    // reboot (only hazard-driven trips — over-temp/sensor/comms — persist).
    do_latch(PB_FAULT_PANIC_OFF, reason, /*inhibit=*/false, /*drive_ssr=*/false, /*persist=*/false);
}

esp_err_t pb_heater_clear_fault(void)
{
    bool was;
    taskENTER_CRITICAL(&s_mux);
    // A permanent inhibit is NOT clearable — leave everything latched.
    if (s_inhibited) {
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "clear ignored: heater permanently inhibited (reboot required)");
        return ESP_ERR_INVALID_STATE;
    }
    was = s_latched_off;
    taskEXIT_CRITICAL(&s_mux);

    // Persist-first, under the persist lock so a concurrent tick retry can't write
    // a stale "latched" after this clears NVS. If the persist fails, leave the fault
    // latched (fail-safe) and report it — the heater stays off rather than falsely
    // appearing cleared but returning on the next reboot.
    if (s_persist_lock) xSemaphoreTake(s_persist_lock, portMAX_DELAY);
    esp_err_t err = persist_fault(false, PB_FAULT_NONE);
    if (err != ESP_OK) {
        if (s_persist_lock) xSemaphoreGive(s_persist_lock);
        ESP_LOGE(TAG, "clear rejected: NVS clear failed (%s) — fault stays latched", esp_err_to_name(err));
        return err;
    }
    taskENTER_CRITICAL(&s_mux);
    s_latched_off = false;
    s_target_c = 0.0f;              // reset leaves the target at ZERO — a fresh
    s_fault_reason = NULL;          // target command is required to resume heating.
    s_fault_code = PB_FAULT_NONE;
    s_persist_pending = false;      // NVS now holds "not latched"; nothing to retry
    taskEXIT_CRITICAL(&s_mux);
    if (s_persist_lock) xSemaphoreGive(s_persist_lock);
    if (was) ESP_LOGW(TAG, "fault latch cleared; target reset to 0 (send a fresh target to resume)");
    return ESP_OK;
}

void pb_heater_inhibit(const char *reason)
{
    // Permanent (reboot-only) -> persist=false: a power cycle must lift it, so
    // persisting could brick the device across reboots on a transient init failure.
    // Any clearable latch already in NVS is left intact.
    do_latch(PB_FAULT_INHIBITED, reason, /*inhibit=*/true, /*drive_ssr=*/true, /*persist=*/false);
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

pb_fault_reason_t pb_heater_fault_code(void)
{
    taskENTER_CRITICAL(&s_mux);
    pb_fault_reason_t c = s_fault_code;
    taskEXIT_CRITICAL(&s_mux);
    return c;
}

void pb_heater_load_fault(void)
{
    nvs_handle_t h;
    esp_err_t oe = nvs_open(NVS_NS, NVS_READONLY, &h);
    bool ns_not_found = (oe == ESP_ERR_NVS_NOT_FOUND);
    bool open_ok      = (oe == ESP_OK);
    uint8_t latch_val = 0, code_val = PB_FAULT_NONE;
    bool latch_read_ok = false, latch_not_found = false;
    if (open_ok) {
        esp_err_t le = nvs_get_u8(h, KEY_FAULT_LATCH, &latch_val);
        latch_not_found = (le == ESP_ERR_NVS_NOT_FOUND);
        latch_read_ok   = (le == ESP_OK);
        nvs_get_u8(h, KEY_FAULT_CODE, &code_val);   // best-effort; decide() range-checks
        nvs_close(h);
    }
    pb_fault_reason_t code;
    bool latched = pb_heater_fault_decide(open_ok, ns_not_found, latch_read_ok,
                                          latch_not_found, latch_val, code_val, &code);
    if (latched) {
        // persist=false (already in NVS, or unreadable); drive_ssr=false (init()
        // already forced the SSR off and the control task is not running yet).
        do_latch(code, NULL, /*inhibit=*/false, /*drive_ssr=*/false, /*persist=*/false);
        ESP_LOGW(TAG, "boot: restored latched fault: %s", pb_heater_fault_str(code));
    }
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
    // Retry a deferred fault-latch persist until it commits, so a latch that
    // couldn't be written when it happened (NVS busy, or a fault before nvs_init)
    // still survives a reboot. Throttled and serialized with clear_fault via the
    // persist lock; re-reads the latch state INSIDE the lock so a clear that landed
    // meanwhile is never overwritten with a stale "latched".
    bool pending;
    taskENTER_CRITICAL(&s_mux);
    pending = s_persist_pending;
    taskEXIT_CRITICAL(&s_mux);
    if (pending) {
        int64_t now = esp_timer_get_time();
        taskENTER_CRITICAL(&s_mux);
        bool due = (now - s_persist_retry_us) >= 2000000;   // ~2 s: bound NVS churn/log spam
        taskEXIT_CRITICAL(&s_mux);
        if (due && s_persist_lock && xSemaphoreTake(s_persist_lock, 0) == pdTRUE) {
            taskENTER_CRITICAL(&s_mux);
            bool still = s_latched_off;
            pb_fault_reason_t code = s_fault_code;
            s_persist_retry_us = now;
            taskEXIT_CRITICAL(&s_mux);
            esp_err_t pr = still ? persist_fault(true, code) : ESP_OK;  // cleared -> nothing to write
            if (pr == ESP_OK) {
                taskENTER_CRITICAL(&s_mux);
                s_persist_pending = false;
                taskEXIT_CRITICAL(&s_mux);
                ESP_LOGI(TAG, "deferred fault persist committed");
            }
            xSemaphoreGive(s_persist_lock);
        }
    }

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
        trip(PB_FAULT_PTC_OVERTEMP);
        return;
    }
    if (cs == PB_NTC_OK && chamber_c >= PB_HEATER_CHAMBER_MAX_C) {
        trip(PB_FAULT_CHAMBER_OVERTEMP);
        return;
    }
    // While armed, EITHER thermistor being anything other than OK — OPEN, SHORT,
    // or a UNINIT read error — is fail-closed. A blind heater (dead chamber
    // sensor) or an unmonitored element (dead PTC sensor) must latch off.
    if (armed && cs != PB_NTC_OK) {
        trip(PB_FAULT_CHAMBER_SENSOR);
        return;
    }
    if (armed && ps != PB_NTC_OK) {
        trip(PB_FAULT_PTC_SENSOR);
        return;
    }
    // Comms-loss watchdog (only while trying to heat).
    if (armed && (esp_timer_get_time() - last_link) > comms_timeout_us) {
        trip(PB_FAULT_LINK_LOST);
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
