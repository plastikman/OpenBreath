// SPDX-License-Identifier: MIT
#include "pb_policy.h"

#include "pb_fan.h"
#include "pb_heater.h"
#include "pb_ntc.h"
#include "pb_leds.h"
#include "pb_buttons.h"
#include "pv_evlog.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "pb_policy";

#define PB_COOLDOWN_TEMP_C          40.0f
#define PB_AUTO_BED_HYSTERESIS_C     3.0f
#define PB_DRYING_MAX_HOURS         12U
#define PB_MIN_MODE_TARGET_C        30.0f

// --- Remembered mode parameters (the only reboot-surviving policy state) -----
#define PB_NVS_NAMESPACE            "app_nvs"
#define PB_NVS_KEY_MANUAL           "md_last"
#define PB_NVS_KEY_AUTO_TGT         "md_auto_tgt"
#define PB_NVS_KEY_AUTO_BED         "md_auto_bed"
#define PB_NVS_KEY_DRY_TGT          "md_dry_tgt"
#define PB_NVS_KEY_DRY_HRS          "md_dry_hrs"

#define PB_AUTO_BED_MIN_C           40.0f
#define PB_AUTO_BED_MAX_C          120.0f

#define PB_DEFAULT_MANUAL_TARGET_C  50.0f
#define PB_DEFAULT_AUTO_TARGET_C    60.0f
#define PB_DEFAULT_AUTO_BED_C      100.0f
#define PB_DEFAULT_DRY_TARGET_C     60.0f
#define PB_DEFAULT_DRY_HOURS        12U

typedef struct {
    pb_mode_t mode;
    pb_source_t source;
    uint32_t revision;

    float requested_target_c;
    uint8_t requested_fan_percent;

    bool mk_connected;
    float bed_c;
    float auto_bed_threshold_c;
    bool auto_engaged;

    int64_t drying_deadline_us;
    int64_t local_power_deadline_us;

    bool lease_active;
    pb_policy_lease_t lease;
    char lease_owner[PB_POLICY_OWNER_LEN + 1];
    int64_t lease_deadline_us;

    bool heated_this_session;
    bool last_faulted;

    pb_policy_params_t params;   // remembered mode parameters
    bool params_dirty;           // params differ from what is on flash
} policy_state_t;

static SemaphoreHandle_t s_lock;
static policy_state_t s;

// Owned exclusively by the persistence worker: the last values successfully
// committed, so only genuinely changed keys are rewritten (flash wear).
static pb_policy_params_t s_written;
static bool s_written_valid;
static TaskHandle_t s_persist_task;

static pb_policy_wake_fn s_wake_cb;

void pb_policy_set_wake_cb(pb_policy_wake_fn fn) { s_wake_cb = fn; }

static void wake_control_task(void)
{
    if (s_wake_cb) s_wake_cb();
}

