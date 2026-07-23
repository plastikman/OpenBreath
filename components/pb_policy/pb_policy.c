// SPDX-License-Identifier: MIT
#include "pb_policy.h"

#include "pb_fan.h"
#include "pb_heater.h"
#include "pb_ntc.h"
#include "pb_leds.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "pb_policy";

#define PB_COOLDOWN_TEMP_C          40.0f
#define PB_AUTO_BED_HYSTERESIS_C     3.0f
#define PB_DRYING_MAX_HOURS         12U
#define PB_MIN_MODE_TARGET_C        30.0f

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
} policy_state_t;

static SemaphoreHandle_t s_lock;
static policy_state_t s;

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

esp_err_t pb_policy_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s, 0, sizeof s);
    s.mode = PB_MODE_OFF;
    s.source = PB_SOURCE_BOOT;
    s.revision = 1;
    s.auto_bed_threshold_c = 100.0f;
    s.last_faulted = pb_heater_is_faulted();
    xSemaphoreGive(s_lock);

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
    xSemaphoreGive(s_lock);
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
    xSemaphoreGive(s_lock);
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
    xSemaphoreGive(s_lock);
    return PB_POLICY_OK;
}

void pb_policy_set_mode_off(pb_source_t source)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_off_locked(source);
    xSemaphoreGive(s_lock);
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

pb_policy_result_t pb_policy_heartbeat_legacy(void)
{
    // Transitional adapter for the alpha /heartbeat route.  API v2 removes this
    // capability and requires the device-issued lease ID on every heartbeat.
    if (!s_lock) return PB_POLICY_STALE_LEASE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // The deployed alpha helper heartbeats even while idle. Keep that harmless
    // during the v1->v2 transition, but never let it revive a missing/expired
    // POWER_ON lease.
    if (s.mode != PB_MODE_POWER_ON) {
        xSemaphoreGive(s_lock);
        return PB_POLICY_OK;
    }
    if (!s.lease_active) {
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

pb_policy_result_t pb_policy_clear_fault(pb_source_t source)
{
    if (!s_lock) return PB_POLICY_INHIBITED;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (pb_heater_is_inhibited()) {
        xSemaphoreGive(s_lock);
        return PB_POLICY_INHIBITED;
    }
    pb_heater_clear_fault();
    s.last_faulted = false;
    set_off_locked(source);
    xSemaphoreGive(s_lock);
    return PB_POLICY_OK;
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

    // Preserve Phase A's panel semantics through the state-machine rewrite.
    // The Power LED is release-only because GPIO21 is also console TX; the On
    // LED mirrors it until the later mode/button work owns Auto/On/Dry.
    pb_led_pattern_t heat_pat =
        faulted ? PB_LED_BLINK : heat ? PB_LED_SOLID : PB_LED_OFF;
    pb_leds_set(PB_LED_POWER, heat_pat);
    pb_leds_set(PB_LED_ON, heat_pat);

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
