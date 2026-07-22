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
// Non-empty guard: httpd_resp_send_chunk() with a 0-length string terminates
// the chunked response early, so we must never send an empty chunk.
#define SEND(req, s) do { const char *_s = (s); if (_s && _s[0]) httpd_resp_send_chunk((req), _s, HTTPD_RESP_USE_STRLEN); } while (0)

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

// ---- static page pieces ----
// Styled to match the stock BIQU Panda Breath web UI (dark #272525 page, #333
// rounded cards, Arial, blue accent) — palette lifted from the stock firmware,
// with the primary accent switched from stock red to the stock's blue (#4087FE).
static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>OpenPanda Setup</title><style>"
    ":root{--bg:#272525;--card:#333;--accent:#4087FE;--text:#F0F0F0;--input:#2c2c2c;--border:rgba(255,255,255,.12);color-scheme:dark}"
    "*{box-sizing:border-box}"
    "body{margin:0;background:var(--bg);font-family:Arial,Helvetica,sans-serif;color:var(--text)}"
    ".hdr{background:linear-gradient(135deg,#4087FE 0%,#0a2e6b 100%);padding:22px 16px 18px;text-align:center}"
    ".hdr h1{margin:0;font-size:1.4rem}.hdr p{margin:.25em 0 0;font-size:.8rem;color:#cdddff}"
    ".wrap{max-width:27em;margin:0 auto;padding:16px}"
    ".card{background:var(--card);border-radius:16px;padding:16px;margin:16px 0}"
    ".card h2{margin:0 0 .2em;font-size:1.1rem;font-weight:600;color:var(--accent)}"
    "label{display:block;margin:.85em 0 .3em;font-size:.8rem;color:#bdbdbd}"
    "input,select{width:100%;padding:12px 14px;font-size:1rem;background:var(--input);color:var(--text);"
    "border:1px solid var(--border);border-radius:8px}"
    "input:focus,select:focus{outline:none;border-color:var(--accent)}"
    ".pw{display:flex;gap:8px}.pw input{flex:1}"
    ".pw button{width:52px;flex:none;background:var(--input);border:1px solid var(--border);border-radius:8px;font-size:1.2rem;cursor:pointer}"
    "button.go{width:100%;padding:14px;margin-top:6px;border:0;border-radius:10px;background:var(--accent);color:#fff;font-size:1.05rem;font-weight:600;cursor:pointer}"
    "button.sec{width:100%;padding:10px;margin-top:12px;border:1px solid var(--border);border-radius:8px;background:#3a3a3a;color:#ddd;cursor:pointer}"
    "small{color:#8a8a8a}</style></head><body>"
    "<div class=hdr><h1>\xF0\x9F\x90\xBC OpenPanda</h1><p>Panda Breath \xC2\xB7 Wi-Fi Setup</p></div>"
    "<div class=wrap><form method=POST action=/save>"
    "<div class=card><h2>Wi-Fi</h2>"
    "<label>Network</label><select id=ssid name=ssid><option value=''>scanning\xE2\x80\xA6</option></select>"
    "<label>\xE2\x80\xA6 or hidden SSID</label><input name=ssid_manual placeholder='(optional)'>"
    "<label>Password</label><div class=pw><input id=pw type=password name=password autocomplete=off>"
    "<button type=button id=eye onclick='togglePw()' aria-label='show password'>\xF0\x9F\x91\x81</button></div>"
    "<button type=button class=sec onclick='rescan()'>Rescan networks</button></div>";

static const char PAGE_TAIL[] =
    "<button type=submit class=go>Save &amp; Connect</button></form>"
    "<p style='text-align:center'><small>The device reboots and joins your network after saving.</small></p></div>"
    "<script>"
    "function togglePw(){var p=document.getElementById('pw'),e=document.getElementById('eye');"
    "var s=p.type==='password';p.type=s?'text':'password';e.textContent=s?'\xF0\x9F\x99\x88':'\xF0\x9F\x91\x81';}"
    "function fill(l){var s=document.getElementById('ssid'),c=s.value;s.innerHTML='';"
    "if(!l.length){s.innerHTML='<option value=\"\">(none found \xE2\x80\x94 tap Rescan)</option>';return;}"
    "l.forEach(function(n){var o=document.createElement('option');o.textContent=n;o.value=n;s.appendChild(o);});"
    "if(c)s.value=c;}"
    "function load(){fetch('/scan.json').then(function(r){return r.json();}).then(fill).catch(function(){});}"
    "function rescan(){fetch('/rescan',{method:'POST'}).then(function(){setTimeout(load,1800);});}"
    "load();setInterval(load,4000);"
    "</script></body></html>";