static bool source_is_remote(pb_source_t source)
{
    return source == PB_SOURCE_WEB || source == PB_SOURCE_KLIPPER;
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void revision_advance_locked(pb_source_t source)
{
    // Reserve zero as "no snapshot received".  Natural uint32 rollover remains
    // valid; skip zero so clients never confuse it with an uninitialized value.
    s.revision++;
    if (s.revision == 0) s.revision = 1;
    s.source = source;
}

static void lease_invalidate_locked(void)
{
    s.lease_active = false;
    memset(&s.lease, 0, sizeof s.lease);
    s.lease_owner[0] = '\0';
    s.lease_deadline_us = 0;
}

static int64_t remote_lease_ttl_us(void)
{
    return (int64_t)pb_heater_get_comms_timeout_ms() * 1000;
}

static void lease_issue_locked(const char *owner, int64_t now_us,
                               pb_policy_lease_t *out)
{
    uint8_t random[PB_POLICY_LEASE_ID_LEN / 2];
    esp_fill_random(random, sizeof random);
    for (size_t i = 0; i < sizeof random; ++i) {
        snprintf(&s.lease.id[i * 2], 3, "%02x", random[i]);
    }
    s.lease.id[PB_POLICY_LEASE_ID_LEN] = '\0';
    copy_text(s.lease_owner, sizeof s.lease_owner,
              owner && owner[0] ? owner : "remote");
    s.lease_deadline_us = now_us + remote_lease_ttl_us();
    s.lease_active = true;
    if (out) *out = s.lease;
}

static bool revision_matches_locked(uint32_t expected)
{
    return expected == PB_POLICY_REVISION_ANY || expected == s.revision;
}

static pb_policy_result_t heat_precheck_locked(float target_c,
                                                uint32_t expected_revision)
{
    if (!revision_matches_locked(expected_revision))
        return PB_POLICY_REVISION_CONFLICT;
    if (!isfinite(target_c) || target_c < PB_MIN_MODE_TARGET_C)
        return PB_POLICY_INVALID;
    if (pb_heater_is_inhibited())
        return PB_POLICY_INHIBITED;
    if (pb_heater_is_faulted())
        return PB_POLICY_FAULT_LATCHED;
    return PB_POLICY_OK;
}

static void set_off_locked(pb_source_t source)
{
    // Target zero is accepted even while faulted/inhibited.
    (void)pb_heater_set_target_c(0.0f);
    s.mode = PB_MODE_OFF;
    s.requested_target_c = 0.0f;
    s.auto_engaged = false;
    s.drying_deadline_us = 0;
    s.local_power_deadline_us = 0;
    lease_invalidate_locked();
    revision_advance_locked(source);
}

// --- Remembered mode parameters ---------------------------------------------
// Parameters are the ONLY policy state that survives a reboot. Mode, target,
// deadlines, and leases deliberately do not: the device always boots OFF.

static uint32_t c_to_centi(float c) { return (uint32_t)lroundf(c * 100.0f); }
static float centi_to_c(uint32_t centi) { return (float)centi / 100.0f; }

// Missing values retain the defaults installed by pb_policy_init(). Stored
// temperatures are clamped to their safe envelope; non-finite values fall back
// to the default defensively.
static float clamp_or_default(float v, float lo, float hi, float dflt)
{
    if (!isfinite(v)) v = dflt;
    return v < lo ? lo : (v > hi ? hi : v);
}

static void params_clamp(pb_policy_params_t *p)
{
    // pb_heater_load_config() runs before pb_policy_load_params(), so use the
    // live configured ceiling here. This keeps remembered/UI-prefill values
    // inside a user-lowered maximum immediately after boot, not merely when a
    // later command is submitted.
    float max_target_c = clamp_or_default(
        pb_heater_get_max_target_c(), PB_MIN_MODE_TARGET_C,
        PB_HEATER_ABS_MAX_TARGET_C, PB_HEATER_ABS_MAX_TARGET_C);
    p->manual_target_c = clamp_or_default(
        p->manual_target_c, PB_MIN_MODE_TARGET_C,
        max_target_c, PB_DEFAULT_MANUAL_TARGET_C);
    p->auto_target_c = clamp_or_default(
        p->auto_target_c, PB_MIN_MODE_TARGET_C,
        max_target_c, PB_DEFAULT_AUTO_TARGET_C);
    p->auto_bed_threshold_c = clamp_or_default(
        p->auto_bed_threshold_c, PB_AUTO_BED_MIN_C,
        PB_AUTO_BED_MAX_C, PB_DEFAULT_AUTO_BED_C);
    p->dry_target_c = clamp_or_default(
        p->dry_target_c, PB_MIN_MODE_TARGET_C,
        max_target_c, PB_DEFAULT_DRY_TARGET_C);
    if (p->dry_hours == 0 || p->dry_hours > PB_DRYING_MAX_HOURS)
        p->dry_hours = PB_DEFAULT_DRY_HOURS;
}

static void params_defaults_locked(void)
{
    s.params.manual_target_c      = PB_DEFAULT_MANUAL_TARGET_C;
    s.params.auto_target_c        = PB_DEFAULT_AUTO_TARGET_C;
    s.params.auto_bed_threshold_c = PB_DEFAULT_AUTO_BED_C;
    s.params.dry_target_c         = PB_DEFAULT_DRY_TARGET_C;
    s.params.dry_hours            = PB_DEFAULT_DRY_HOURS;
}

// Wake the persistence worker. Call AFTER releasing s_lock: NVS writes must
// never happen under the control mutex.
static void params_notify(void)
{
    if (s_persist_task) xTaskNotifyGive(s_persist_task);
}

// Only the persistence worker (or the host test) reaches this, so writes are
// serialized by construction: there is exactly one writer, and it always writes
// the latest canonical snapshot. Concurrent HTTP/button commands therefore
// cannot land on flash out of order.
static esp_err_t persist_params(const pb_policy_params_t *p)
{
    const struct { const char *key; uint32_t val, prev; } kv[] = {
        { PB_NVS_KEY_MANUAL,   c_to_centi(p->manual_target_c),
                               c_to_centi(s_written.manual_target_c) },
        { PB_NVS_KEY_AUTO_TGT, c_to_centi(p->auto_target_c),
                               c_to_centi(s_written.auto_target_c) },
        { PB_NVS_KEY_AUTO_BED, c_to_centi(p->auto_bed_threshold_c),
                               c_to_centi(s_written.auto_bed_threshold_c) },
        { PB_NVS_KEY_DRY_TGT,  c_to_centi(p->dry_target_c),
                               c_to_centi(s_written.dry_target_c) },
        { PB_NVS_KEY_DRY_HRS,  p->dry_hours, s_written.dry_hours },
    };
    const size_t n = sizeof kv / sizeof kv[0];

    bool any = !s_written_valid;
    for (size_t i = 0; !any && i < n; ++i)
        if (kv[i].val != kv[i].prev) any = true;
    if (!any) return ESP_OK;   // nothing actually changed -- spare the flash

    nvs_handle_t h;
    esp_err_t err = nvs_open(PB_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    for (size_t i = 0; i < n; ++i) {
        if (s_written_valid && kv[i].val == kv[i].prev) continue;
        esp_err_t e = nvs_set_u32(h, kv[i].key, kv[i].val);
        if (e != ESP_OK && err == ESP_OK) err = e;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        s_written = *p;
        s_written_valid = true;
    }
    return err;
}

bool pb_policy_persist_pending(void)
{
    if (!s_lock) return false;

    pb_policy_params_t p;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool dirty = s.params_dirty;
    p = s.params;                 // canonical post-clamp values, never raw input
    if (dirty) s.params_dirty = false;
    xSemaphoreGive(s_lock);
    if (!dirty) return false;

    esp_err_t err = persist_params(&p);
    if (err != ESP_OK) {
        // Stay dirty so the next accepted command retries. A command that landed
        // while we were writing may re-dirty this too; an extra no-op pass is
        // harmless.
        ESP_LOGE(TAG, "params persist failed (%d); will retry on next change",
                 (int)err);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s.params_dirty = true;
        xSemaphoreGive(s_lock);
    }
    return true;
}

static void persist_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        pb_policy_persist_pending();
    }
}

