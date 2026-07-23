// SPDX-License-Identifier: MIT
#include "pb_httpd.h"
#include "pb_heater.h"
#include "pb_ntc.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "nvs.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "pb_httpd";
static httpd_handle_t s_server;

void pb_httpd_ctl_token(char *out, size_t outsz)
{
    if (!out || outsz == 0) return;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open("app_nvs", NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = outsz;
    nvs_get_str(h, "ctl_token", out, &sz);   // leaves out="" on any error
    out[outsz - 1] = '\0';
    nvs_close(h);
}

bool pb_httpd_auth_ok(httpd_req_t *req)
{
    char tok[65];
    pb_httpd_ctl_token(tok, sizeof tok);

    // Read the custom header. Its mere presence defeats cross-origin HTML forms
    // (which cannot set custom headers); a configured token additionally pins the
    // value. Absent header, or one too long to be a valid (<=64 char) token, fails.
    char hv[65] = {0};
    size_t hlen = httpd_req_get_hdr_value_len(req, PB_AUTH_HEADER);
    if (hlen == 0 || hlen >= sizeof hv) return false;
    if (httpd_req_get_hdr_value_str(req, PB_AUTH_HEADER, hv, sizeof hv) != ESP_OK) return false;

    if (tok[0]) return strcmp(hv, tok) == 0;   // configured token -> exact match
    return hv[0] != '\0';                       // else presence-only CSRF gate
}

// Reject a mutating request lacking a valid PB_AUTH_HEADER with 403. Returns true
// when the caller should stop (already responded).
static bool auth_reject(httpd_req_t *req)
{
    if (pb_httpd_auth_ok(req)) return false;
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"missing/invalid " PB_AUTH_HEADER " header\"}");
    return true;
}

static const char *ntc_status_str(pb_ntc_status_t s)
{
    switch (s) {
        case PB_NTC_OK:    return "ok";
        case PB_NTC_OPEN:  return "open";
        case PB_NTC_SHORT: return "short";
        default:           return "uninit";
    }
}

// Add a number rounded to 1 decimal (as a raw JSON literal so cJSON doesn't emit
// full float precision), or JSON null when the value is non-finite.
static void add_num1(cJSON *o, const char *key, float v)
{
    if (!isfinite(v)) { cJSON_AddNullToObject(o, key); return; }
    char b[16];
    snprintf(b, sizeof b, "%.1f", (double)v);
    cJSON_AddRawToObject(o, key, b);
}

// Read-only. Deliberately has NO side effects — monitoring (a dashboard, a
// browser) must never keep the heater alive. Liveness is a separate explicit
// POST /heartbeat, so losing the controller trips the comms watchdog. Built with
// cJSON so temps are emitted as JSON null (never the invalid bare token `nan`)
// when a channel isn't reading OK, alongside explicit per-sensor status.
static esp_err_t status_get(httpd_req_t *req)
{
    pb_ntc_status_t cs = pb_ntc_last_status(PB_NTC_CHAMBER);
    pb_ntc_status_t ps = pb_ntc_last_status(PB_NTC_PTC);

    cJSON *o = cJSON_CreateObject();
    if (!o) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }

    add_num1(o, "temp", cs == PB_NTC_OK ? pb_ntc_smoothed_c(PB_NTC_CHAMBER) : NAN);
    add_num1(o, "ptc",  ps == PB_NTC_OK ? pb_ntc_smoothed_c(PB_NTC_PTC)     : NAN);
    cJSON_AddStringToObject(o, "chamber_status", ntc_status_str(cs));
    cJSON_AddStringToObject(o, "ptc_status", ntc_status_str(ps));
    add_num1(o, "target", pb_heater_get_target_c());
    cJSON_AddBoolToObject(o, "heating", pb_heater_is_on());
    cJSON_AddBoolToObject(o, "fault", pb_heater_is_faulted());
    const char *fr = pb_heater_fault_reason();
    if (fr) cJSON_AddStringToObject(o, "fault_reason", fr);
    else    cJSON_AddNullToObject(o, "fault_reason");
    add_num1(o, "max", PB_HEATER_MAX_TARGET_C);
    cJSON_AddStringToObject(o, "version", esp_app_get_description()->version);

    char *out = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, out ? out : "{}");
    if (out) cJSON_free(out);
    cJSON_Delete(o);
    return r;
}

