// SPDX-License-Identifier: MIT
#include "pb_portal.h"
#include "pb_dns.h"
#include "pb_httpd.h"
#include "pv_wifi.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "lwip/inet.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pb_portal";
#define NVS_NS "app_nvs"
// Non-empty guard: httpd_resp_send_chunk() with a 0-length string terminates
// the chunked response early, so we must never send an empty chunk.
#define SEND(req, s) do { const char *_s = (s); if (_s && _s[0]) httpd_resp_send_chunk((req), _s, HTTPD_RESP_USE_STRLEN); } while (0)

// Shared client-side auth helpers, injected into every control page's <script>.
// window.DB_TOK holds the CSRF sentinel ("web") ONLY when no control token is
// configured. When a token IS configured the device sets window.DB_NEEDTOK
// instead and never emits the secret — we prompt for it and cache it in
// localStorage, so a configured token is genuine auth rather than a value baked
// into this public page. On a 403 the cached token is dropped and re-prompted.
#define DB_AUTH_JS \
    "function tok(){if(window.DB_TOK)return window.DB_TOK;" \
    "var t=localStorage.getItem('db_tok');" \
    "if(!t){t=prompt('DragonBreath control token')||'';if(t)localStorage.setItem('db_tok',t);}" \
    "return t;}" \
    "function hdr(){return {'X-DragonBreath-Auth':tok()};}" \
    "function post(u){return fetch(u,{method:'POST',headers:hdr()}).then(function(r){" \
    "if(r.status==403){localStorage.removeItem('db_tok');alert('Control token rejected \\u2014 try again.');}return r;});}"

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

// Escape a string for insertion inside an HTML double-quoted attribute value,
// so a stored mk_host can't break out of value="..." and inject markup/script.
static void html_attr_escape(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (const char *p = in; *p; p++) {
        const char *rep;
        switch (*p) {
            case '&':  rep = "&amp;";  break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            default:
                if (o + 1 >= outsz) { out[o] = '\0'; return; }
                out[o++] = *p;
                continue;
        }
        size_t rl = strlen(rep);
        if (o + rl >= outsz) break;
        memcpy(out + o, rep, rl);
        o += rl;
    }
    out[o] = '\0';
}

// ---- static page pieces ----
// Styled to match the stock BIQU Panda Breath web UI (dark #272525 page, #333
// rounded cards, Arial, blue accent) — palette lifted from the stock firmware,
// with the primary accent switched from stock red to the stock's blue (#4087FE).
// Shared head + CSS + header + <div class=wrap> — used by both the status page
// (STA root) and the config page (AP captive / /setup).
static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>DragonBreath</title><style>"
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
    "button:disabled{opacity:.4;cursor:not-allowed}"
    ".srow{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid rgba(255,255,255,.07)}.srow:last-child{border:0}"
    "small{color:#8a8a8a}a{color:#4aa3ff}</style></head><body>"
    "<div class=hdr><h1>\xF0\x9F\x90\xBC DragonBreath</h1><p>Panda Breath</p></div>"
    "<div class=wrap>";

// Wi-Fi form card (config page).
static const char CONFIG_WIFI[] =
    "<form id=cfg onsubmit='return save(event)'>"
    "<div class=card><h2>Wi-Fi</h2>"
    "<label>Network</label><select id=ssid name=ssid><option value=''>scanning\xE2\x80\xA6</option></select>"
    "<label>\xE2\x80\xA6 or hidden SSID</label><input name=ssid_manual placeholder='(optional)'>"
    "<label>Password</label><div class=pw><input id=pw type=password name=password autocomplete=off>"
    "<button type=button id=eye onclick='togglePw()' aria-label='show password'>\xF0\x9F\x91\x81</button></div>"
    "<button type=button class=sec onclick='rescan()'>Rescan networks</button></div>";

