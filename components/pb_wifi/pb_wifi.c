#include "pb_wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/inet.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "pb_wifi";

#define NVS_NS      "app_nvs"    // matches stock firmware
#define KEY_SSID    "ssid"
#define KEY_PASS    "password"
#define KEY_AP_SSID "ap_ssid"
#define KEY_AP_PASS "ap_pass"
#define KEY_AP_IP   "ap_ip"
#define KEY_AP_EN   "ap_enabled"

#define STA_MAX_RETRIES  5
#define BIT_CONNECTED    BIT0
#define BIT_FAILED       BIT1

#define DEFAULT_AP_IP  0xC0A80401U   // 192.168.4.1

static pb_wifi_state_t s_state = PB_WIFI_STATE_INIT;
static EventGroupHandle_t s_events = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static int s_retry = 0;
static bool s_mdns_started = false;

// Scan cache — mutex-protected because on_wifi_event fires on the WiFi task
// but pb_wifi_get_scan_results is called from the httpd task.
static SemaphoreHandle_t s_scan_lock = NULL;
static wifi_ap_record_t s_scan_cache[PB_WIFI_SCAN_MAX];
static int              s_scan_count = 0;
static bool             s_scanning   = false;

static esp_err_t start_mdns(void)
{
    if (s_mdns_started) return ESP_OK;

    esp_err_t err = mdns_init();
    if (err != ESP_OK) return err;

    err = mdns_hostname_set(PB_WIFI_HOSTNAME);
    if (err != ESP_OK) return err;

    err = mdns_instance_name_set(PB_WIFI_HOSTNAME);
    if (err != ESP_OK) return err;

    err = mdns_service_add(PB_WIFI_HOSTNAME, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) return err;

    s_mdns_started = true;
    ESP_LOGI(TAG, "mDNS hostname: %s.local", PB_WIFI_HOSTNAME);
    return ESP_OK;
}

// ---------- NVS helpers ----------

static esp_err_t nvs_read_str(const char *key, char *out, size_t out_sz)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = out_sz;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_write_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool load_saved_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    esp_err_t err = nvs_read_str(KEY_SSID, ssid, ssid_sz);
    if (err != ESP_OK || ssid[0] == '\0') return false;
    err = nvs_read_str(KEY_PASS, pass, pass_sz);
    if (err != ESP_OK) pass[0] = '\0';  // open network is legal
    return true;
}

// ---------- AP mode ----------

static void build_default_ap_ssid(char *out, size_t out_sz)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(out, out_sz, "%s%02X%02X", PB_WIFI_AP_SSID_PREFIX, mac[4], mac[5]);
}

// Populate `out` with the effective AP config: NVS values if set, defaults
// otherwise (MAC-derived SSID, hardcoded password, 192.168.4.1).
static void load_ap_config(pb_wifi_ap_config_t *out)
{
    memset(out, 0, sizeof(*out));

    if (nvs_read_str(KEY_AP_SSID, out->ssid, sizeof(out->ssid)) != ESP_OK ||
        out->ssid[0] == '\0') {
        build_default_ap_ssid(out->ssid, sizeof(out->ssid));
    }
    if (nvs_read_str(KEY_AP_PASS, out->password, sizeof(out->password)) != ESP_OK ||
        out->password[0] == '\0') {
        strncpy(out->password, PB_WIFI_AP_PASSWORD, sizeof(out->password) - 1);
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t ip = 0;
        if (nvs_get_u32(h, KEY_AP_IP, &ip) != ESP_OK || ip == 0) ip = DEFAULT_AP_IP;
        out->ip = ip;
        uint8_t en = 1;
        if (nvs_get_u8(h, KEY_AP_EN, &en) != ESP_OK) en = 1;   // default on
        out->enabled = (en != 0);
        nvs_close(h);
    } else {
        out->ip = DEFAULT_AP_IP;
        out->enabled = true;
    }
}