// Explicit controller liveness. The controller must POST this regularly while it
// wants heat; if it stops, the heater comms-watchdog latches off.
static esp_err_t heartbeat_post(httpd_req_t *req)
{
    if (auth_reject(req)) return ESP_OK;
    pb_heater_notify_link_alive();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Explicit safety-fault reset (over-temp / sensor / comms). POST-only.
static esp_err_t reset_post(httpd_req_t *req)
{
    if (auth_reject(req)) return ESP_OK;
    // A permanent inhibit can't be cleared — say so (409) rather than falsely
    // reporting a successful clear.
    if (pb_heater_is_inhibited()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
            "{\"error\":\"heater permanently inhibited — reboot required\"}");
    }
    pb_heater_clear_fault();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Parse a finite decimal temperature. strtof with an end-pointer check rejects
// junk AND non-finite tokens ("nan"/"inf" -> isfinite() false), which atof would
// silently accept. Trailing non-whitespace ("45junk", "45 60") is also rejected
// so a partially-numeric value can never be silently truncated to a target.
static bool parse_temp(const char *s, float *out)
{
    if (!s || !*s) return false;
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s || !isfinite(v)) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end != '\0') return false;      // trailing junk after the number
    *out = v;
    return true;
}

// POST /target?t=<celsius>  (0 = off). POST-only so a stray GET (browser prefetch,
// crawler, link) can never energize the heater. A target command also counts as
// liveness. Falls back to a plain-number body if no query.
static esp_err_t target_set(httpd_req_t *req)
{
    if (auth_reject(req)) return ESP_OK;
    float t = 0.0f;
    bool have = false;
    char q[48];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        char v[16];
        if (httpd_query_key_value(q, "t", v, sizeof v) == ESP_OK) have = parse_temp(v, &t);
    }
    if (!have) {                                     // try request body ("60" / "60.0")
        char body[16] = {0};
        int r = httpd_req_recv(req, body, sizeof body - 1);
        if (r > 0) { body[r] = '\0'; have = parse_temp(body, &t); }
    }
    if (!have) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad/missing target ?t=<finite C>");
        return ESP_FAIL;
    }

    pb_heater_notify_link_alive();                   // a command counts as liveness
    esp_err_t se = pb_heater_set_target_c(t);        // validates + clamps internally
    if (se == ESP_ERR_INVALID_STATE) {               // heat requested while faulted
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
            "{\"error\":\"fault latched — POST /reset, then set a target\"}");
    }
    if (se != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid target");
        return ESP_FAIL;
    }
    char buf[48];
    int n = snprintf(buf, sizeof buf, "{\"target\":%.1f}", pb_heater_get_target_c());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

// POST /update — stream a new DragonBreath .bin into the inactive OTA slot, verify,
// set it as boot, and reboot. Auth-gated. SAFETY: refused while the heater is
// armed/on — a reboot mid-heat leaves the SSR state undefined until re-init, so
// the operator must turn the heater off first. Rollback (PENDING_VERIFY) means a
// bad image that crashes before app_main marks itself healthy reverts on reboot.
static esp_err_t ota_fail(httpd_req_t *req, const char *status, esp_ota_handle_t h, const char *msg)
{
    if (h) esp_ota_abort(h);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char b[128];
    snprintf(b, sizeof b, "{\"error\":\"%s\"}", msg);
    httpd_resp_sendstr(req, b);
    return ESP_FAIL;
}

static void ota_reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1200));   // give the HTTP response time to flush
    esp_restart();
}

