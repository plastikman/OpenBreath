// SPDX-License-Identifier: MIT
// DragonBreath — open firmware for the BIGTREETECH Panda Breath (ESP32-C3).
//
// Safety-first init: the heater SSR is forced OFF before anything can request
// heat, and the control/telemetry loop is started BEFORE networking so it runs
// regardless of WiFi/Moonraker state (network bring-up must never gate the safety
// loop). Networking uses the OpenVent shared core (pv_wifi + pv_moonraker):
// connect to WiFi, dial into the printer's Moonraker over WebSocket, feed printer
// state to pb_policy.
//
// BRING-UP PHASE: connect + read + log printer state, but DO NOT auto-heat yet
// (chamber target held at 0) until the heat policy is defined.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdbool.h>
#include <string.h>

#include "pb_board.h"
#include "pb_ntc.h"
#include "pb_heater.h"
#include "pb_fan.h"
#include "pb_policy.h"
#include "pb_leds.h"
#include "pb_hil.h"

#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "mdns.h"
#include "pv_wifi.h"
#include "pv_moonraker.h"

#include "pb_httpd.h"
#include "pb_portal.h"
#include "esp_ota_ops.h"

// Optional local dev config (gitignored): WiFi creds + Moonraker host. Without it
// the build still works — the device just comes up without network credentials.
#if defined(__has_include)
#  if __has_include("dev_config.h")
#    include "dev_config.h"
#  endif
#endif

static const char *TAG = "dragonbreath";

#define PB_TICK_PERIOD_MS 500

// Set true once the network components have been started, so the control loop
// doesn't touch pv_* state before it's initialized.
static volatile bool s_net_up = false;
// Set true only if pv_moonraker_start() succeeded — never query a client that
// failed to initialize (its internal state/mutex may be unset).
static volatile bool s_mk_up = false;

// Brand the captive-portal AP as "DragonBreath_XXXX". The shared pv_wifi reads the
// AP SSID from NVS (key "ap_ssid"), so we override its "OpenVent_" default this way
// without patching the shared component. We also migrate the previous "OpenPanda_"
// default (pre-DragonBreath rebrand) so an already-provisioned device adopts the new
// name, while preserving any user-customized SSID. (mDNS hostname is overridden
// separately in brand_hostname().)
#ifndef CONFIG_PB_HIL_DEVBOARD
static void brand_ap(void)
{
    nvs_handle_t h;
    if (nvs_open("app_nvs", NVS_READWRITE, &h) != ESP_OK) return;
    // Set the default once (avoid NVS flash wear each boot) and never clobber a
    // user-customized name — but DO migrate the legacy "OpenPanda_" default.
    char cur[33] = {0};
    size_t sz = sizeof cur;
    esp_err_t r = nvs_get_str(h, "ap_ssid", cur, &sz);
    if (r == ESP_OK && sz > 1 && strncmp(cur, "OpenPanda_", 10) != 0) {
        nvs_close(h);
        return;
    }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[33];
    snprintf(ssid, sizeof ssid, "DragonBreath_%02X%02X", mac[4], mac[5]);
    nvs_set_str(h, "ap_ssid", ssid);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "AP SSID default set: %s", ssid);
}

// Override the shared pv_wifi mDNS/netif hostname ("OpenVent") so the device
// advertises as dragonbreath.local, WITHOUT patching the OpenVent submodule.
// Call AFTER pv_wifi_start() (which runs mdns_init + sets the OpenVent default);
// these calls just update the already-registered records. Best-effort / log-only:
// a failure only means the device keeps the OpenVent.local name — it never affects
// the safety loop or WiFi. (Interim until the shared core exposes a hostname API —
// see plans/rebrand-dragonbreath.md.)
static void brand_hostname(void)
{
    static const char *HN = "dragonbreath";
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_t *ap  = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (sta) esp_netif_set_hostname(sta, HN);
    if (ap)  esp_netif_set_hostname(ap, HN);
    mdns_hostname_set(HN);
    mdns_instance_name_set(HN);
    mdns_service_instance_name_set("_http", "_tcp", HN);
    ESP_LOGI(TAG, "mDNS hostname override: %s.local", HN);
}
#endif

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
}