void pb_policy_load_params(void)
{
    if (!s_lock) return;

    pb_policy_params_t p;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    p = s.params;                 // defaults installed by pb_policy_init()
    xSemaphoreGive(s_lock);

    nvs_handle_t h;
    if (nvs_open(PB_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint32_t v;
        if (nvs_get_u32(h, PB_NVS_KEY_MANUAL, &v) == ESP_OK)
            p.manual_target_c = centi_to_c(v);
        if (nvs_get_u32(h, PB_NVS_KEY_AUTO_TGT, &v) == ESP_OK)
            p.auto_target_c = centi_to_c(v);
        if (nvs_get_u32(h, PB_NVS_KEY_AUTO_BED, &v) == ESP_OK)
            p.auto_bed_threshold_c = centi_to_c(v);
        if (nvs_get_u32(h, PB_NVS_KEY_DRY_TGT, &v) == ESP_OK)
            p.dry_target_c = centi_to_c(v);
        if (nvs_get_u32(h, PB_NVS_KEY_DRY_HRS, &v) == ESP_OK)
            p.dry_hours = v > UINT8_MAX ? UINT8_MAX : (uint8_t)v;
        nvs_close(h);
    }
    params_clamp(&p);

    // Parameters only. Mode, target, deadlines, and leases are untouched, so
    // this cannot arm heat -- the device stays OFF exactly as init left it.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.params = p;
    s.params_dirty = false;
    xSemaphoreGive(s_lock);

    s_written = p;
    s_written_valid = true;

    if (!s_persist_task &&
        xTaskCreate(persist_task, "pb_pol_persist", 2560, NULL, 2,
                    &s_persist_task) != pdPASS) {
        s_persist_task = NULL;
        ESP_LOGE(TAG, "persistence worker did not start; params stay RAM-only");
    }
    ESP_LOGI(TAG,
        "params loaded: manual=%.1fC auto=%.1fC/bed%.1fC dry=%.1fC/%uh",
        p.manual_target_c, p.auto_target_c, p.auto_bed_threshold_c,
        p.dry_target_c, (unsigned)p.dry_hours);
}

void pb_policy_get_params(pb_policy_params_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s.params;
    xSemaphoreGive(s_lock);
}

esp_err_t pb_policy_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s, 0, sizeof s);
    s.mode = PB_MODE_OFF;
    s.source = PB_SOURCE_BOOT;
    s.revision = 1;
    s.auto_bed_threshold_c = PB_DEFAULT_AUTO_BED_C;
    s.last_faulted = pb_heater_is_faulted();
    params_defaults_locked();
    xSemaphoreGive(s_lock);
    s_written_valid = false;   // nothing known about flash until load_params()

    // The heater was forced off before policy initialization.  Do not restore
    // mode, target, deadlines, or leases from storage.
    (void)pb_heater_set_target_c(0.0f);
    ESP_LOGI(TAG, "authoritative state initialized: OFF revision=1");
    return ESP_OK;
}

