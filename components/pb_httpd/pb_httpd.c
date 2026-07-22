// SPDX-License-Identifier: MIT
#include "pb_httpd.h"
#include "pb_heater.h"
#include "pb_ntc.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "pb_httpd";
static httpd_handle_t s_server;

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
    pb_heater_notify_link_alive();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Explicit safety-fault reset (over-temp / sensor / comms). POST-only.
static esp_err_t reset_post(httpd_req_t *req)
{
    pb_heater_clear_fault();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Parse a finite decimal temperature. strtof with an end-pointer check rejects
// junk AND non-finite tokens ("nan"/"inf" -> isfinite() false), which atof would
// silently accept.
static bool parse_temp(const char *s, float *out)
{
    if (!s || !*s) return false;
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s || !isfinite(v)) return false;
    *out = v;
    return true;
}

// POST /target?t=<celsius>  (0 = off). POST-only so a stray GET (browser prefetch,
// crawler, link) can never energize the heater. A target command also counts as
// liveness. Falls back to a plain-number body if no query.
static esp_err_t target_set(httpd_req_t *req)
{
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

esp_err_t pb_httpd_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;   // lets pb_portal add a "/*" captive catch-all
    cfg.max_uri_handlers = 12;                     // control API + portal + captive
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) return err;

    httpd_uri_t status = { .uri = "/status",    .method = HTTP_GET,  .handler = status_get };
    httpd_uri_t tgt    = { .uri = "/target",    .method = HTTP_POST, .handler = target_set };
    httpd_uri_t hb     = { .uri = "/heartbeat", .method = HTTP_POST, .handler = heartbeat_post };
    httpd_uri_t rst    = { .uri = "/reset",     .method = HTTP_POST, .handler = reset_post };
    httpd_register_uri_handler(s_server, &status);
    httpd_register_uri_handler(s_server, &tgt);
    httpd_register_uri_handler(s_server, &hb);
    httpd_register_uri_handler(s_server, &rst);
    ESP_LOGI(TAG, "HTTP API up :80 (GET /status; POST /target?t=<C>, /heartbeat, /reset)");
    return ESP_OK;
}

httpd_handle_t pb_httpd_handle(void) { return s_server; }