#if defined(DB_WIFI_SSID) || defined(DB_MOONRAKER_HOST)
// Dev-only: seed WiFi creds + Moonraker config into the NVS layout the shared
// components load at start (namespace app_nvs; keys ssid/password + mk_host/mk_port).
// This is what the portal would normally write. IMPORTANT: seed via NVS and let
// pv_moonraker_start() load it — do NOT call pv_moonraker_set_config() before
// _start(): set_config takes an internal mutex that _start() creates, so calling
// it first dereferences a NULL semaphore handle (asserts / reboot loop).
static void seed_dev_config(void)
{
    nvs_handle_t h;
    if (nvs_open("app_nvs", NVS_READWRITE, &h) != ESP_OK) return;
    // Only seed if not already provisioned — so captive-portal / user-entered
    // creds always win over the dev default.
    size_t sz = 0;
    if (nvs_get_str(h, "ssid", NULL, &sz) == ESP_OK && sz > 1) {
        nvs_close(h);
        ESP_LOGI(TAG, "WiFi creds already in NVS; not seeding dev_config");
        return;
    }
#ifdef DB_WIFI_SSID
    nvs_set_str(h, "ssid", DB_WIFI_SSID);
    nvs_set_str(h, "password", DB_WIFI_PASS);
#endif
#ifdef DB_MOONRAKER_HOST
    nvs_set_str(h, "mk_host", DB_MOONRAKER_HOST);
    nvs_set_u16(h, "mk_port", DB_MOONRAKER_PORT);
#endif
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "seeded dev config into NVS (app_nvs)");
}
#endif

static void control_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(PB_TICK_PERIOD_MS);
    TickType_t last = xTaskGetTickCount();
    int dbg = 0;

    // Subscribe this task to the task watchdog so a hung control loop panics
    // (-> reboot -> heater off on boot) rather than silently stalling. Check the
    // result so we don't silently claim coverage that isn't actually armed.
    bool wdt_armed = (esp_task_wdt_add(NULL) == ESP_OK);
    if (!wdt_armed) {
        // Fail closed: without watchdog coverage a hung control loop could leave
        // the SSR energized undetected, so refuse to heat for the rest of this
        // boot. This is a PERMANENT inhibit — API clear_fault cannot clear it (unlike
        // a normal fault); only a power cycle (which re-attempts enrollment) can.
        ESP_LOGE(TAG, "task WDT subscribe FAILED — inhibiting heat (reboot required)");
        pb_heater_inhibit("task watchdog unavailable");
    }

    for (;;) {
        pv_moonraker_status_t st = {0};
#ifndef CONFIG_PB_HIL_DEVBOARD
        if (s_net_up && s_mk_up) pv_moonraker_get_status(&st);
        bool mk_connected = s_mk_up && st.state == PV_MK_SUBSCRIBED;
        pb_policy_set_env(st.bed_temp, mk_connected);
#endif

        // Safety/control loop: enforces every heater cutoff + fan-follows-heater.
        // pb_policy is the sole mode/target writer. Network clients and local
        // inputs submit commands to it; this task applies the resulting outputs.
        pb_policy_tick();
        if (wdt_armed) esp_task_wdt_reset();   // successful loop iteration

        if (++dbg >= 4) {   // ~2 s
            dbg = 0;
            bool net = s_net_up;
            pb_policy_snapshot_t snap;
            pb_policy_get_snapshot(&snap);
            uint32_t zc = 0, zciv = 0;
            pb_fan_zc_diag(&zc, &zciv);
            ESP_LOGI(TAG,
                "rev=%lu mode=%s source=%s target=%.0fC heater=%s | chamber=%.1fC ptc=%.1fC | "
                "wifi=%d mk=%d printer=%s bed=%.1f | ZC n=%lu dt=%luus",
                (unsigned long)snap.state_revision,
                pb_policy_mode_str(snap.mode), pb_policy_source_str(snap.source),
                snap.effective_target_c, snap.heater_output ? "ON" : "off",
                snap.chamber_c, snap.ptc_c,
                net ? (int)pv_wifi_state() : -1, (int)st.state,
                pv_printer_state_str(st.printer), st.bed_temp,
                (unsigned long)zc, (unsigned long)zciv);
        }
        vTaskDelayUntil(&last, period);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "DragonBreath starting");

    pb_board_init();
    ESP_ERROR_CHECK(pb_heater_init());     // SSR forced OFF before anything else
    ESP_ERROR_CHECK(pb_ntc_init());
    ESP_ERROR_CHECK(pb_fan_init());
    ESP_ERROR_CHECK(pb_policy_init());
    ESP_ERROR_CHECK(pb_leds_start());       // indicator LEDs (pb_policy drives them)
