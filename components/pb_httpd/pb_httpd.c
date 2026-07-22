// SPDX-License-Identifier: MIT
#include "pb_httpd.h"
#include "pb_heater.h"
#include "pb_ntc.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pb_httpd";
static httpd_handle_t s_server;

// Read-only. Deliberately has NO side effects — monitoring (a dashboard, a
// browser) must never keep the heater alive. Liveness is a separate explicit
// POST /heartbeat, so losing the controller trips the comms watchdog.
static esp_err_t status_get(httpd_req_t *req)
{
    char buf[224];
    int n = snprintf(buf, sizeof buf,
        "{\"temp\":%.1f,\"ptc\":%.1f,\"target\":%.1f,\"heating\":%s,\"fault\":%s,\"max\":%.0f}",
        pb_ntc_smoothed_c(PB_NTC_CHAMBER), pb_ntc_smoothed_c(PB_NTC_PTC),
        pb_heater_get_target_c(), pb_heater_is_on() ? "true" : "false",
        pb_heater_is_faulted() ? "true" : "false", (double)PB_HEATER_MAX_TARGET_C);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
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

// POST /target?t=<celsius>  (0 = off). POST-only so a stray GET (browser prefetch,
// crawler, link) can never energize the heater. A target command also counts as
// liveness. Falls back to a plain-number body if no query.
static esp_err_t target_set(httpd_req_t *req)
{
    float t = -1.0f;
    char q[48];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        char v[12];
        if (httpd_query_key_value(q, "t", v, sizeof v) == ESP_OK) t = atof(v);
    }
    if (t < 0.0f) {                                  // try request body ("60" or "60.0")
        char body[16] = {0};
        int r = httpd_req_recv(req, body, sizeof body - 1);
        if (r > 0) t = atof(body);
    }
    if (t < 0.0f) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing target ?t=<C>");
        return ESP_FAIL;
    }
    pb_heater_notify_link_alive();
    pb_heater_set_target_c(t);                       // clamps to [0, MAX] internally
    char buf[48];
    int n = snprintf(buf, sizeof buf, "{\"target\":%.1f}", pb_heater_get_target_c());
    httpd_resp_set_type(req, "application/json");
    ESP_LOGI(TAG, "target set to %.1f C via HTTP", pb_heater_get_target_c());
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