pb_policy_result_t pb_policy_set_power_on(
    float target_c,
    pb_source_t source,
    const char *owner,
    uint32_t expected_revision,
    pb_policy_lease_t *lease_out)
{
    if (!s_lock) return PB_POLICY_INHIBITED;
    if (lease_out) memset(lease_out, 0, sizeof *lease_out);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    pb_policy_result_t r = heat_precheck_locked(target_c, expected_revision);
    if (r != PB_POLICY_OK) {
        xSemaphoreGive(s_lock);
        return r;
    }

    esp_err_t hr = pb_heater_set_target_c(target_c);
    if (hr != ESP_OK) {
        r = hr == ESP_ERR_INVALID_STATE
            ? PB_POLICY_FAULT_LATCHED : PB_POLICY_INVALID;
        xSemaphoreGive(s_lock);
        return r;
    }

    int64_t now = esp_timer_get_time();
    lease_invalidate_locked();
    s.mode = PB_MODE_POWER_ON;
    s.requested_target_c = pb_heater_get_target_c(); // includes heater clamp
    s.auto_engaged = false;
    s.drying_deadline_us = 0;
    s.local_power_deadline_us = 0;

    if (source_is_remote(source)) {
        lease_issue_locked(owner, now, lease_out);
        pb_heater_notify_link_alive();
    } else {
        s.local_power_deadline_us =
            now + (int64_t)PB_POLICY_LOCAL_POWER_MAX_MS * 1000;
    }
    revision_advance_locked(source);
    s.params.manual_target_c = s.requested_target_c;   // post-clamp
    s.params_dirty = true;
    xSemaphoreGive(s_lock);
    wake_control_task();
    params_notify();
    return PB_POLICY_OK;
}

pb_policy_result_t pb_policy_set_auto(
    float target_c,
    float bed_threshold_c,
    pb_source_t source,
    uint32_t expected_revision)
{
    if (!s_lock || !isfinite(bed_threshold_c)
            || bed_threshold_c < 40.0f || bed_threshold_c > 120.0f)
        return PB_POLICY_INVALID;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    pb_policy_result_t r = heat_precheck_locked(target_c, expected_revision);
    if (r != PB_POLICY_OK) {
        xSemaphoreGive(s_lock);
        return r;
    }
    lease_invalidate_locked();
    s.mode = PB_MODE_AUTO;
    float max_target_c = pb_heater_get_max_target_c();
    s.requested_target_c =
        target_c > max_target_c ? max_target_c : target_c;
    s.auto_bed_threshold_c = bed_threshold_c;
    s.auto_engaged = false;
    s.drying_deadline_us = 0;
    s.local_power_deadline_us = 0;
    (void)pb_heater_set_target_c(0.0f);
    revision_advance_locked(source);
    s.params.auto_target_c = s.requested_target_c;     // post-clamp
    s.params.auto_bed_threshold_c = bed_threshold_c;
    s.params_dirty = true;
    xSemaphoreGive(s_lock);
    wake_control_task();
    params_notify();
    return PB_POLICY_OK;
}

