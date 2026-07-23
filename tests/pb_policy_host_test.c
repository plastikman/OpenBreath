#include "pb_policy.h"
#include "pb_heater.h"
#include "pb_leds.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t fake_now_us;
static unsigned random_generation;
static float heater_target;
static bool heater_on;
static bool heater_fault;
static bool heater_inhibited;
static const char *heater_reason;
static unsigned heater_link_pets;
static float heater_max_target_c;
static uint32_t heater_comms_timeout_ms;
static uint8_t fan_level;
static pb_led_pattern_t led_pattern[PB_LED_COUNT];
static float chamber_c = 25.0f;
static float ptc_c = 25.0f;
static pb_ntc_status_t chamber_status = PB_NTC_OK;
static pb_ntc_status_t ptc_status = PB_NTC_OK;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

int64_t esp_timer_get_time(void) { return fake_now_us; }

void esp_fill_random(void *buf, size_t len)
{
    unsigned char *p = buf;
    random_generation++;
    for (size_t i = 0; i < len; ++i)
        p[i] = (unsigned char)(random_generation + i);
}

esp_err_t pb_heater_set_target_c(float target)
{
    if (!isfinite(target)) return ESP_ERR_INVALID_ARG;
    if ((heater_fault || heater_inhibited) && target > 0.0f)
        return ESP_ERR_INVALID_STATE;
    if (target < 0.0f) target = 0.0f;
    if (target > heater_max_target_c) target = heater_max_target_c;
    heater_target = target;
    return ESP_OK;
}

float pb_heater_get_target_c(void) { return heater_target; }
float pb_heater_get_max_target_c(void) { return heater_max_target_c; }
uint32_t pb_heater_get_comms_timeout_ms(void)
{
    return heater_comms_timeout_ms;
}
void pb_heater_notify_link_alive(void) { heater_link_pets++; }
void pb_heater_tick(void) { heater_on = heater_target > 0.0f && !heater_fault; }

void pb_heater_emergency_off(const char *reason)
{
    heater_target = 0.0f;
    heater_on = false;
    heater_fault = true;
    heater_reason = reason;
}

void pb_heater_clear_fault(void)
{
    if (heater_inhibited) return;
    heater_fault = false;
    heater_target = 0.0f;
    heater_reason = NULL;
}

bool pb_heater_is_inhibited(void) { return heater_inhibited; }
bool pb_heater_is_faulted(void) { return heater_fault || heater_inhibited; }
const char *pb_heater_fault_reason(void) { return heater_reason; }
bool pb_heater_is_on(void) { return heater_on; }
bool pb_heater_heat_mode(void)
{
    return heater_target > 0.0f && !heater_fault && !heater_inhibited;
}

void pb_fan_set_level(uint8_t percent) { fan_level = percent ? 100 : 0; }
uint8_t pb_fan_get_level(void) { return fan_level; }
void pb_leds_set(pb_led_id_t id, pb_led_pattern_t pattern)
{
    if (id >= 0 && id < PB_LED_COUNT) led_pattern[id] = pattern;
}

pb_ntc_status_t pb_ntc_last_status(pb_ntc_channel_t channel)
{
    return channel == PB_NTC_CHAMBER ? chamber_status : ptc_status;
}

float pb_ntc_smoothed_c(pb_ntc_channel_t channel)
{
    return channel == PB_NTC_CHAMBER ? chamber_c : ptc_c;
}

static void reset_fixture(void)
{
    fake_now_us = 1000000;
    heater_target = 0.0f;
    heater_on = false;
    heater_fault = false;
    heater_inhibited = false;
    heater_reason = NULL;
    heater_link_pets = 0;
    heater_max_target_c = 70.0f;
    heater_comms_timeout_ms = 5U * 60U * 1000U;
    fan_level = 0;
    memset(led_pattern, 0, sizeof led_pattern);
    chamber_c = 25.0f;
    ptc_c = 25.0f;
    chamber_status = PB_NTC_OK;
    ptc_status = PB_NTC_OK;
    CHECK(pb_policy_init() == ESP_OK);
}

static pb_policy_snapshot_t snapshot(void)
{
    pb_policy_snapshot_t snap;
    pb_policy_get_snapshot(&snap);
    return snap;
}

static void test_boot_is_off(void)
{
    reset_fixture();
    pb_policy_snapshot_t snap = snapshot();
    CHECK(snap.state_revision == 1);
    CHECK(snap.mode == PB_MODE_OFF);
    CHECK(snap.source == PB_SOURCE_BOOT);
    CHECK(snap.effective_target_c == 0.0f);
    CHECK(!snap.lease_active);
}

