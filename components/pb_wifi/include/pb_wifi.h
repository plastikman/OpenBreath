#pragma once

// Panda Vent WiFi manager: reads saved credentials from NVS at boot, connects
// as STA, and falls back to AP + captive portal if either the credentials are
// missing or the connection fails. Cred storage is compatible with the stock
// firmware's NVS layout (namespace "app_nvs", keys "ssid" / "password").

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

typedef enum {
    PB_WIFI_STATE_INIT,
    PB_WIFI_STATE_STA_CONNECTING,
    PB_WIFI_STATE_STA_CONNECTED,
    PB_WIFI_STATE_AP_PORTAL,   // hosting captive portal for setup
} pb_wifi_state_t;

// AP hotspot configuration, overridable via the portal. Empty strings and
// ip == 0 select the built-in defaults (MAC-derived SSID, "987654321",
// 192.168.4.1). Stored in NVS under app_nvs.
typedef struct {
    char     ssid[33];      // "" → default OpenVent_XXXX
    char     password[65];  // "" → default 987654321 (WPA2-PSK requires ≥ 8 chars)
    uint32_t ip;            // host byte order; 0 → default 192.168.4.1
    bool     enabled;       // false = don't fall back to AP if STA fails
                            // (still start AP if there are no saved STA creds
                            // at all — otherwise the device is unrecoverable)
} pb_wifi_ap_config_t;

#define PB_WIFI_SCAN_MAX 20

// Start the WiFi manager. Non-blocking; state transitions happen async.
esp_err_t pb_wifi_start(void);

// Persist WiFi credentials and reboot into STA mode. Intended to be called
// from the captive-portal HTTP handler after the user submits the form.
esp_err_t pb_wifi_save_creds_and_reboot(const char *ssid, const char *password);

// Wipe saved WiFi credentials.
esp_err_t pb_wifi_clear_creds(void);

pb_wifi_state_t pb_wifi_state(void);

// Kick off an async scan of visible networks. Returns immediately; results
// land in the cache when WIFI_EVENT_SCAN_DONE fires. Safe to call in AP mode
// (driver runs in APSTA so the AP stays up).
esp_err_t pb_wifi_scan_start(void);

// True while a scan is in flight (results not yet populated for this cycle).
bool pb_wifi_is_scanning(void);

// Copy the latest cached scan results into `out` (capacity `max_count`).
// Returns the number of records actually written.
int pb_wifi_get_scan_results(wifi_ap_record_t *out, int max_count);

// AP hotspot config: current effective values (with defaults applied) and
// the setter (persists to NVS + reboots so netif re-inits with the new IP).
esp_err_t pb_wifi_get_ap_config(pb_wifi_ap_config_t *out);
esp_err_t pb_wifi_set_ap_config_and_reboot(const pb_wifi_ap_config_t *cfg);

// AP details, useful for portal DNS/HTTP setup.
#define PB_WIFI_AP_PASSWORD    "987654321"   // matches stock firmware
#define PB_WIFI_AP_SSID_PREFIX "OpenVent_"
#define PB_WIFI_HOSTNAME       "OpenVent"    // resolves as OpenVent.local via mDNS