#ifdef CONFIG_PB_HIL_CONSOLE
    ESP_ERROR_CHECK(pb_hil_start());
#endif

    // Start the safety/telemetry loop FIRST — it must run regardless of the
    // network coming up (a blocking/hung network stack must never stop it).
    xTaskCreate(control_task, "pb_control", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "control loop running; heater held OFF (bring-up: no auto-heat)");

    // Bring up networking. If a start call blocks under a flaky link, the control
    // loop above is already running, so safety + telemetry continue.
    nvs_init();
    pb_heater_load_config();                 // apply persisted max-target + comms timeout (NVS is up now)
    pb_policy_load_params();                 // remembered mode params (never a mode/target — boot stays OFF)
#ifndef CONFIG_PB_HIL_DEVBOARD
    brand_ap();                              // AP name = DragonBreath_XXXX
#if defined(DB_WIFI_SSID) || defined(DB_MOONRAKER_HOST)
    seed_dev_config();
#endif
    // Network bring-up is LOG-AND-CONTINUE, never ESP_ERROR_CHECK: a transient
    // init error (e.g. httpd_start NO_MEM under boot heap pressure) must not
    // abort/reboot and tear down the safety loop that's already running above.
    esp_err_t e;
    if ((e = pv_wifi_start()) != ESP_OK)
        ESP_LOGE(TAG, "pv_wifi_start: %s (continuing; safety loop unaffected)", esp_err_to_name(e));
    else
        brand_hostname();                    // advertise as dragonbreath.local
    // Mains-powered device: disable WiFi modem-sleep so the control API stays
    // responsive (power-save adds ~0.5s latency spikes to incoming requests).
    esp_wifi_set_ps(WIFI_PS_NONE);
    if ((e = pv_moonraker_start()) != ESP_OK)
        ESP_LOGE(TAG, "pv_moonraker_start: %s (continuing; will not query moonraker)", esp_err_to_name(e));
    else
        s_mk_up = true;
    if ((e = pb_httpd_start()) != ESP_OK)
        ESP_LOGE(TAG, "pb_httpd_start: %s (continuing)", esp_err_to_name(e));
    else if ((e = pb_portal_start()) != ESP_OK)   // portal needs the httpd handle
        ESP_LOGE(TAG, "pb_portal_start: %s (continuing)", esp_err_to_name(e));

    s_net_up = true;
    ESP_LOGI(TAG, "network bring-up done (wifi + moonraker + http api + portal, best-effort)");
#else
    ESP_LOGW(TAG, "HIL dev-board target: network stack skipped; inject env over serial");
#endif

    // OTA rollback confirm: we reached a healthy state (safety loop running, init
    // complete), so mark this image valid and cancel the pending-verify rollback.
    // A future web-flashed image that crashes before here reverts to the last
    // good app on reboot. No-op unless we actually booted in PENDING_VERIFY.
    esp_ota_mark_app_valid_cancel_rollback();
}