// Reassign the AP netif's IP + DHCP pool. Must happen while the AP is down.
static esp_err_t apply_ap_ip(uint32_t ip_host_order)
{
    if (s_ap_netif == NULL) return ESP_ERR_INVALID_STATE;
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_ap_netif));

    esp_netif_ip_info_t info = {
        .ip.addr      = htonl(ip_host_order),
        .netmask.addr = htonl(0xFFFFFF00U),   // /24
        .gw.addr      = htonl(ip_host_order),
    };
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &info));

    // Advertise ourselves as the DNS server via DHCP option 6. Without this,
    // clients that joined never learn where our fake DNS is, iOS/Android
    // captive-portal probes go to their default DNS (unreachable from our
    // subnet), and the "Sign in to network" banner never fires.
    dhcps_offer_t offer_dns = OFFER_DNS;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(
        s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
        &offer_dns, sizeof(offer_dns)));
    esp_netif_dns_info_t dns = {
        .ip = { .u_addr.ip4.addr = info.ip.addr, .type = IPADDR_TYPE_V4 },
    };
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns));

    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));
    return ESP_OK;
}

static void start_ap_mode(void)
{
    ESP_LOGI(TAG, "starting AP + captive portal");
    // Flip state first so any STA disconnect events firing during the driver
    // restart don't fall into the retry branch and re-issue esp_wifi_connect.
    s_state = PB_WIFI_STATE_AP_PORTAL;
    ESP_ERROR_CHECK(esp_wifi_stop());

    pb_wifi_ap_config_t cfg;
    load_ap_config(&cfg);
    apply_ap_ip(cfg.ip);

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid,     cfg.ssid,     sizeof(ap.ap.ssid) - 1);
    strncpy((char *)ap.ap.password, cfg.password, sizeof(ap.ap.password) - 1);
    ap.ap.ssid_len       = strlen((const char *)ap.ap.ssid);
    ap.ap.channel        = 1;
    ap.ap.max_connection = 4;
    // Open network is legal too, but WPA2 requires ≥ 8 chars for the password.
    ap.ap.authmode = strlen(cfg.password) >= 8 ? WIFI_AUTH_WPA2_PSK
                                               : WIFI_AUTH_OPEN;

    // APSTA so pb_wifi_scan_start() can enumerate networks without dropping
    // the portal AP off the air.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_state = PB_WIFI_STATE_AP_PORTAL;
    // uint32_t is 'long unsigned' under IDF 5.3's toolchain — cast per octet
    // so %u picks up plain unsigned int.
    ESP_LOGI(TAG, "AP SSID=%s ip=%u.%u.%u.%u",
             ap.ap.ssid,
             (unsigned)((cfg.ip >> 24) & 0xFF),
             (unsigned)((cfg.ip >> 16) & 0xFF),
             (unsigned)((cfg.ip >>  8) & 0xFF),
             (unsigned)( cfg.ip        & 0xFF));
    // Portal is started by app_main after pb_wifi_start returns.
}

// ---------- STA mode ----------

static void start_sta_mode(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "connecting to %s", ssid);

    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid,     ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    s_state = PB_WIFI_STATE_STA_CONNECTING;
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ---------- Event handlers ----------

static void handle_scan_done(void)
{
    // Land the results directly in the cache — the event-loop task's stack
    // (default 2304 B) can't spare 1.6 KB for a local wifi_ap_record_t[20].
    uint16_t count = PB_WIFI_SCAN_MAX;
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, s_scan_cache);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_get_ap_records: %s", esp_err_to_name(err));
        count = 0;
    }
    s_scan_count = count;
    s_scanning = false;
    xSemaphoreGive(s_scan_lock);
    ESP_LOGI(TAG, "scan done: %d networks", (int)count);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != WIFI_EVENT) return;

    if (id == WIFI_EVENT_STA_START) {
        // Only auto-connect if we're actually trying to be a station.
        if (s_state == PB_WIFI_STATE_STA_CONNECTING) esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == PB_WIFI_STATE_STA_CONNECTING || s_state == PB_WIFI_STATE_STA_CONNECTED) {
            if (s_retry < STA_MAX_RETRIES) {
                s_retry++;
                ESP_LOGW(TAG, "STA disconnect; retry %d/%d", s_retry, STA_MAX_RETRIES);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "STA gave up; falling back to portal");
                xEventGroupSetBits(s_events, BIT_FAILED);
            }
        }
    } else if (id == WIFI_EVENT_SCAN_DONE) {
        handle_scan_done();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry = 0;
        s_state = PB_WIFI_STATE_STA_CONNECTED;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

// ---------- Public API ----------

esp_err_t pb_wifi_start(void)
{
    // NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else if (err != ESP_OK) {
        return err;
    }

    // Netif + event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) return ESP_FAIL;

    ESP_ERROR_CHECK(esp_netif_set_hostname(s_sta_netif, PB_WIFI_HOSTNAME));
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_ap_netif, PB_WIFI_HOSTNAME));
    ESP_ERROR_CHECK(start_mdns());

    // WiFi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    s_events = xEventGroupCreate();
    s_scan_lock = xSemaphoreCreateMutex();
    if (s_scan_lock == NULL) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL));

    char ssid[33] = {0};
    char pass[65] = {0};
    if (load_saved_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        start_sta_mode(ssid, pass);
        // Wait for a decision. STA_MAX_RETRIES * ~4 s each ≈ 20 s of real work,
        // plus a generous margin so a slow-associating AP still has room.
        EventBits_t bits = xEventGroupWaitBits(
            s_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(30000));
        if (!(bits & BIT_CONNECTED)) {
            pb_wifi_ap_config_t ap_cfg;
            load_ap_config(&ap_cfg);
            if (ap_cfg.enabled) {
                ESP_LOGW(TAG, "STA never came up (bits=0x%x) — falling back to AP",
                         (unsigned)bits);
                start_ap_mode();
            } else {
                ESP_LOGW(TAG, "STA never came up; AP fallback is disabled — "
                              "letting the driver keep retrying in the background");
                // Leave s_state == STA_CONNECTING; the wifi driver will keep
                // its own auto-reconnect running.
            }
        }
    } else {
        ESP_LOGI(TAG, "no saved WiFi credentials");
        start_ap_mode();
    }
    return ESP_OK;
}

