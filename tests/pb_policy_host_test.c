#include "pb_policy.h"
#include "pb_heater.h"
#include "pb_leds.h"
#include "nvs.h"

#include <math.h>
#include <stdarg.h>
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
// When set, the "sensor condition" is still unsafe, so the next tick re-latches
// after any clear -- mirroring pb_heater_tick()'s real re-evaluation.
static bool heater_relatch;
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
void pb_heater_tick(void)
{
    if (heater_relatch) { heater_fault = true; heater_reason = "still unsafe"; }
    heater_on = heater_target > 0.0f && !heater_fault;
}

void pb_heater_emergency_off(const char *reason)
{
    heater_target = 0.0f;
    heater_on = false;
    heater_fault = true;
    heater_reason = reason;
}

// Latch only — no "GPIO write". The real heater drops the SSR on the next tick;
// the fake reflects that by not clearing heater_on until pb_heater_tick() runs.
void pb_heater_request_panic_off(const char *reason)
{
    heater_target = 0.0f;
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

static unsigned wake_calls;
static void count_wake(void) { wake_calls++; }

static bool panic_logged_before_wake;
static char last_evlog[128];
void pv_evlog_init(void) {}
void pv_evlog_add(const char *fmt, ...)
{
    if (strstr(fmt, "panic-off") && wake_calls == 0)
        panic_logged_before_wake = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_evlog, sizeof last_evlog, fmt, ap);
    va_end(ap);
}

// --- In-memory NVS ----------------------------------------------------------
// Enough of the API for pb_policy's parameter persistence, plus a failure switch
// so the retry path is testable.
#define NVS_MAX_KEYS 16
static struct { char key[20]; uint32_t val; bool set; } nvs_store[NVS_MAX_KEYS];
static bool nvs_write_fails;
static unsigned nvs_commits;
static unsigned nvs_writes;