pb_policy_result_t pb_policy_start_drying(
    float target_c,
    uint8_t hours,
    pb_source_t source,
    uint32_t expected_revision)
{
    if (!s_lock || hours == 0 || hours > PB_DRYING_MAX_HOURS)
        return PB_POLICY_INVALID;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    pb_policy_result_t r = heat_precheck_locked(target_c, expected_revision);
    if (r != PB_POLICY_OK) {
        xSemaphoreGive(s_lock);
        return r;
    }
    esp_err_t hr = pb_heater_set_target_c(target_c);
    if (hr != ESP_OK) {
        r = hr == ESP_ERR_INVALID_STATE
            ? PB_POLICY_FAULT_LATCHED : PB_POLICY_INVALID;
        xSemaphoreGive(s_lock);
        return r;
    }
    lease_invalidate_locked();
    s.mode = PB_MODE_DRYING;
    s.requested_target_c = pb_heater_get_target_c();
    s.auto_engaged = false;
    s.local_power_deadline_us = 0;
    s.drying_deadline_us = esp_timer_get_time()
        + (int64_t)hours * 60 * 60 * 1000000;
    revision_advance_locked(source);
    s.params.dry_target_c = s.requested_target_c;      // post-clamp
    s.params.dry_hours = hours;
    s.params_dirty = true;
    xSemaphoreGive(s_lock);
    wake_control_task();
    params_notify();
    return PB_POLICY_OK;
}

void pb_policy_set_mode_off(pb_source_t source)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_off_locked(source);
    xSemaphoreGive(s_lock);
    wake_control_task();
}

void pb_policy_stop_drying(pb_source_t source)
{
    pb_policy_set_mode_off(source);
}

void pb_policy_set_env(float bed_c, bool moonraker_connected)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.bed_c = isfinite(bed_c) ? bed_c : 0.0f;
    s.mk_connected = moonraker_connected;
    xSemaphoreGive(s_lock);
}

pb_policy_result_t pb_policy_heartbeat(const pb_policy_lease_t *lease)
{
    if (!s_lock || !lease || lease->id[0] == '\0')
        return PB_POLICY_STALE_LEASE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s.lease_active || strcmp(lease->id, s.lease.id) != 0
            || s.mode != PB_MODE_POWER_ON) {
        xSemaphoreGive(s_lock);
        return PB_POLICY_STALE_LEASE;
    }
    int64_t now = esp_timer_get_time();
    if (now >= s.lease_deadline_us) {
        xSemaphoreGive(s_lock);
        return PB_POLICY_STALE_LEASE;
    }
    s.lease_deadline_us = now + remote_lease_ttl_us();
    pb_heater_notify_link_alive();
    xSemaphoreGive(s_lock);
    return PB_POLICY_OK;
}

pb_policy_result_t pb_policy_clear_fault(
    pb_source_t source,
    uint32_t expected_revision)
{
    if (!s_lock) return PB_POLICY_INHIBITED;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!revision_matches_locked(expected_revision)) {
        xSemaphoreGive(s_lock);
        return PB_POLICY_REVISION_CONFLICT;
    }
    if (pb_heater_is_inhibited()) {
        xSemaphoreGive(s_lock);
        return PB_POLICY_INHIBITED;
    }
    pb_heater_clear_fault();
    s.last_faulted = false;
    set_off_locked(source);
    xSemaphoreGive(s_lock);
    wake_control_task();
    return PB_POLICY_OK;
}

void pb_policy_request_panic_off(pb_source_t source, const char *reason)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Latch the heater without a GPIO write (this may not be the control task),
    // then drive the full policy transition here so the tick's generic
    // faulted-sync does NOT re-stamp it as SAFETY on the next pass.
    pb_heater_request_panic_off(reason);
    s.mode = PB_MODE_OFF;
    s.requested_target_c = 0.0f;
    s.auto_engaged = false;
    s.drying_deadline_us = 0;
    s.local_power_deadline_us = 0;
    lease_invalidate_locked();
    revision_advance_locked(source);
    // Claim the fault edge so pb_policy_tick() sees last_faulted already true and
    // skips its own SAFETY attribution / lease-invalidate for this transition.
    s.last_faulted = true;
    xSemaphoreGive(s_lock);

    // Drop the SSR now rather than on the next periodic tick: the control task
    // runs the full safety tick immediately on this notification.
    wake_control_task();
    // Keep logging after the wake: diagnostic I/O must never consume the
    // panic-off latency budget.
    ESP_LOGW(TAG, "panic-off requested: %s", reason ? reason : "(unspecified)");
}

static const char *button_str(pb_button_id_t id)
{
    switch (id) {
        case PB_BUTTON_POWER: return "power";
        case PB_BUTTON_AUTO:  return "auto";
        case PB_BUTTON_ON:    return "on";
        case PB_BUTTON_DRY:   return "dry";
        default:              return "?";
    }
}