static void test_remote_lease_and_stale_revision(void)
{
    reset_fixture();
    pb_policy_lease_t first;
    CHECK(pb_policy_set_power_on(
        45.0f, PB_SOURCE_KLIPPER, "u1", 1, &first) == PB_POLICY_OK);
    CHECK(strlen(first.id) == PB_POLICY_LEASE_ID_LEN);

    pb_policy_snapshot_t snap = snapshot();
    CHECK(snap.state_revision == 2);
    CHECK(snap.mode == PB_MODE_POWER_ON);
    CHECK(snap.source == PB_SOURCE_KLIPPER);
    CHECK(snap.requested_target_c == 45.0f);
    CHECK(snap.lease_active);
    CHECK(strcmp(snap.lease_owner, "u1") == 0);
    CHECK(heater_link_pets == 1);

    pb_policy_lease_t rejected = {0};
    CHECK(pb_policy_set_power_on(
        50.0f, PB_SOURCE_WEB, "old-tab", 1, &rejected)
        == PB_POLICY_REVISION_CONFLICT);
    CHECK(rejected.id[0] == '\0');
    CHECK(snapshot().requested_target_c == 45.0f);

    CHECK(pb_policy_heartbeat(&first) == PB_POLICY_OK);
    CHECK(heater_link_pets == 2);
}

static void test_new_command_supersedes_old_lease_and_off_is_unconditional(void)
{
    reset_fixture();
    pb_policy_lease_t first, second;
    CHECK(pb_policy_set_power_on(
        45.0f, PB_SOURCE_WEB, "tab-1", 1, &first) == PB_POLICY_OK);
    uint32_t rev = snapshot().state_revision;
    CHECK(pb_policy_set_power_on(
        50.0f, PB_SOURCE_KLIPPER, "u1", rev, &second) == PB_POLICY_OK);
    CHECK(strcmp(first.id, second.id) != 0);
    CHECK(pb_policy_heartbeat(&first) == PB_POLICY_STALE_LEASE);
    CHECK(pb_policy_heartbeat(&second) == PB_POLICY_OK);

    // OFF ignores an arbitrarily stale caller revision by design.
    pb_policy_set_mode_off(PB_SOURCE_WEB);
    pb_policy_snapshot_t snap = snapshot();
    CHECK(snap.mode == PB_MODE_OFF);
    CHECK(snap.effective_target_c == 0.0f);
    CHECK(!snap.lease_active);
    CHECK(pb_policy_heartbeat(&second) == PB_POLICY_STALE_LEASE);
}

static void test_lease_expiry_latches_watchdog_fault(void)
{
    reset_fixture();
    pb_policy_lease_t lease;
    CHECK(pb_policy_set_power_on(
        55.0f, PB_SOURCE_KLIPPER, "u1", 1, &lease) == PB_POLICY_OK);
    pb_policy_tick();
    CHECK(pb_heater_is_on());

    fake_now_us += (int64_t)heater_comms_timeout_ms * 1000 + 1;
    pb_policy_tick();
    pb_policy_snapshot_t snap = snapshot();
    CHECK(snap.mode == PB_MODE_OFF);
    CHECK(snap.source == PB_SOURCE_WATCHDOG);
    CHECK(snap.fault_latched);
    CHECK(strcmp(snap.fault_reason, "controller lease expired") == 0);
    CHECK(!snap.heater_output);
    CHECK(!snap.lease_active);
    CHECK(pb_policy_heartbeat(&lease) == PB_POLICY_STALE_LEASE);
}

static void test_auto_requires_live_moonraker_and_uses_hysteresis(void)
{
    reset_fixture();
    CHECK(pb_policy_set_auto(
        60.0f, 100.0f, PB_SOURCE_WEB, 1) == PB_POLICY_OK);

    pb_policy_set_env(99.0f, true);
    pb_policy_tick();
    CHECK(snapshot().effective_target_c == 0.0f);

    pb_policy_set_env(100.0f, true);
    pb_policy_tick();
    pb_policy_snapshot_t snap = snapshot();
    CHECK(snap.auto_engaged);
    CHECK(snap.effective_target_c == 60.0f);

    pb_policy_set_env(98.0f, true);
    pb_policy_tick();
    CHECK(snapshot().auto_engaged);

    pb_policy_set_env(96.9f, true);
    pb_policy_tick();
    snap = snapshot();
    CHECK(!snap.auto_engaged);
    CHECK(snap.effective_target_c == 0.0f);

    pb_policy_set_env(105.0f, false);
    pb_policy_tick();
    CHECK(!snapshot().auto_engaged);
}

static void test_drying_is_bounded_and_expires_off(void)
{
    reset_fixture();
    CHECK(pb_policy_start_drying(
        55.0f, 13, PB_SOURCE_WEB, 1) == PB_POLICY_INVALID);
    CHECK(pb_policy_start_drying(
        55.0f, 1, PB_SOURCE_WEB, 1) == PB_POLICY_OK);
    pb_policy_tick();
    CHECK(snapshot().drying);
    CHECK(snapshot().effective_target_c == 55.0f);

    fake_now_us += (int64_t)60 * 60 * 1000000 + 1;
    pb_policy_tick();
    pb_policy_snapshot_t snap = snapshot();
    CHECK(snap.mode == PB_MODE_OFF);
    CHECK(!snap.drying);
    CHECK(snap.effective_target_c == 0.0f);
}