static int nvs_slot(const char *key, bool create)
{
    for (int i = 0; i < NVS_MAX_KEYS; ++i)
        if (nvs_store[i].set && strcmp(nvs_store[i].key, key) == 0) return i;
    if (!create) return -1;
    for (int i = 0; i < NVS_MAX_KEYS; ++i) {
        if (!nvs_store[i].set) {
            snprintf(nvs_store[i].key, sizeof nvs_store[i].key, "%s", key);
            nvs_store[i].set = true;
            return i;
        }
    }
    return -1;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)ns; (void)mode;
    if (out) *out = 1;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    (void)h;
    int i = nvs_slot(key, false);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    if (out) *out = nvs_store[i].val;
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value)
{
    (void)h;
    if (nvs_write_fails) return ESP_ERR_INVALID_STATE;
    int i = nvs_slot(key, true);
    if (i < 0) return ESP_ERR_NO_MEM;
    nvs_store[i].val = value;
    nvs_writes++;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h) { (void)h; nvs_commits++; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

static uint32_t nvs_read(const char *key)
{
    uint32_t v = 0;
    CHECK(nvs_get_u32(1, key, &v) == ESP_OK);
    return v;
}

static bool nvs_has(const char *key) { return nvs_slot(key, false) >= 0; }

pb_ntc_status_t pb_ntc_last_status(pb_ntc_channel_t channel)
{
    return channel == PB_NTC_CHAMBER ? chamber_status : ptc_status;
}

float pb_ntc_smoothed_c(pb_ntc_channel_t channel)
{
    return channel == PB_NTC_CHAMBER ? chamber_c : ptc_c;
}

static void reset_nvs(void)
{
    memset(nvs_store, 0, sizeof nvs_store);
    nvs_write_fails = false;
    nvs_commits = 0;
    nvs_writes = 0;
}

static void reset_fixture(void)
{
    reset_nvs();
    fake_now_us = 1000000;
    heater_target = 0.0f;
    heater_on = false;
    heater_fault = false;
    heater_inhibited = false;
    heater_reason = NULL;
    heater_link_pets = 0;
    heater_relatch = false;
    heater_max_target_c = 70.0f;
    heater_comms_timeout_ms = 5U * 60U * 1000U;
    fan_level = 0;
    wake_calls = 0;
    panic_logged_before_wake = false;
    last_evlog[0] = '\0';
    memset(led_pattern, 0, sizeof led_pattern);
    chamber_c = 25.0f;
    ptc_c = 25.0f;
    chamber_status = PB_NTC_OK;
    ptc_status = PB_NTC_OK;
    CHECK(pb_policy_init() == ESP_OK);
    pb_policy_set_wake_cb(count_wake);
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
    CHECK(led_pattern[PB_LED_ON] == PB_LED_OFF);   // fault forced mode OFF

    // OFF is unconditional even while the underlying safety fault remains latched.
    pb_policy_set_mode_off(PB_SOURCE_WEB);
    snap = snapshot();
    CHECK(snap.mode == PB_MODE_OFF);
    CHECK(snap.effective_target_c == 0.0f);
    CHECK(snap.fault_latched);

    uint32_t clear_revision = snap.state_revision;
    CHECK(pb_policy_clear_fault(
        PB_SOURCE_WEB, clear_revision + 1) == PB_POLICY_REVISION_CONFLICT);
    CHECK(pb_policy_clear_fault(
        PB_SOURCE_WEB, clear_revision) == PB_POLICY_OK);
    snap = snapshot();
    CHECK(snap.mode == PB_MODE_OFF);
    CHECK(snap.source == PB_SOURCE_WEB);
    CHECK(!snap.fault_latched);
    CHECK(snap.effective_target_c == 0.0f);
}

static void test_params_default_and_load_never_arms(void)
{
    reset_fixture();
    pb_policy_load_params();

    pb_policy_params_t p;
    pb_policy_get_params(&p);
    CHECK(p.manual_target_c == 50.0f);
    CHECK(p.auto_target_c == 60.0f);
    CHECK(p.auto_bed_threshold_c == 100.0f);
    CHECK(p.dry_target_c == 60.0f);
    CHECK(p.dry_hours == 12);

    // Loading parameters must never arm heat or change the mode.
    pb_policy_snapshot_t snap = snapshot();
    CHECK(snap.mode == PB_MODE_OFF);
    CHECK(snap.effective_target_c == 0.0f);
    CHECK(!snap.lease_active);
    CHECK(snap.params.manual_target_c == p.manual_target_c);
    CHECK(snap.params.auto_target_c == p.auto_target_c);
    CHECK(snap.params.auto_bed_threshold_c == p.auto_bed_threshold_c);
    CHECK(snap.params.dry_target_c == p.dry_target_c);
    CHECK(snap.params.dry_hours == p.dry_hours);
    // Defaults match an empty flash, so there is nothing to write back.
    CHECK(!pb_policy_persist_pending());
    CHECK(nvs_writes == 0);
}

static void test_params_clamp_corrupt_values(void)
{
    reset_fixture();
    CHECK(nvs_set_u32(1, "md_last", 9000) == ESP_OK);      // 90 C -> ceiling 70
    CHECK(nvs_set_u32(1, "md_auto_tgt", 100) == ESP_OK);   // 1 C  -> floor 30
    CHECK(nvs_set_u32(1, "md_auto_bed", 50000) == ESP_OK); // 500 C -> 120
    CHECK(nvs_set_u32(1, "md_dry_hrs", 99) == ESP_OK);     // out of range -> 12
    pb_policy_load_params();

    pb_policy_params_t p;
    pb_policy_get_params(&p);
    CHECK(p.manual_target_c == 70.0f);
    CHECK(p.auto_target_c == 30.0f);
    CHECK(p.auto_bed_threshold_c == 120.0f);
    CHECK(p.dry_hours == 12);
    CHECK(snapshot().mode == PB_MODE_OFF);
}

static void test_params_load_respects_runtime_max(void)
{
    reset_fixture();
    heater_max_target_c = 55.0f;
    CHECK(nvs_set_u32(1, "md_last", 7000) == ESP_OK);
    CHECK(nvs_set_u32(1, "md_auto_tgt", 6500) == ESP_OK);
    CHECK(nvs_set_u32(1, "md_dry_tgt", 6000) == ESP_OK);
    pb_policy_load_params();

    pb_policy_params_t p;
    pb_policy_get_params(&p);
    CHECK(p.manual_target_c == 55.0f);
    CHECK(p.auto_target_c == 55.0f);
    CHECK(p.dry_target_c == 55.0f);
    CHECK(snapshot().mode == PB_MODE_OFF);
}

static void test_params_persist_only_changed_keys(void)
{
    reset_fixture();
    pb_policy_load_params();

    CHECK(pb_policy_set_power_on(
        45.0f, PB_SOURCE_WEB, "tab", PB_POLICY_REVISION_ANY, NULL)
        == PB_POLICY_OK);
    CHECK(pb_policy_persist_pending());
    CHECK(nvs_read("md_last") == 4500);
    CHECK(nvs_commits == 1);
    // Nothing left dirty, so a second drain is a no-op.
    CHECK(!pb_policy_persist_pending());
    CHECK(nvs_commits == 1);

    unsigned before = nvs_writes;
    CHECK(pb_policy_set_auto(
        55.0f, 90.0f, PB_SOURCE_WEB, PB_POLICY_REVISION_ANY) == PB_POLICY_OK);
    CHECK(pb_policy_persist_pending());
    CHECK(nvs_read("md_auto_tgt") == 5500);
    CHECK(nvs_read("md_auto_bed") == 9000);
    // Only the two AUTO keys changed -- md_last must not be rewritten.
    CHECK(nvs_writes - before == 2);

    CHECK(pb_policy_start_drying(
        50.0f, 3, PB_SOURCE_WEB, PB_POLICY_REVISION_ANY) == PB_POLICY_OK);
    CHECK(pb_policy_persist_pending());
    CHECK(nvs_read("md_dry_tgt") == 5000);
    CHECK(nvs_read("md_dry_hrs") == 3);
}

static void test_params_persist_canonical_post_clamp_value(void)
{
    reset_fixture();
    pb_policy_load_params();
    // Ceiling below the request, and distinct from the 50 C default so a write
    // is genuinely required.
    heater_max_target_c = 55.0f;

    CHECK(pb_policy_set_power_on(
        70.0f, PB_SOURCE_WEB, "tab", PB_POLICY_REVISION_ANY, NULL)
        == PB_POLICY_OK);
    CHECK(pb_policy_persist_pending());
    // The clamped value reaches flash, not the raw 70 that was requested.
    CHECK(nvs_read("md_last") == 5500);
    pb_policy_params_t p;
    pb_policy_get_params(&p);
    CHECK(p.manual_target_c == 55.0f);
    CHECK(snapshot().params.manual_target_c == 55.0f);
}

static void test_params_failed_write_stays_dirty_and_retries(void)
{
    reset_fixture();
    pb_policy_load_params();

    nvs_write_fails = true;
    CHECK(pb_policy_set_power_on(
        65.0f, PB_SOURCE_WEB, "tab", PB_POLICY_REVISION_ANY, NULL)
        == PB_POLICY_OK);
    CHECK(pb_policy_persist_pending());     // attempted
    CHECK(!nvs_has("md_last"));             // but nothing landed
    CHECK(nvs_commits == 0);                // and no commit on a failed write

    nvs_write_fails = false;
    CHECK(pb_policy_persist_pending());     // still dirty -> retried
    CHECK(nvs_read("md_last") == 6500);
    CHECK(nvs_commits == 1);
    CHECK(!pb_policy_persist_pending());
}

static void test_params_survive_reboot_but_mode_does_not(void)
{
    reset_fixture();
    pb_policy_load_params();
    CHECK(pb_policy_start_drying(
        45.0f, 6, PB_SOURCE_WEB, PB_POLICY_REVISION_ANY) == PB_POLICY_OK);
    CHECK(pb_policy_persist_pending());
    CHECK(snapshot().mode == PB_MODE_DRYING);

    // Reboot: re-init policy WITHOUT clearing the fake flash.
    CHECK(pb_policy_init() == ESP_OK);
    pb_policy_load_params();

    pb_policy_params_t p;
    pb_policy_get_params(&p);
    CHECK(p.dry_target_c == 45.0f);
    CHECK(p.dry_hours == 6);

    // ...but the active mode, target, and deadline are gone.
    pb_policy_snapshot_t snap = snapshot();
    CHECK(snap.mode == PB_MODE_OFF);
    CHECK(snap.source == PB_SOURCE_BOOT);
    CHECK(!snap.drying);
    CHECK(snap.effective_target_c == 0.0f);
    CHECK(!snap.lease_active);
}

// Power = device alive (blink on fault); On/Auto/Dry each carry their own mode,
// with Auto distinguishing armed-but-waiting from actually engaged.
static void test_panel_leds_track_mode(void)
{
    reset_fixture();
    pb_policy_tick();
    CHECK(led_pattern[PB_LED_POWER] == PB_LED_SOLID);
    CHECK(led_pattern[PB_LED_ON] == PB_LED_OFF);
    CHECK(led_pattern[PB_LED_AUTO] == PB_LED_OFF);
    CHECK(led_pattern[PB_LED_DRY] == PB_LED_OFF);

    CHECK(pb_policy_set_power_on(
        45.0f, PB_SOURCE_WEB, "tab", 1, NULL) == PB_POLICY_OK);
    pb_policy_tick();
    CHECK(led_pattern[PB_LED_ON] == PB_LED_SOLID);
    CHECK(led_pattern[PB_LED_AUTO] == PB_LED_OFF);
    CHECK(led_pattern[PB_LED_DRY] == PB_LED_OFF);

    // AUTO armed but not engaged (no printer link) -> slow blink, not solid.
    pb_policy_set_mode_off(PB_SOURCE_WEB);
    CHECK(pb_policy_set_auto(
        60.0f, 100.0f, PB_SOURCE_WEB, PB_POLICY_REVISION_ANY) == PB_POLICY_OK);
    pb_policy_set_env(20.0f, false);
    pb_policy_tick();
    CHECK(led_pattern[PB_LED_AUTO] == PB_LED_BLINK_SLOW);
    CHECK(led_pattern[PB_LED_ON] == PB_LED_OFF);

    pb_policy_set_env(105.0f, true);
    pb_policy_tick();
    CHECK(snapshot().auto_engaged);
    CHECK(led_pattern[PB_LED_AUTO] == PB_LED_SOLID);

    pb_policy_set_mode_off(PB_SOURCE_WEB);
    CHECK(pb_policy_start_drying(
        55.0f, 2, PB_SOURCE_WEB, PB_POLICY_REVISION_ANY) == PB_POLICY_OK);
    pb_policy_tick();
    CHECK(led_pattern[PB_LED_DRY] == PB_LED_SOLID);
    CHECK(led_pattern[PB_LED_AUTO] == PB_LED_OFF);
    CHECK(led_pattern[PB_LED_POWER] == PB_LED_SOLID);

    // A fault forces mode OFF, so every mode LED goes dark and Power blinks.
    pb_heater_emergency_off("test trip");
    pb_policy_tick();
    CHECK(led_pattern[PB_LED_POWER] == PB_LED_BLINK);
    CHECK(led_pattern[PB_LED_ON] == PB_LED_OFF);
    CHECK(led_pattern[PB_LED_AUTO] == PB_LED_OFF);
    CHECK(led_pattern[PB_LED_DRY] == PB_LED_OFF);

    // A permanent inhibit reports as a fault too, so Power must keep blinking.
    heater_fault = false;
    heater_inhibited = true;
    pb_policy_tick();
    CHECK(led_pattern[PB_LED_POWER] == PB_LED_BLINK);
}

static void test_fault_clear_requires_current_revision(void)
{
    reset_fixture();
    heater_fault = true;
    heater_reason = "test fault";
    pb_policy_snapshot_t snap = snapshot();
    CHECK(pb_policy_clear_fault(
        PB_SOURCE_WEB, snap.state_revision + 1) == PB_POLICY_REVISION_CONFLICT);
    CHECK(heater_fault);
    CHECK(pb_policy_clear_fault(
        PB_SOURCE_WEB, snap.state_revision) == PB_POLICY_OK);
    CHECK(!heater_fault);
}

static void test_button_short_toggles_modes(void)
{
    reset_fixture();
    pb_policy_load_params();   // manual=50, auto=60/bed100, dry=60/12h

    // On -> POWER_ON at the remembered manual target, attributed to BUTTON.
    pb_policy_on_button(PB_BUTTON_ON, PB_BUTTON_SHORT);
    CHECK(wake_calls == 1);
    pb_policy_snapshot_t snap = snapshot();
    CHECK(snap.mode == PB_MODE_POWER_ON);
    CHECK(snap.source == PB_SOURCE_BUTTON);
    CHECK(snap.requested_target_c == 50.0f);
    CHECK(!snap.lease_active);
    // Press again -> OFF.
    pb_policy_on_button(PB_BUTTON_ON, PB_BUTTON_SHORT);
    CHECK(wake_calls == 2);
    CHECK(snapshot().mode == PB_MODE_OFF);

    // Auto toggles AUTO with the remembered target + threshold.
    pb_policy_on_button(PB_BUTTON_AUTO, PB_BUTTON_SHORT);
    CHECK(wake_calls == 3);
    snap = snapshot();
    CHECK(snap.mode == PB_MODE_AUTO);
    CHECK(snap.source == PB_SOURCE_BUTTON);
    CHECK(snap.auto_bed_threshold_c == 100.0f);
    pb_policy_on_button(PB_BUTTON_AUTO, PB_BUTTON_SHORT);
    CHECK(wake_calls == 4);
    CHECK(snapshot().mode == PB_MODE_OFF);

    // Dry toggles DRYING.
    pb_policy_on_button(PB_BUTTON_DRY, PB_BUTTON_SHORT);
    CHECK(wake_calls == 5);
    snap = snapshot();
    CHECK(snap.mode == PB_MODE_DRYING);
    CHECK(snap.drying);

    // Power short is master OFF from any mode.
    pb_policy_on_button(PB_BUTTON_POWER, PB_BUTTON_SHORT);
    CHECK(wake_calls == 6);
    CHECK(snapshot().mode == PB_MODE_OFF);
}

static void test_button_power_short_while_off_does_not_bump_revision(void)
{
    reset_fixture();
    pb_policy_load_params();
    uint32_t rev = snapshot().state_revision;
    pb_policy_on_button(PB_BUTTON_POWER, PB_BUTTON_SHORT);   // already OFF
    CHECK(wake_calls == 0);
    CHECK(snapshot().state_revision == rev);                 // evlog only, no bump
    CHECK(snapshot().mode == PB_MODE_OFF);
}

static void test_button_invalidates_remote_lease(void)
{
    reset_fixture();
    pb_policy_load_params();
    pb_policy_lease_t lease;
    CHECK(pb_policy_set_power_on(
        45.0f, PB_SOURCE_KLIPPER, "klippy", 1, &lease) == PB_POLICY_OK);
    CHECK(snapshot().lease_active);
    wake_calls = 0;

    // A physical OFF must drop the lease and beat any later heartbeat.
    pb_policy_on_button(PB_BUTTON_POWER, PB_BUTTON_SHORT);
    CHECK(!snapshot().lease_active);
    CHECK(pb_policy_heartbeat(&lease) == PB_POLICY_STALE_LEASE);
    CHECK(snapshot().source == PB_SOURCE_BUTTON);
    CHECK(wake_calls == 1);
}

static void test_remote_commands_wake_only_when_accepted(void)
{
    reset_fixture();
    pb_policy_load_params();

    CHECK(pb_policy_set_power_on(
        45.0f, PB_SOURCE_WEB, "tab", PB_POLICY_REVISION_ANY, NULL)
        == PB_POLICY_OK);
    CHECK(wake_calls == 1);

    pb_policy_set_mode_off(PB_SOURCE_WEB);
    CHECK(wake_calls == 2);

    CHECK(pb_policy_set_auto(
        50.0f, 60.0f, PB_SOURCE_WEB, PB_POLICY_REVISION_ANY)
        == PB_POLICY_OK);
    CHECK(wake_calls == 3);

    pb_policy_set_mode_off(PB_SOURCE_KLIPPER);
    CHECK(wake_calls == 4);

    CHECK(pb_policy_start_drying(
        50.0f, 2, PB_SOURCE_KLIPPER, PB_POLICY_REVISION_ANY)
        == PB_POLICY_OK);
    CHECK(wake_calls == 5);

    // Rejected commands do not change policy/output state and must not wake.
    heater_fault = true;
    heater_reason = "test fault";
    CHECK(pb_policy_set_power_on(
        45.0f, PB_SOURCE_WEB, "tab", PB_POLICY_REVISION_ANY, NULL)
        == PB_POLICY_FAULT_LATCHED);
    CHECK(wake_calls == 5);

    CHECK(pb_policy_clear_fault(
        PB_SOURCE_WEB, PB_POLICY_REVISION_ANY) == PB_POLICY_OK);
    CHECK(wake_calls == 6);
}

static void test_button_rejection_is_logged_without_wake(void)
{
    reset_fixture();
    pb_policy_load_params();
    heater_fault = true;
    heater_reason = "test fault";

    pb_policy_on_button(PB_BUTTON_AUTO, PB_BUTTON_SHORT);

    CHECK(snapshot().mode == PB_MODE_OFF);
    CHECK(wake_calls == 0);
    CHECK(strcmp(last_evlog, "btn: auto rejected (fault_latched)") == 0);
}

static void test_button_long_press_panic_off(void)
{
    reset_fixture();
    pb_policy_load_params();
    pb_policy_lease_t lease;
    CHECK(pb_policy_set_power_on(
        50.0f, PB_SOURCE_WEB, "tab", 1, &lease) == PB_POLICY_OK);
    pb_policy_tick();
    CHECK(pb_heater_is_on());

    wake_calls = 0;
    pb_policy_on_button(PB_BUTTON_AUTO, PB_BUTTON_LONG);   // any button latches
    pb_policy_snapshot_t snap = snapshot();
    // Attributed to BUTTON, NOT SAFETY -- this is the whole point of routing
    // panic-off through the policy rather than a bare heater latch.
    CHECK(snap.source == PB_SOURCE_BUTTON);
    CHECK(snap.fault_latched);
    CHECK(snap.mode == PB_MODE_OFF);
    CHECK(!snap.lease_active);
    CHECK(wake_calls == 1);                 // control task woken immediately
    CHECK(!panic_logged_before_wake);       // diagnostics cannot delay the wake

    // The wake runs a real tick, which drops the SSR.
    pb_policy_tick();
    CHECK(!pb_heater_is_on());

    // And the tick must NOT re-stamp the transition as SAFETY.
    CHECK(snapshot().source == PB_SOURCE_BUTTON);
}

static void test_button_power_long_clears_or_holds_fault(void)
{
    reset_fixture();
    pb_policy_load_params();

    // Fault with the condition already recovered -> Power long clears it.
    pb_heater_emergency_off("test trip");
    pb_policy_tick();
    CHECK(snapshot().fault_latched);
    pb_policy_on_button(PB_BUTTON_POWER, PB_BUTTON_LONG);
    pb_policy_tick();
    CHECK(!snapshot().fault_latched);
    CHECK(snapshot().mode == PB_MODE_OFF);

    // Fault whose underlying condition is still unsafe -> the clear is attempted
    // but the next tick re-latches; the device stays OFF and faulted.
    heater_relatch = true;
    pb_heater_emergency_off("still hot");
    pb_policy_tick();
    CHECK(snapshot().fault_latched);
    pb_policy_on_button(PB_BUTTON_POWER, PB_BUTTON_LONG);
    pb_policy_tick();
    CHECK(snapshot().fault_latched);        // re-latched
    CHECK(snapshot().mode == PB_MODE_OFF);
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
    test_local_power_limit_expires_off_without_fault();
    test_external_fault_sync_off_and_clear();
    test_fault_clear_requires_current_revision();
    test_panel_leds_track_mode();
    test_params_default_and_load_never_arms();
    test_params_clamp_corrupt_values();
    test_params_load_respects_runtime_max();
    test_params_persist_only_changed_keys();
    test_params_persist_canonical_post_clamp_value();
    test_params_failed_write_stays_dirty_and_retries();
    test_params_survive_reboot_but_mode_does_not();
    test_button_short_toggles_modes();
    test_button_power_short_while_off_does_not_bump_revision();
    test_button_invalidates_remote_lease();
    test_remote_commands_wake_only_when_accepted();
    test_button_rejection_is_logged_without_wake();
    test_button_long_press_panic_off();
    test_button_power_long_clears_or_holds_fault();
    puts("pb_policy host tests: PASS");
    return 0;
}