// Toggle a mode: if already in `target_mode`, go OFF; otherwise arm it from the
// remembered parameters. Runs the setter WITHOUT the policy lock (setters take
// it themselves); source=BUTTON, revision-any (a physical actor always wins).
static pb_policy_result_t button_toggle_mode(pb_mode_t target_mode)
{
    if (pb_policy_get_mode() == target_mode) {
        pb_policy_set_mode_off(PB_SOURCE_BUTTON);
        return PB_POLICY_OK;
    }
    pb_policy_params_t p;
    pb_policy_get_params(&p);
    switch (target_mode) {
        case PB_MODE_POWER_ON:
            return pb_policy_set_power_on(
                p.manual_target_c, PB_SOURCE_BUTTON,
                "button", PB_POLICY_REVISION_ANY, NULL);
        case PB_MODE_AUTO:
            return pb_policy_set_auto(
                p.auto_target_c, p.auto_bed_threshold_c,
                PB_SOURCE_BUTTON, PB_POLICY_REVISION_ANY);
        case PB_MODE_DRYING:
            return pb_policy_start_drying(
                p.dry_target_c, p.dry_hours,
                PB_SOURCE_BUTTON, PB_POLICY_REVISION_ANY);
        default:
            return PB_POLICY_INVALID;
    }
}

void pb_policy_on_button(pb_button_id_t id, pb_button_event_t ev)
{
    if (!s_lock) return;

    if (ev == PB_BUTTON_LONG) {
        // Power long-press while faulted: attempt a recovery clear instead of a
        // redundant panic. Any other long-press latches panic-off.
        if (id == PB_BUTTON_POWER && pb_heater_is_faulted()) {
            // A physical actor always wins, so revision-any is correct here.
            pb_policy_result_t r =
                pb_policy_clear_fault(PB_SOURCE_BUTTON, PB_POLICY_REVISION_ANY);
            pv_evlog_add("btn: power long -> clear fault (%s)",
                         pb_policy_result_str(r));
            return;
        }
        pb_policy_request_panic_off(PB_SOURCE_BUTTON, "button panic-off");
        // The event log can wait on its diagnostic mutex, so record only after
        // the heater is latched off and the control task has been notified.
        pv_evlog_add("btn: %s long -> panic-off", button_str(id));
        return;
    }

    // SHORT press.
    pb_policy_result_t r;
    switch (id) {
        case PB_BUTTON_ON:
            r = button_toggle_mode(PB_MODE_POWER_ON);
            if (r == PB_POLICY_OK) {
                pv_evlog_add("btn: on -> %s",
                             pb_policy_mode_str(pb_policy_get_mode()));
            } else {
                pv_evlog_add("btn: on rejected (%s)",
                             pb_policy_result_str(r));
            }
            break;
        case PB_BUTTON_AUTO:
            r = button_toggle_mode(PB_MODE_AUTO);
            if (r == PB_POLICY_OK) {
                pv_evlog_add("btn: auto -> %s",
                             pb_policy_mode_str(pb_policy_get_mode()));
            } else {
                pv_evlog_add("btn: auto rejected (%s)",
                             pb_policy_result_str(r));
            }
            break;
        case PB_BUTTON_DRY:
            r = button_toggle_mode(PB_MODE_DRYING);
            if (r == PB_POLICY_OK) {
                pv_evlog_add("btn: dry -> %s",
                             pb_policy_mode_str(pb_policy_get_mode()));
            } else {
                pv_evlog_add("btn: dry rejected (%s)",
                             pb_policy_result_str(r));
            }
            break;
        case PB_BUTTON_POWER:
            // Master OFF. Already-off is a deliberate no-op: log it but do not
            // bump the revision, so an idle tap does not churn observers.
            if (pb_policy_get_mode() == PB_MODE_OFF) {
                pv_evlog_add("btn: power (already off)");
            } else {
                pb_policy_set_mode_off(PB_SOURCE_BUTTON);
                pv_evlog_add("btn: power -> off");
            }
            break;
        default:
            break;
    }
}