static void test_runtime_limits_drive_auto_and_remote_lease(void)
{
    reset_fixture();
    heater_max_target_c = 52.0f;
    CHECK(pb_policy_set_auto(
        65.0f, 100.0f, PB_SOURCE_WEB, 1) == PB_POLICY_OK);
    CHECK(snapshot().requested_target_c == 52.0f);
    pb_policy_set_env(100.0f, true);
    pb_policy_tick();
    CHECK(snapshot().effective_target_c == 52.0f);

    reset_fixture();
    heater_comms_timeout_ms = 10000;
    pb_policy_lease_t lease;
    CHECK(pb_policy_set_power_on(
        45.0f, PB_SOURCE_KLIPPER, "u1", 1, &lease) == PB_POLICY_OK);
    CHECK(snapshot().lease_expires_ms == heater_comms_timeout_ms);
    fake_now_us += (int64_t)heater_comms_timeout_ms * 1000 + 1;
    pb_policy_tick();
    CHECK(snapshot().fault_latched);
}

static void test_legacy_idle_heartbeat_is_benign(void)
{
    reset_fixture();
    CHECK(pb_policy_heartbeat_legacy() == PB_POLICY_OK);
    CHECK(heater_link_pets == 0);
    CHECK(snapshot().mode == PB_MODE_OFF);
}

static void test_local_power_limit_expires_off_without_fault(void)
{
    reset_fixture();
    CHECK(pb_policy_set_power_on(
        45.0f, PB_SOURCE_BUTTON, "panel", 1, NULL) == PB_POLICY_OK);
    pb_policy_tick();
    CHECK(snapshot().heater_output);

    fake_now_us += (int64_t)PB_POLICY_LOCAL_POWER_MAX_MS * 1000 + 1;
    pb_policy_tick();
    pb_policy_snapshot_t snap = snapshot();
    CHECK(snap.mode == PB_MODE_OFF);
    CHECK(snap.source == PB_SOURCE_WATCHDOG);
    CHECK(!snap.fault_latched);
    CHECK(snap.effective_target_c == 0.0f);

    CHECK(pb_policy_set_power_on(
        45.0f, PB_SOURCE_BUTTON, "panel", snap.state_revision, NULL)
        == PB_POLICY_OK);
}

static void test_external_fault_sync_off_and_clear(void)
{
    reset_fixture();
    pb_policy_lease_t lease;
    CHECK(pb_policy_set_power_on(
        45.0f, PB_SOURCE_KLIPPER, "u1", 1, &lease) == PB_POLICY_OK);
    pb_policy_tick();
    CHECK(led_pattern[PB_LED_POWER] == PB_LED_SOLID);
    CHECK(led_pattern[PB_LED_ON] == PB_LED_SOLID);

    pb_heater_emergency_off("external trip");
    pb_policy_tick();
    pb_policy_snapshot_t snap = snapshot();
    CHECK(snap.mode == PB_MODE_OFF);
    CHECK(snap.source == PB_SOURCE_SAFETY);
    CHECK(snap.fault_latched);
    CHECK(strcmp(snap.fault_reason, "external trip") == 0);
    CHECK(!snap.lease_active);
    CHECK(led_pattern[PB_LED_POWER] == PB_LED_BLINK);
    CHECK(led_pattern[PB_LED_ON] == PB_LED_BLINK);

    // OFF is unconditional even while the underlying safety fault remains latched.
    pb_policy_set_mode_off(PB_SOURCE_WEB);
    snap = snapshot();
    CHECK(snap.mode == PB_MODE_OFF);
    CHECK(snap.effective_target_c == 0.0f);
    CHECK(snap.fault_latched);

    CHECK(pb_policy_clear_fault(PB_SOURCE_WEB) == PB_POLICY_OK);
    snap = snapshot();
    CHECK(snap.mode == PB_MODE_OFF);
    CHECK(snap.source == PB_SOURCE_WEB);
    CHECK(!snap.fault_latched);
    CHECK(snap.effective_target_c == 0.0f);
}

int main(void)
{
    test_boot_is_off();
    test_remote_lease_and_stale_revision();
    test_new_command_supersedes_old_lease_and_off_is_unconditional();
    test_lease_expiry_latches_watchdog_fault();
    test_auto_requires_live_moonraker_and_uses_hysteresis();
    test_drying_is_bounded_and_expires_off();
    test_runtime_limits_drive_auto_and_remote_lease();
    test_legacy_idle_heartbeat_is_benign();
    test_local_power_limit_expires_off_without_fault();
    test_external_fault_sync_off_and_clear();
    puts("pb_policy host tests: PASS");
    return 0;
}
