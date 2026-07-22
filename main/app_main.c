// SPDX-License-Identifier: MIT
// OpenBreath — open firmware for the BIGTREETECH Panda Breath (ESP32-C3).
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
#include "nvs_flash.h"
#include "nvs.h"
#include <stdbool.h>

#include "pb_board.h"
#include "pb_ntc.h"
#include "pb_heater.h"
#include "pb_fan.h"
#include "pb_policy.h"

#include "esp_wifi.h"
#include "esp_mac.h"
#include "pv_wifi.h"
#include "pv_moonraker.h"

#include "pb_httpd.h"
#include "pb_portal.h"

// Optional local dev config (gitignored): WiFi creds + Moonraker host. Without it
// the build still works — the device just comes up without network credentials.
#if defined(__has_include)
#  if __has_include("dev_config.h")
#    include "dev_config.h"
#  endif
#endif

static const char *TAG = "openbreath";

#define PB_TICK_PERIOD_MS 500

// Set true once the network components have been started, so the control loop
// doesn't touch pv_* state before it's initialized.
static volatile bool s_net_up = false;

// Brand the captive-portal AP as "OpenPanda_XXXX". The shared pv_wifi reads the
// AP SSID from NVS (key "ap_ssid"), so we override its "OpenVent_" default this
// way without patching the shared component. (mDNS hostname stays OpenVent.local
// until pv_wifi is made configurable upstream.)
static void brand_ap(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[33];
    snprintf(ssid, sizeof ssid, "OpenPanda_%02X%02X", mac[4], mac[5]);
    nvs_handle_t h;
    if (nvs_open("app_nvs", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ap_ssid", ssid);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "AP SSID branded: %s", ssid);
    }
}

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
}

#if defined(OB_WIFI_SSID) || defined(OB_MOONRAKER_HOST)
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
#ifdef OB_WIFI_SSID
    nvs_set_str(h, "ssid", OB_WIFI_SSID);
    nvs_set_str(h, "password", OB_WIFI_PASS);
#endif
#ifdef OB_MOONRAKER_HOST
    nvs_set_str(h, "mk_host", OB_MOONRAKER_HOST);
    nvs_set_u16(h, "mk_port", OB_MOONRAKER_PORT);
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
    for (;;) {
        // Safety/control loop: enforces every heater cutoff + fan-follows-heater.
        // The chamber setpoint is owned by the HTTP API (pb_httpd -> pb_heater),
        // and the comms watchdog is fed by controller polls in pb_httpd — so we
        // must NOT set the target here (that would clobber the HTTP-set value).
        pb_policy_tick();

        if (++dbg >= 4) {   // ~2 s
            dbg = 0;
            bool net = s_net_up;
            pv_moonraker_status_t st = {0};
            if (net) pv_moonraker_get_status(&st);
            uint32_t zc = 0, zciv = 0;
            pb_fan_zc_diag(&zc, &zciv);
            ESP_LOGI(TAG,
                "target=%.0fC heater=%s | chamber=%.1fC ptc=%.1fC | "
                "wifi=%d mk=%d printer=%s bed=%.1f | ZC n=%lu dt=%luus",
                pb_heater_get_target_c(), pb_heater_is_on() ? "ON" : "off",
                pb_ntc_smoothed_c(PB_NTC_CHAMBER), pb_ntc_smoothed_c(PB_NTC_PTC),
                net ? (int)pv_wifi_state() : -1, (int)st.state,
                pv_printer_state_str(st.printer), st.bed_temp,
                (unsigned long)zc, (unsigned long)zciv);
        }
        vTaskDelayUntil(&last, period);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "OpenBreath starting");

    pb_board_init();
    ESP_ERROR_CHECK(pb_heater_init());     // SSR forced OFF before anything else
    ESP_ERROR_CHECK(pb_ntc_init());
    ESP_ERROR_CHECK(pb_fan_init());
    ESP_ERROR_CHECK(pb_policy_init());

    // Start the safety/telemetry loop FIRST — it must run regardless of the
    // network coming up (a blocking/hung network stack must never stop it).
    xTaskCreate(control_task, "pb_control", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "control loop running; heater held OFF (bring-up: no auto-heat)");

    // Bring up networking. If a start call blocks under a flaky link, the control
    // loop above is already running, so safety + telemetry continue.
    nvs_init();
    brand_ap();                              // AP name = OpenPanda_XXXX
#if defined(OB_WIFI_SSID) || defined(OB_MOONRAKER_HOST)
    seed_dev_config();
#endif
    ESP_ERROR_CHECK(pv_wifi_start());
    // Mains-powered device: disable WiFi modem-sleep so the control API stays
    // responsive (power-save adds ~0.5s latency spikes to incoming requests).
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(pv_moonraker_start());   // loads mk_host/mk_port from NVS, connects
    ESP_ERROR_CHECK(pb_httpd_start());       // HTTP control API (the Klipper module talks to this)
    ESP_ERROR_CHECK(pb_portal_start());      // config + captive portal on the same server
    s_net_up = true;
    ESP_LOGI(TAG, "networking up (wifi + moonraker + http api + portal)");
}