void pb_policy_tick(void)
{
    if (!s_lock) return;
    int64_t now = esp_timer_get_time();
    bool watchdog_trip = false;
    bool local_limit_expired = false;
    const char *watchdog_reason = NULL;
    float target = 0.0f;
    bool autonomous = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    switch (s.mode) {
        case PB_MODE_POWER_ON:
            if (s.lease_active && now >= s.lease_deadline_us) {
                watchdog_trip = true;
                watchdog_reason = "controller lease expired";
            } else if (!s.lease_active && s.local_power_deadline_us > 0
                       && now >= s.local_power_deadline_us) {
                local_limit_expired = true;
                set_off_locked(PB_SOURCE_WATCHDOG);
            } else {
                target = s.requested_target_c;
                autonomous = !s.lease_active;
            }
            break;

        case PB_MODE_AUTO:
        {
            bool was_engaged = s.auto_engaged;
            if (!s.mk_connected) {
                s.auto_engaged = false;
            } else if (!s.auto_engaged && s.bed_c >= s.auto_bed_threshold_c) {
                s.auto_engaged = true;
            } else if (s.auto_engaged
                       && s.bed_c < s.auto_bed_threshold_c
                                      - PB_AUTO_BED_HYSTERESIS_C) {
                s.auto_engaged = false;
            }
            if (s.auto_engaged != was_engaged)
                revision_advance_locked(s.source);
            if (s.auto_engaged) {
                target = s.requested_target_c;
                autonomous = true;
            }
            break;
        }

        case PB_MODE_DRYING:
            if (now >= s.drying_deadline_us) {
                set_off_locked(PB_SOURCE_WATCHDOG);
            } else {
                target = s.requested_target_c;
                autonomous = true;
            }
            break;

        case PB_MODE_OFF:
        default:
            break;
    }

    if (watchdog_trip) {
        // Sole control-task path for a latching remote/local timeout.
        lease_invalidate_locked();
        s.mode = PB_MODE_OFF;
        s.requested_target_c = 0.0f;
        s.auto_engaged = false;
        s.drying_deadline_us = 0;
        s.local_power_deadline_us = 0;
        revision_advance_locked(PB_SOURCE_WATCHDOG);
    }

    // Keep the computed transition and its actuator application under the same
    // policy mutex. Otherwise an OFF command could land after this tick computed
    // an old positive target but before it applied that target, allowing the
    // stale tick to re-arm heat after OFF returned.
    if (watchdog_trip) {
        pb_heater_emergency_off(watchdog_reason);
    } else {
        // Avoid logging/reapplying an unchanged target on every 500 ms tick.
        if (fabsf(pb_heater_get_target_c() - target) >= 0.05f)
            (void)pb_heater_set_target_c(target);
        if (autonomous && target > 0.0f)
            pb_heater_notify_link_alive();
    }

    pb_heater_tick();

    bool heat = pb_heater_heat_mode();
    bool faulted = pb_heater_is_faulted();
    float chamber_c = pb_ntc_smoothed_c(PB_NTC_CHAMBER);
    bool chamber_ok = pb_ntc_last_status(PB_NTC_CHAMBER) == PB_NTC_OK
                      && isfinite(chamber_c);

    if (heat) s.heated_this_session = true;

    bool cooldown = false;
    if (!heat && s.heated_this_session) {
        if (chamber_ok && chamber_c > PB_COOLDOWN_TEMP_C) {
            cooldown = true;
        } else {
            s.heated_this_session = false;
        }
    }

    if (faulted && !s.last_faulted) {
        s.mode = PB_MODE_OFF;
        s.requested_target_c = 0.0f;
        s.auto_engaged = false;
        s.drying_deadline_us = 0;
        s.local_power_deadline_us = 0;
        lease_invalidate_locked();
        // Preserve WATCHDOG attribution for a policy-triggered expiry.
        if (!watchdog_trip) revision_advance_locked(PB_SOURCE_SAFETY);
    }
    s.last_faulted = faulted;

    bool want_airflow = heat || faulted || cooldown;
    uint8_t fan = s.requested_fan_percent;
    if (want_airflow && fan < 30) fan = 30;
    pb_fan_set_level(fan);

    // Front-panel indication. Power is the "device alive / something's wrong"
    // light; On/Auto/Dry each carry their own mode, so the panel alone tells you
    // which mode is active. `faulted` is pb_heater_is_faulted(), which already
    // covers a permanent inhibit as well as a latched trip. A fault forces
    // mode=OFF above, so the mode LEDs go dark on their own.
    // Power is release-only: GPIO21 is also the console TX pin (CONFIG_PB_POWER_LED).
    pb_leds_set(PB_LED_POWER, faulted ? PB_LED_BLINK : PB_LED_SOLID);
    pb_leds_set(PB_LED_ON, s.mode == PB_MODE_POWER_ON ? PB_LED_SOLID : PB_LED_OFF);
    // AUTO distinguishes armed-but-waiting (no Moonraker link, or bed below the
    // threshold) from actually driving heat — the most useful thing the panel says.
    pb_leds_set(PB_LED_AUTO,
                s.mode != PB_MODE_AUTO  ? PB_LED_OFF
                : s.auto_engaged        ? PB_LED_SOLID
                                        : PB_LED_BLINK_SLOW);
    pb_leds_set(PB_LED_DRY, s.mode == PB_MODE_DRYING ? PB_LED_SOLID : PB_LED_OFF);

    if (local_limit_expired)
        ESP_LOGW(TAG, "local POWER_ON limit expired; mode set OFF");
    xSemaphoreGive(s_lock);
}