// Live status dashboard (STA root). Polls /status every 2s; can set the target,
// turn off, and clear a fault. Links to /setup for Wi-Fi/printer config.
static const char STATUS_BODY[] =
    "<div id=fault class=card style='display:none;background:#5a1f1f;color:#ffd7d7'></div>"
    "<div class=card style='text-align:center'>"
    "<div style='font-size:.8rem;color:#bdbdbd'>Chamber</div>"
    "<div id=temp style='font-size:3rem;font-weight:700;color:var(--accent);line-height:1.15'>--</div>"
    "<div id=cstat style='font-size:.75rem;color:#8a8a8a'></div></div>"
    "<div class=card>"
    "<div class=srow><span>Target</span><b id=target>--</b></div>"
    "<div class=srow><span>Heating</span><b id=heating>--</b></div>"
    "<div class=srow><span>Element (PTC)</span><b id=ptc>--</b></div></div>"
    "<div class=card><label>Set chamber target (&deg;C, 0 = off)</label>"
    "<div class=pw><input id=tin type=number min=0 max=70 step=1 value=45>"
    "<button type=button class=go style='width:auto;margin:0;padding:12px 18px' onclick='setT()'>Set</button></div>"
    "<button type=button id=off class=sec onclick='setOff()' disabled>Turn heater off</button>"
    "<button type=button class=sec id=rst style='display:none' onclick='doReset()'>Clear fault</button></div>"
    "<p style='text-align:center'><small><a href='/setup'>Wi-Fi / printer setup</a>"
    " &middot; <a href='/fw'>Firmware update</a></small></p>"
    "<p style='text-align:center;margin-top:-6px'><small style='color:#6f6f6f'>"
    "Firmware update installs <b>DragonBreath</b> updates only \xE2\x80\x94 it does <b>not</b> "
    "restore the stock Panda firmware.</small></p>"
    "<div id=ver style='text-align:center;color:#5a5a5a;font-size:.72rem;margin-top:2px'></div></div>"
    "<script>" DB_AUTH_JS
    "if(window.DB_VER)document.getElementById('ver').textContent='DragonBreath '+window.DB_VER;"
    // iOwn: did THIS tab start the current heat? We heartbeat ONLY then, so a
    // passive or freshly-reloaded dashboard (iOwn=false) never pets the comms
    // watchdog and therefore can't mask a Klippy/controller failure.
    "var iOwn=false;"
    "function u(i,v){document.getElementById(i).textContent=v;}"
    "function refresh(){fetch('/status').then(function(r){return r.json();}).then(function(s){"
    "u('temp',s.temp==null?'--':s.temp.toFixed(1)+'\\u00b0C');"
    "u('ptc',s.ptc==null?'--':s.ptc.toFixed(1)+'\\u00b0C'+(s.ptc_status!='ok'?' ('+s.ptc_status+')':''));"
    "u('target',s.target?s.target.toFixed(0)+'\\u00b0C':'off');"
    // Reflect the live target (e.g. one set by Klipper) in the set-temp box, but
    // never while the user is typing in it. Keep the last value when off.
    "var tin=document.getElementById('tin');"
    "if(document.activeElement!==tin&&s.target>0){tin.value=Math.round(s.target);}"
    "u('heating',s.heating?'ON':'off');"
    "var ob=document.getElementById('off');var on=s.target>0;ob.disabled=!on;"
    "ob.style.background=on?'var(--accent)':'';ob.style.color=on?'#fff':'';ob.style.borderColor=on?'var(--accent)':'';"
    "u('cstat',s.chamber_status=='ok'?'':'sensor: '+s.chamber_status);"
    "if(!(s.target>0))iOwn=false;"          // heat is off -> nobody's lease, incl. ours
    // Heartbeat lease — only for heat THIS tab started. Close the tab (or never
    // having started it) lets the 5-min watchdog latch the heater off (fail-safe).
    "if(iOwn&&s.target>0){fetch('/heartbeat',{method:'POST',headers:hdr()}).catch(function(){});}"
    "var f=document.getElementById('fault'),rb=document.getElementById('rst');"
    "if(s.fault){f.style.display='block';f.textContent='\\u26a0 Fault: '+(s.fault_reason||'')+' (fix, then Clear fault)';rb.style.display='block';}"
    "else{f.style.display='none';rb.style.display='none';}"
    "}).catch(function(){});}"
    // Take ownership ONLY after the device accepts a positive target (HTTP ok).
    // A rejected/failed request must NOT leave this tab thinking it owns heat, or
    // it could later heartbeat a target another controller set.
    "function setT(){var v=document.getElementById('tin').value;var pos=parseFloat(v)>0;"
    "post('/target?t='+encodeURIComponent(v)).then(function(r){iOwn=!!(r&&r.ok&&pos);refresh();})"
    ".catch(function(){iOwn=false;refresh();});}"
    "function setOff(){iOwn=false;post('/target?t=0').then(refresh);}"
    "function doReset(){post('/reset').then(refresh);}"
    "refresh();setInterval(refresh,2000);"
    "</script></body></html>";