static esp_err_t update_post(httpd_req_t *req)
{
    if (auth_reject(req)) return ESP_OK;

    // Block while heating (chosen safety policy).
    if (pb_heater_is_on() || pb_heater_get_target_c() > 0.0f)
        return ota_fail(req, "409 Conflict", 0, "turn the heater off before updating");

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return ota_fail(req, "500 Internal Server Error", 0, "no OTA partition");

    esp_ota_handle_t h = 0;
    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &h) != ESP_OK)
        return ota_fail(req, "500 Internal Server Error", 0, "esp_ota_begin failed");
    ESP_LOGW(TAG, "OTA: receiving new image into %s", part->label);

    // Hash the received bytes so the operator can confirm what was flashed.
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);   // 0 = SHA-256

    char buf[1024];
    int total = 0, r;
    while ((r = httpd_req_recv(req, buf, sizeof buf)) > 0) {
        if (esp_ota_write(h, buf, r) != ESP_OK) {
            mbedtls_sha256_free(&sha);
            return ota_fail(req, "500 Internal Server Error", h, "flash write failed");
        }
        mbedtls_sha256_update(&sha, (const unsigned char *)buf, r);
        total += r;
    }
    if (r < 0) {    // recv error / timeout (r == 0 is a clean end of body)
        mbedtls_sha256_free(&sha);
        return ota_fail(req, "400 Bad Request", h, "upload interrupted");
    }
    if (total == 0) {
        mbedtls_sha256_free(&sha);
        return ota_fail(req, "400 Bad Request", h, "empty upload");
    }

    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    char sha_hex[65];
    for (int i = 0; i < 32; i++) snprintf(sha_hex + i * 2, 3, "%02x", digest[i]);

    esp_err_t err = esp_ota_end(h);   // validates the image (magic/size/checksum)
    if (err != ESP_OK)
        return ota_fail(req, "400 Bad Request", 0, "invalid firmware image");

    // Identity check: only boot images that are actually DragonBreath — reject any
    // other (even a valid ESP32-C3) app. The app descriptor's project_name comes
    // from CMake project(dragonbreath).
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK)
        return ota_fail(req, "400 Bad Request", 0, "cannot read image descriptor");
    if (strcmp(desc.project_name, "dragonbreath") != 0) {
        ESP_LOGE(TAG, "OTA rejected: project_name='%s' (not dragonbreath)", desc.project_name);
        return ota_fail(req, "400 Bad Request", 0, "not an DragonBreath image");
    }

    if (esp_ota_set_boot_partition(part) != ESP_OK)
        return ota_fail(req, "500 Internal Server Error", 0, "set boot partition failed");

    ESP_LOGW(TAG, "OTA: %d bytes -> %s, sha256=%s, ver=%s; rebooting shortly",
             total, part->label, sha_hex, desc.version);
    httpd_resp_set_type(req, "application/json");
    char b[160];
    snprintf(b, sizeof b, "{\"ok\":true,\"bytes\":%d,\"sha256\":\"%s\"}", total, sha_hex);
    httpd_resp_sendstr(req, b);
    // Reboot from a separate task so this handler can RETURN first: httpd then
    // closes the connection cleanly and the client reliably receives the full
    // JSON (incl. the SHA) before esp_restart() tears the socket down.
    xTaskCreate(ota_reboot_task, "ob_ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t pb_httpd_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;   // lets pb_portal add a "/*" captive catch-all
    cfg.max_uri_handlers = 12;                     // control API + portal + captive
    // The OTA handler hashes the image (mbedtls) with a 1 KB read buffer + the
    // app descriptor on-stack, which overflows the 4 KB default httpd task stack
    // (stack-protection panic). Give it headroom.
    cfg.stack_size = 8192;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) return err;

    httpd_uri_t status = { .uri = "/status",    .method = HTTP_GET,  .handler = status_get };
    httpd_uri_t tgt    = { .uri = "/target",    .method = HTTP_POST, .handler = target_set };
    httpd_uri_t hb     = { .uri = "/heartbeat", .method = HTTP_POST, .handler = heartbeat_post };
    httpd_uri_t rst    = { .uri = "/reset",     .method = HTTP_POST, .handler = reset_post };
    httpd_uri_t upd    = { .uri = "/update",    .method = HTTP_POST, .handler = update_post };
    httpd_register_uri_handler(s_server, &status);
    httpd_register_uri_handler(s_server, &tgt);
    httpd_register_uri_handler(s_server, &hb);
    httpd_register_uri_handler(s_server, &rst);
    httpd_register_uri_handler(s_server, &upd);
    ESP_LOGI(TAG, "HTTP API up :80 (GET /status; POST /target?t=<C>, /heartbeat, /reset, /update)");
    return ESP_OK;
}

httpd_handle_t pb_httpd_handle(void) { return s_server; }