// ---- handlers ----
static esp_err_t portal_page(httpd_req_t *req)
{
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
    SEND(req, PAGE_HEAD);

    // Moonraker card — values embedded so we never emit an empty chunk.
    char card[320];
    snprintf(card, sizeof card,
        "<div class=card><h2>Printer (Moonraker)</h2>"
        "<label>Host / IP</label><input name=mk_host value=\"%s\" placeholder='e.g. 10.168.2.34'>"
        "<label>Port</label><input name=mk_port value=\"%u\"></div>",
        mk_host, (unsigned)mk_port);
    SEND(req, card);

    SEND(req, PAGE_TAIL);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t scan_json(httpd_req_t *req)
{
    wifi_ap_record_t recs[PV_WIFI_SCAN_MAX];
    int n = pv_wifi_get_scan_results(recs, PV_WIFI_SCAN_MAX);
    if (n == 0 && !pv_wifi_is_scanning()) pv_wifi_scan_start();

    httpd_resp_set_type(req, "application/json");
    SEND(req, "[");
    for (int i = 0; i < n; i++) {
        // Skip SSIDs with a double-quote to keep the JSON valid (rare).
        if (strchr((char *)recs[i].ssid, '"')) continue;
        char item[80];
        snprintf(item, sizeof item, "%s\"%s\"", i ? "," : "", (char *)recs[i].ssid);
        SEND(req, item);
    }
    SEND(req, "]");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t rescan_post(httpd_req_t *req)
{
    pv_wifi_scan_start();
    httpd_resp_set_status(req, "204 No Content");
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
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='font-family:system-ui,sans-serif;text-align:center;margin-top:3em;background:#141414;color:#eee'>"
        "<h2>Saved &#10003;</h2><p>Rebooting and joining your Wi-Fi&hellip;</p>"
        "<p><small>This page will disconnect \xE2\x80\x94 that's expected.</small></p>");
    ESP_LOGI(TAG, "provisioned SSID='%s' moonraker='%s' — rebooting", chosen, mk_host);
    pv_wifi_save_creds_and_reboot(chosen, pass);   // writes ssid/password + reboots
    return ESP_OK;                                  // unreachable
}

esp_err_t pb_portal_start(void)
{
    httpd_handle_t s = pb_httpd_handle();
    if (s == NULL) return ESP_ERR_INVALID_STATE;

    httpd_uri_t save   = { .uri = "/save",      .method = HTTP_POST, .handler = save_post };
    httpd_uri_t rescan = { .uri = "/rescan",    .method = HTTP_POST, .handler = rescan_post };
    httpd_uri_t scan   = { .uri = "/scan.json", .method = HTTP_GET,  .handler = scan_json };
    httpd_uri_t root   = { .uri = "/*",          .method = HTTP_GET,  .handler = portal_page };
    httpd_register_uri_handler(s, &save);
    httpd_register_uri_handler(s, &rescan);
    httpd_register_uri_handler(s, &scan);
    httpd_register_uri_handler(s, &root);   // catch-all LAST (captive-portal probes)

    if (pv_wifi_state() == PV_WIFI_STATE_AP_PORTAL) {
        pv_wifi_ap_config_t ap;
        pv_wifi_get_ap_config(&ap);
        pb_dns_start(htonl(ap.ip));
        pv_wifi_scan_start();
        ESP_LOGI(TAG, "AP captive portal active");
    } else {
        ESP_LOGI(TAG, "config portal available (STA mode)");
    }
    return ESP_OK;
}