// Dedicated firmware-update page (GET /fw) — DragonBreath OTA only, its own page so
// it isn't mixed in with Wi-Fi/printer setup.
static const char FW_BODY[] =
    "<div class=card><h2>Firmware update</h2>"
    "<p style='margin:.2em 0 .7em;font-size:.85rem;color:#bdbdbd'>Installs an "
    "<b>DragonBreath</b> firmware update (upload <code>dragonbreath.bin</code>). The "
    "image is verified and the device reboots into it; a bad image rolls back on "
    "the next boot.</p>"
    "<div class=card style='background:#3a2f1f;color:#ffe0b0;font-size:.8rem'>"
    "\xE2\x9A\xA0 This does <b>not</b> restore the stock Panda firmware. To go back to "
    "stock, reflash your saved backup over USB with <code>tools/flash.py --restore</code>.</div>"
    "<label>DragonBreath firmware (.bin)</label>"
    "<input type=file id=fw accept='.bin' onchange='fwsel()'>"
    "<button type=button id=fwbtn class=go onclick='doUpdate()' disabled>Upload &amp; flash</button>"
    "<div id=fwmsg style='margin-top:.6em;word-break:break-all'><small>Turn the heater OFF first "
    "(updates are refused while heating). Do not power off during the update.</small></div></div>"
    "<p style='text-align:center'><small><a href='/'>\xE2\x86\x90 Back to status</a></small></p>"
    "<div id=ver style='text-align:center;color:#5a5a5a;font-size:.72rem;margin-top:2px'></div></div>"
    "<script>" DB_AUTH_JS
    "if(window.DB_VER)document.getElementById('ver').textContent='DragonBreath '+window.DB_VER;"
    // Enable the flash button only once a file is chosen.
    "function fwsel(){document.getElementById('fwbtn').disabled=!document.getElementById('fw').files.length;}"
    // Stream the chosen .bin to /update with the auth header; device validates,
    // reports the SHA-256 it computed over the received image, and reboots. A
    // dropped connection on .catch is the expected reboot path.
    "function doUpdate(){var f=document.getElementById('fw').files[0];"
    "var m=document.getElementById('fwmsg');"
    "if(!f){m.innerHTML='<small>Choose a .bin file first.</small>';return;}"
    "m.innerHTML='<small>Uploading &amp; flashing\\u2026 do not power off.</small>';"
    "fetch('/update',{method:'POST',headers:hdr(),body:f})"
    ".then(function(r){return r.json().then(function(j){return {s:r.status,j:j};})"
    ".catch(function(){return {s:r.status,j:{}};});})"
    ".then(function(x){if(x.j&&x.j.ok){m.innerHTML='<h3>Flashed \\u2713</h3>"
    "<small>SHA-256 of uploaded image:<br><code>'+(x.j.sha256||'?')+'</code><br>"
    "Rebooting into the new firmware\\u2026</small>';}"
    "else{m.innerHTML='<small>Update failed: '+((x.j&&x.j.error)||('HTTP '+x.s))+'</small>';}})"
    ".catch(function(){m.innerHTML='<small>Connection lost \\u2014 if it was flashing, "
    "the device is rebooting into the new firmware.</small>';});}"
    "</script></body></html>";

static const char PAGE_TAIL[] =
    "<button type=submit class=go>Save &amp; Connect</button></form>"
    "<div id=msg style='text-align:center'><small>The device reboots and joins your network after saving.</small></div>"
    "<p style='text-align:center'><small><a href='/fw'>Firmware update</a></small></p></div>"
    "<script>" DB_AUTH_JS
    // Submit via fetch so we can attach the X-DragonBreath-Auth header (a plain form
    // POST can't). Required in STA /setup (the /save handler gates on it there);
    // harmless in AP mode. The device reboots on save, so a dropped connection on
    // the .then/.catch is the expected success path.
    "function save(e){e.preventDefault();"
    "var b=new URLSearchParams(new FormData(document.getElementById('cfg'))).toString();"
    "var done=function(){document.getElementById('msg').innerHTML="
    "'<h3>Saved \\u2713</h3><small>Rebooting and joining your Wi-Fi\\u2026 this page will disconnect.</small>';};"
    "var h=hdr();h['Content-Type']='application/x-www-form-urlencoded';"
    "fetch('/save',{method:'POST',headers:h,body:b}).then(done).catch(done);"
    "return false;}"
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

// Inject the client-side auth bootstrap. If a control token is configured we emit
// only a NEEDTOK flag (never the secret) so the page prompts for it; otherwise we
// emit the "web" CSRF sentinel. Paired with DB_AUTH_JS's tok()/hdr().
static void send_auth_inject(httpd_req_t *req)
{
    char tok[65];
    pb_httpd_ctl_token(tok, sizeof tok);   // 65-byte buffer: never false-negative
    SEND(req, tok[0]
        ? "<script>window.DB_NEEDTOK=1;</script>"
        : "<script>window.DB_TOK=\"web\";</script>");
}

