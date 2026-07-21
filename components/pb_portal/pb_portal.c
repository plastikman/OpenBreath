// SPDX-License-Identifier: MIT
#include "pb_portal.h"
#include "pb_dns.h"
#include "pb_httpd.h"
#include "pv_wifi.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "lwip/inet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pb_portal";
#define NVS_NS "app_nvs"
#define SEND(req, s) httpd_resp_send_chunk((req), (s), HTTPD_RESP_USE_STRLEN)

// ---- form parsing (application/x-www-form-urlencoded) ----
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void urldecode(char *s)
{
    char *o = s;
    for (char *p = s; *p; ) {
        if (*p == '%' && p[1] && p[2]) {
            int hi = hexval(p[1]), lo = hexval(p[2]);
            if (hi >= 0 && lo >= 0) { *o++ = (char)((hi << 4) | lo); p += 3; continue; }
        }
        if (*p == '+') { *o++ = ' '; p++; continue; }
        *o++ = *p++;
    }
    *o = '\0';
}
static bool form_get(const char *body, const char *key, char *out, size_t outsz)
{
    size_t klen = strlen(key);
    for (const char *p = body; p && *p; ) {
        const char *amp = strchr(p, '&');
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t vlen = amp ? (size_t)(amp - v) : strlen(v);
            if (vlen >= outsz) vlen = outsz - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            urldecode(out);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    if (outsz) out[0] = '\0';
    return false;
}

// ---- handlers ----
static esp_err_t portal_page(httpd_req_t *req)
{
    wifi_ap_record_t recs[PV_WIFI_SCAN_MAX];
    int n = pv_wifi_get_scan_results(recs, PV_WIFI_SCAN_MAX);
    if (n == 0 && !pv_wifi_is_scanning()) pv_wifi_scan_start();

    char mk_host[64] = {0};
    uint16_t mk_port = 7125;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof mk_host;
        nvs_get_str(h, "mk_host", mk_host, &sz);
        nvs_get_u16(h, "mk_port", &mk_port);
        nvs_close(h);
    }

    httpd_resp_set_type(req, "text/html");
    SEND(req, "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
              "<title>OpenBreath Setup</title>"
              "<body style='font-family:sans-serif;max-width:30em;margin:1em auto;padding:0 1em'>"
              "<h2>OpenBreath Setup</h2>"
              "<form method=POST action=/save><h3>Wi-Fi</h3>Network:<br><select name=ssid style='width:100%'>");
    char row[160];
    for (int i = 0; i < n; i++) {
        snprintf(row, sizeof row, "<option>%s</option>", (char *)recs[i].ssid);
        SEND(req, row);
    }
    SEND(req, "</select><br><small>or type SSID (hidden net):</small><br>"
              "<input name=ssid_manual style='width:100%'><br>"
              "Password:<br><input type=password name=password style='width:100%'><br>"
              "<h3>Printer (Moonraker)</h3>Host / IP:<br><input name=mk_host style='width:100%' value='");
    SEND(req, mk_host);
    SEND(req, "'><br>Port:<br><input name=mk_port style='width:100%' value='");
    char pbuf[8]; snprintf(pbuf, sizeof pbuf, "%u", (unsigned)mk_port);
    SEND(req, pbuf);
    SEND(req, "'><br><br><button type=submit style='width:100%;padding:.6em'>Save &amp; Reboot</button></form>"
              "<form method=POST action=/rescan><button style='width:100%;padding:.4em;margin-top:.5em'>Rescan Wi-Fi</button></form>");
    if (n == 0) SEND(req, "<p><small>scanning\xE2\x80\xA6 tap Rescan if the list is empty</small></p>");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t rescan_post(httpd_req_t *req)
{
    pv_wifi_scan_start();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[640];
    int total = 0, r;
    while ((r = httpd_req_recv(req, body + total, sizeof body - 1 - total)) > 0) {
        total += r;
        if (total >= (int)sizeof body - 1) break;
    }
    body[total > 0 ? total : 0] = '\0';

    char ssid[33] = {0}, ssid_manual[33] = {0}, pass[65] = {0};
    char mk_host[64] = {0}, mk_port_s[8] = {0};
    form_get(body, "ssid", ssid, sizeof ssid);
    form_get(body, "ssid_manual", ssid_manual, sizeof ssid_manual);
    form_get(body, "password", pass, sizeof pass);
    form_get(body, "mk_host", mk_host, sizeof mk_host);
    form_get(body, "mk_port", mk_port_s, sizeof mk_port_s);

    const char *chosen = ssid_manual[0] ? ssid_manual : ssid;
    if (chosen[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no Wi-Fi network chosen");
        return ESP_FAIL;
    }

    // Persist Moonraker config first (pv_wifi_save_creds_and_reboot won't return).
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (mk_host[0]) nvs_set_str(h, "mk_host", mk_host);
        int port = atoi(mk_port_s);
        if (port > 0 && port < 65536) nvs_set_u16(h, "mk_port", (uint16_t)port);
        nvs_commit(h);
        nvs_close(h);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='font-family:sans-serif;text-align:center;margin-top:3em'>"
        "<h2>Saved \xE2\x9C\x93</h2><p>Rebooting and joining your Wi-Fi\xE2\x80\xA6</p>"
        "<p>This page will disconnect.</p>");
    ESP_LOGI(TAG, "provisioned SSID='%s' moonraker='%s' — rebooting", chosen, mk_host);
    pv_wifi_save_creds_and_reboot(chosen, pass);   // writes ssid/password + reboots
    return ESP_OK;                                  // unreachable
}

esp_err_t pb_portal_start(void)
{
    httpd_handle_t s = pb_httpd_handle();
    if (s == NULL) return ESP_ERR_INVALID_STATE;

    httpd_uri_t save   = { .uri = "/save",   .method = HTTP_POST, .handler = save_post };
    httpd_uri_t rescan = { .uri = "/rescan", .method = HTTP_POST, .handler = rescan_post };
    httpd_uri_t root   = { .uri = "/*",       .method = HTTP_GET,  .handler = portal_page };
    httpd_register_uri_handler(s, &save);
    httpd_register_uri_handler(s, &rescan);
    httpd_register_uri_handler(s, &root);   // catch-all LAST (captive-portal probes)

    if (pv_wifi_state() == PV_WIFI_STATE_AP_PORTAL) {
        pv_wifi_ap_config_t ap;
        pv_wifi_get_ap_config(&ap);
        pb_dns_start(htonl(ap.ip));          // redirect all lookups to the AP IP
        pv_wifi_scan_start();                // kick an initial network scan
        ESP_LOGI(TAG, "AP captive portal active");
    } else {
        ESP_LOGI(TAG, "config portal available (STA mode)");
    }
    return ESP_OK;
}