void pb_policy_get_snapshot(pb_policy_snapshot_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!s_lock) return;

    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->state_revision = s.revision;
    out->mode = s.mode;
    out->source = s.source;
    out->requested_target_c = s.requested_target_c;
    out->requested_fan_percent = s.requested_fan_percent;
    out->moonraker_connected = s.mk_connected;
    out->bed_c = s.bed_c;
    out->auto_engaged = s.auto_engaged;
    out->auto_bed_threshold_c = s.auto_bed_threshold_c;
    out->params = s.params;
    out->drying = s.mode == PB_MODE_DRYING;
    if (out->drying && s.drying_deadline_us > now) {
        out->drying_remaining_s =
            (uint32_t)((s.drying_deadline_us - now + 999999) / 1000000);
    }
    out->lease_active = s.lease_active && now < s.lease_deadline_us;
    if (out->lease_active) {
        copy_text(out->lease_id, sizeof out->lease_id, s.lease.id);
        copy_text(out->lease_owner, sizeof out->lease_owner, s.lease_owner);
        out->lease_expires_ms =
            (uint32_t)((s.lease_deadline_us - now + 999) / 1000);
    }
    xSemaphoreGive(s_lock);

    out->effective_target_c = pb_heater_get_target_c();
    out->heater_demand = pb_heater_heat_mode();
    out->heater_output = pb_heater_is_on();
    out->effective_fan_percent = pb_fan_get_level();
    out->fault_latched = pb_heater_is_faulted();
    out->inhibited = pb_heater_is_inhibited();
    const char *reason = pb_heater_fault_reason();
    copy_text(out->fault_reason, sizeof out->fault_reason, reason);

    out->chamber_status = pb_ntc_last_status(PB_NTC_CHAMBER);
    out->ptc_status = pb_ntc_last_status(PB_NTC_PTC);
    out->chamber_c = out->chamber_status == PB_NTC_OK
        ? pb_ntc_smoothed_c(PB_NTC_CHAMBER) : NAN;
    out->ptc_c = out->ptc_status == PB_NTC_OK
        ? pb_ntc_smoothed_c(PB_NTC_PTC) : NAN;

    out->thermal_purge = !out->heater_demand
        && !out->fault_latched
        && out->effective_fan_percent > out->requested_fan_percent;
}

pb_mode_t pb_policy_get_mode(void)
{
    if (!s_lock) return PB_MODE_OFF;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pb_mode_t mode = s.mode;
    xSemaphoreGive(s_lock);
    return mode;
}

const char *pb_policy_mode_str(pb_mode_t mode)
{
    switch (mode) {
        case PB_MODE_POWER_ON: return "power_on";
        case PB_MODE_AUTO:     return "auto";
        case PB_MODE_DRYING:   return "drying";
        case PB_MODE_OFF:
        default:               return "off";
    }
}

const char *pb_policy_source_str(pb_source_t source)
{
    switch (source) {
        case PB_SOURCE_WEB:     return "web";
        case PB_SOURCE_KLIPPER: return "klipper";
        case PB_SOURCE_BUTTON:  return "button";
        case PB_SOURCE_SAFETY:  return "safety";
        case PB_SOURCE_WATCHDOG:return "watchdog";
        case PB_SOURCE_BOOT:
        default:                return "boot";
    }
}

const char *pb_policy_result_str(pb_policy_result_t result)
{
    switch (result) {
        case PB_POLICY_OK:                return "ok";
        case PB_POLICY_INVALID:           return "invalid";
        case PB_POLICY_REVISION_CONFLICT: return "revision_conflict";
        case PB_POLICY_FAULT_LATCHED:     return "fault_latched";
        case PB_POLICY_INHIBITED:         return "inhibited";
        case PB_POLICY_STALE_LEASE:       return "stale_lease";
        default:                          return "unknown";
    }
}