// Inject the firmware version (git tag for releases, short hash for PR/local) as
// window.DB_VER so pages can show it. From the ESP app descriptor (see CMakeLists).
static void send_version_inject(httpd_req_t *req)
{
    char b[128];
    snprintf(b, sizeof b, "<script>window.DB_VER=\"%s\";</script>",
             esp_app_get_description()->version);
    SEND(req, b);
}

// ---- handlers ----
// Live status dashboard (root in STA mode).
static esp_err_t status_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    SEND(req, PAGE_HEAD);
    send_auth_inject(req);
    send_version_inject(req);
    SEND(req, STATUS_BODY);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Dedicated firmware-update page (GET /fw).
static esp_err_t fw_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    SEND(req, PAGE_HEAD);
    send_auth_inject(req);
    send_version_inject(req);
    SEND(req, FW_BODY);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Wi-Fi + Moonraker config page (AP captive root, and /setup in STA mode).
static esp_err_t config_page(httpd_req_t *req)
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

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    SEND(req, PAGE_HEAD);
    send_auth_inject(req);
    SEND(req, CONFIG_WIFI);

    // Moonraker card — values embedded so we never emit an empty chunk. mk_host
    // is user-controlled (stored in NVS via /save), so escape it before it lands
    // in the value="..." attribute (stored-XSS prevention). mk_port is a uint16.
    char mk_host_esc[192];
    html_attr_escape(mk_host, mk_host_esc, sizeof mk_host_esc);
    char card[420];
    snprintf(card, sizeof card,
        "<div class=card><h2>Printer (Moonraker)</h2>"
        "<label>Host / IP</label><input name=mk_host value=\"%s\" placeholder='e.g. 10.168.2.34'>"
        "<label>Port</label><input name=mk_port value=\"%u\"></div>",
        mk_host_esc, (unsigned)mk_port);
    SEND(req, card);

    SEND(req, PAGE_TAIL);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Catch-all root: in AP mode serve the config page so captive-portal probes land
// on setup; in STA mode serve the live status dashboard.
static esp_err_t root_page(httpd_req_t *req)
{
    if (pv_wifi_state() == PV_WIFI_STATE_AP_PORTAL) return config_page(req);
    return status_page(req);
}

static esp_err_t scan_json(httpd_req_t *req)
{
    wifi_ap_record_t recs[PV_WIFI_SCAN_MAX];
    int n = pv_wifi_get_scan_results(recs, PV_WIFI_SCAN_MAX);
    if (n == 0 && !pv_wifi_is_scanning()) pv_wifi_scan_start();

    // cJSON handles comma placement and escaping (quotes/backslashes/control chars),
    // so a skipped/odd SSID can't produce invalid JSON.
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n && arr; i++) {
        recs[i].ssid[sizeof recs[i].ssid - 1] = '\0';   // guarantee NUL-terminated
        if (recs[i].ssid[0] == '\0') continue;
        cJSON *s = cJSON_CreateString((const char *)recs[i].ssid);
        if (s) cJSON_AddItemToArray(arr, s);
    }
    char *out = arr ? cJSON_PrintUnformatted(arr) : NULL;
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, out ? out : "[]");
    if (out) cJSON_free(out);
    cJSON_Delete(arr);
    return r;
}

static esp_err_t rescan_post(httpd_req_t *req)
{
    pv_wifi_scan_start();
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t save_post(httpd_req_t *req)
{
    // Provisioning is open only in AP/setup mode (no credentials yet to send a
    // header from). Once joined to a network (STA), rewriting Wi-Fi config is a
    // mutating control action, so require the CSRF header like the other POSTs.
    if (pv_wifi_state() != PV_WIFI_STATE_AP_PORTAL && !pb_httpd_auth_ok(req)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"missing/invalid X-DragonBreath-Auth header\"}");
    }

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

    httpd_resp_set_type(req, "text/html; charset=utf-8");
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
    httpd_uri_t setup  = { .uri = "/setup",     .method = HTTP_GET,  .handler = config_page };
    httpd_uri_t fw     = { .uri = "/fw",        .method = HTTP_GET,  .handler = fw_page };
    httpd_uri_t root   = { .uri = "/*",          .method = HTTP_GET,  .handler = root_page };
    httpd_register_uri_handler(s, &save);
    httpd_register_uri_handler(s, &rescan);
    httpd_register_uri_handler(s, &scan);
    httpd_register_uri_handler(s, &setup);
    httpd_register_uri_handler(s, &fw);
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