esp_err_t pb_wifi_save_creds_and_reboot(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "saving creds for SSID=%s; rebooting", ssid);
    esp_err_t err = nvs_write_str(KEY_SSID, ssid);
    if (err == ESP_OK) err = nvs_write_str(KEY_PASS, password ? password : "");
    if (err != ESP_OK) return err;

    // Give the HTTP response a moment to flush before we reboot.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;   // unreachable
}

esp_err_t pb_wifi_clear_creds(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, KEY_SSID);
    nvs_erase_key(h, KEY_PASS);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

pb_wifi_state_t pb_wifi_state(void) { return s_state; }

// ---------- Scan ----------

esp_err_t pb_wifi_scan_start(void)
{
    if (s_scan_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    if (s_scanning) {
        xSemaphoreGive(s_scan_lock);
        return ESP_OK;   // scan already in flight — coalesce
    }
    s_scanning = true;
    xSemaphoreGive(s_scan_lock);

    wifi_scan_config_t cfg = {0};   // all channels, active scan, no ssid filter
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        xSemaphoreTake(s_scan_lock, portMAX_DELAY);
        s_scanning = false;
        xSemaphoreGive(s_scan_lock);
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(err));
    }
    return err;
}

bool pb_wifi_is_scanning(void)
{
    if (s_scan_lock == NULL) return false;
    bool r;
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    r = s_scanning;
    xSemaphoreGive(s_scan_lock);
    return r;
}

int pb_wifi_get_scan_results(wifi_ap_record_t *out, int max_count)
{
    if (out == NULL || max_count <= 0 || s_scan_lock == NULL) return 0;
    int n;
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    n = s_scan_count < max_count ? s_scan_count : max_count;
    memcpy(out, s_scan_cache, n * sizeof(wifi_ap_record_t));
    xSemaphoreGive(s_scan_lock);
    return n;
}

// ---------- AP config ----------

esp_err_t pb_wifi_get_ap_config(pb_wifi_ap_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    load_ap_config(out);
    return ESP_OK;
}

esp_err_t pb_wifi_set_ap_config_and_reboot(const pb_wifi_ap_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    // Empty string clears the entry so the default reapplies.
    if (cfg->ssid[0] == '\0') nvs_erase_key(h, KEY_AP_SSID);
    else                      nvs_set_str(h, KEY_AP_SSID, cfg->ssid);
    if (cfg->password[0] == '\0') nvs_erase_key(h, KEY_AP_PASS);
    else                          nvs_set_str(h, KEY_AP_PASS, cfg->password);
    if (cfg->ip == 0) nvs_erase_key(h, KEY_AP_IP);
    else              nvs_set_u32(h, KEY_AP_IP, cfg->ip);
    nvs_set_u8(h, KEY_AP_EN, cfg->enabled ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "AP config saved; rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;   // unreachable
}
