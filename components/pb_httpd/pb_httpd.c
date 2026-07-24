// SPDX-License-Identifier: MIT
#include "pb_httpd.h"
#include "pb_heater.h"
#include "pb_ntc.h"
#include "pb_leds.h"
#include "pb_policy.h"
#include "pv_evlog.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "nvs.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "pb_httpd";
static httpd_handle_t s_server;
static char s_device_id[32];
static char s_boot_id[33];
static portMUX_TYPE s_sse_mux = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_sse_clients;

#define API_VERSION 2
#define API_BODY_MAX 1536
#define SSE_MAX_CLIENTS 2

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
    httpd_resp_sendstr(req,
        "{\"ok\":false,\"error\":\"auth_failed\","
        "\"message\":\"missing/invalid " PB_AUTH_HEADER " header\"}");
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

static const char *fan_reason(const pb_policy_snapshot_t *s)
{
    if (s->fault_latched || s->inhibited) return "fault";
    if (s->thermal_purge) return "thermal_purge";
    if (s->heater_demand) return "heater";
    if (s->effective_fan_percent) return "requested";
    return "off";
}

static cJSON *state_json(const pb_policy_snapshot_t *s)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddNumberToObject(o, "api_version", API_VERSION);
    cJSON_AddStringToObject(o, "device_id", s_device_id);
    cJSON_AddStringToObject(o, "boot_id", s_boot_id);
    cJSON_AddStringToObject(o, "firmware", esp_app_get_description()->version);
    cJSON_AddNumberToObject(o, "state_revision", s->state_revision);
    cJSON_AddStringToObject(o, "mode", pb_policy_mode_str(s->mode));
    cJSON_AddStringToObject(o, "source", pb_policy_source_str(s->source));

    cJSON *target = cJSON_AddObjectToObject(o, "target");
    add_num1(target, "requested_c", s->requested_target_c);
    add_num1(target, "effective_c", s->effective_target_c);
    add_num1(target, "maximum_c", pb_heater_get_max_target_c());

    cJSON *config = cJSON_AddObjectToObject(o, "config");
    add_num1(config, "max", pb_heater_get_max_target_c());
    add_num1(config, "max_abs", PB_HEATER_ABS_MAX_TARGET_C);
    cJSON_AddNumberToObject(
        config, "comms_ms", pb_heater_get_comms_timeout_ms());

    cJSON *heater = cJSON_AddObjectToObject(o, "heater");
    cJSON_AddBoolToObject(heater, "demand", s->heater_demand);
    cJSON_AddBoolToObject(heater, "output", s->heater_output);

    cJSON *fan = cJSON_AddObjectToObject(o, "fan");
    cJSON_AddNumberToObject(fan, "requested_percent", s->requested_fan_percent);
    cJSON_AddNumberToObject(fan, "effective_percent", s->effective_fan_percent);
    cJSON_AddStringToObject(fan, "reason", fan_reason(s));

    cJSON *sensors = cJSON_AddObjectToObject(o, "sensors");
    cJSON *chamber = cJSON_AddObjectToObject(sensors, "chamber");
    add_num1(chamber, "temperature_c", s->chamber_c);
    cJSON_AddStringToObject(chamber, "status", ntc_status_str(s->chamber_status));
    cJSON *ptc = cJSON_AddObjectToObject(sensors, "ptc");
    add_num1(ptc, "temperature_c", s->ptc_c);
    cJSON_AddStringToObject(ptc, "status", ntc_status_str(s->ptc_status));

    cJSON *environment = cJSON_AddObjectToObject(o, "environment");
    cJSON_AddBoolToObject(environment, "moonraker_connected", s->moonraker_connected);
    add_num1(environment, "bed_temperature_c", s->bed_c);
    cJSON_AddBoolToObject(environment, "auto_engaged", s->auto_engaged);
    add_num1(environment, "auto_bed_threshold_c", s->auto_bed_threshold_c);

    cJSON *drying = cJSON_AddObjectToObject(o, "drying");
    cJSON_AddBoolToObject(drying, "active", s->drying);
    cJSON_AddNumberToObject(drying, "remaining_seconds", s->drying_remaining_s);

    cJSON *control = cJSON_AddObjectToObject(o, "control");
    cJSON *lease = cJSON_AddObjectToObject(control, "lease");
    cJSON_AddBoolToObject(lease, "active", s->lease_active);
    if (s->lease_active) {
        cJSON_AddStringToObject(lease, "owner", s->lease_owner);
        cJSON_AddNumberToObject(lease, "expires_in_ms", s->lease_expires_ms);
    } else {
        cJSON_AddNullToObject(lease, "owner");
        cJSON_AddNumberToObject(lease, "expires_in_ms", 0);
    }

    // Remembered mode parameters: what a mode is re-armed with when the caller
    // supplies no values of its own (front-panel buttons), and what the UI
    // pre-fills from. These persist across reboot; active state never does.
    cJSON *pj = cJSON_AddObjectToObject(o, "params");
    add_num1(pj, "manual_target_c", s->params.manual_target_c);
    add_num1(pj, "auto_target_c", s->params.auto_target_c);
    add_num1(pj, "auto_bed_threshold_c", s->params.auto_bed_threshold_c);
    add_num1(pj, "dry_target_c", s->params.dry_target_c);
    cJSON_AddNumberToObject(pj, "dry_hours", s->params.dry_hours);

    cJSON *safety = cJSON_AddObjectToObject(o, "safety");
    cJSON_AddBoolToObject(safety, "fault_latched", s->fault_latched);
    cJSON_AddBoolToObject(safety, "inhibited", s->inhibited);
    if (s->fault_reason[0])
        cJSON_AddStringToObject(safety, "reason", s->fault_reason);
    else
        cJSON_AddNullToObject(safety, "reason");
    return o;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *o)
{
    char *out = cJSON_PrintUnformatted(o);
    if (!out) {
        cJSON_Delete(o);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    cJSON_Delete(o);
    return r;
}

static esp_err_t api_error(
    httpd_req_t *req,
    const char *status,
    const char *code,
    const char *message,
    const char *request_id)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return ESP_FAIL;
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", code);
    cJSON_AddStringToObject(o, "message", message);
    if (request_id) cJSON_AddStringToObject(o, "request_id", request_id);
    pb_policy_snapshot_t snap;
    pb_policy_get_snapshot(&snap);
    cJSON_AddItemToObject(o, "state", state_json(&snap));
    httpd_resp_set_status(req, status);
    return send_json(req, o);
}

static cJSON *recv_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > API_BODY_MAX) return NULL;
    char body[API_BODY_MAX + 1];
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return NULL;
        got += r;
    }
    body[got] = '\0';
    return cJSON_ParseWithLength(body, got);
}

static bool json_number(const cJSON *o, const char *key, double *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) return false;
    *out = v->valuedouble;
    return true;
}

static esp_err_t info_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "api_version", API_VERSION);
    cJSON_AddStringToObject(o, "device_id", s_device_id);
    cJSON_AddStringToObject(o, "boot_id", s_boot_id);
    cJSON_AddStringToObject(o, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "project", esp_app_get_description()->project_name);
    cJSON *cap = cJSON_AddArrayToObject(o, "capabilities");
    cJSON_AddItemToArray(cap, cJSON_CreateString("power_on"));
    cJSON_AddItemToArray(cap, cJSON_CreateString("auto"));
    cJSON_AddItemToArray(cap, cJSON_CreateString("drying"));
    cJSON_AddItemToArray(cap, cJSON_CreateString("lease_heartbeat"));
    cJSON_AddItemToArray(cap, cJSON_CreateString("sse"));
    return send_json(req, o);
}

static esp_err_t state_get(httpd_req_t *req)
{
    pb_policy_snapshot_t snap;
    pb_policy_get_snapshot(&snap);
    return send_json(req, state_json(&snap));
}

typedef struct {
    char actor_id[PB_POLICY_OWNER_LEN + 1];
    char request_id[65];
    uint32_t revision;
    char response[3072];
} replay_entry_t;

static replay_entry_t s_replay[2];
static unsigned s_replay_next;

static replay_entry_t *replay_find(
    const char *actor_id,
    const char *request_id)
{
    for (size_t i = 0; i < sizeof s_replay / sizeof s_replay[0]; i++)
        if (s_replay[i].request_id[0]
                && strcmp(s_replay[i].actor_id, actor_id) == 0
                && strcmp(s_replay[i].request_id, request_id) == 0)
            return &s_replay[i];
    return NULL;
}

static void replay_store(
    const char *actor_id,
    const char *request_id,
    uint32_t revision,
    const char *response)
{
    replay_entry_t *e = &s_replay[s_replay_next++ % (sizeof s_replay / sizeof s_replay[0])];
    snprintf(e->actor_id, sizeof e->actor_id, "%s", actor_id);
    snprintf(e->request_id, sizeof e->request_id, "%s", request_id);
    e->revision = revision;
    snprintf(e->response, sizeof e->response, "%s", response);
}

static esp_err_t send_command_success(
    httpd_req_t *req,
    const char *actor_id,
    const char *request_id,
    const char *lease_id,
    bool cache)
{
    pb_policy_snapshot_t snap;
    pb_policy_get_snapshot(&snap);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "request_id", request_id);
    if (lease_id && lease_id[0])
        cJSON_AddStringToObject(o, "lease_id", lease_id);
    cJSON_AddItemToObject(o, "state", state_json(&snap));
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!out) return api_error(req, "500 Internal Server Error", "internal", "out of memory", request_id);
    if (cache)
        replay_store(actor_id, request_id, snap.state_revision, out);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return r;
}

static esp_err_t policy_error(
    httpd_req_t *req,
    pb_policy_result_t result,
    const char *request_id)
{
    const char *status = result == PB_POLICY_INVALID
        ? "400 Bad Request" : "409 Conflict";
    const char *code = result == PB_POLICY_INVALID
        ? "invalid_command" : pb_policy_result_str(result);
    return api_error(req, status, code,
                     "command rejected by authoritative policy", request_id);
}

static esp_err_t command_post(httpd_req_t *req)
{
    if (auth_reject(req)) return ESP_OK;
    cJSON *root = recv_json(req);
    if (!root)
        return api_error(req, "400 Bad Request", "invalid_command", "invalid JSON body", NULL);

    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "api_version");
    cJSON *rid = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    cJSON *actor = cJSON_GetObjectItemCaseSensitive(root, "actor");
    cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "command");
    cJSON *kind = cJSON_IsObject(actor)
        ? cJSON_GetObjectItemCaseSensitive(actor, "kind") : NULL;
    cJSON *actor_id = cJSON_IsObject(actor)
        ? cJSON_GetObjectItemCaseSensitive(actor, "id") : NULL;
    cJSON *name = cJSON_IsObject(command)
        ? cJSON_GetObjectItemCaseSensitive(command, "name") : NULL;

    char request_id_buf[65] = {0};
    const char *request_id = NULL;
    if (cJSON_IsString(rid) && rid->valuestring[0]
            && strlen(rid->valuestring) <= 64) {
        snprintf(request_id_buf, sizeof request_id_buf, "%s", rid->valuestring);
        request_id = request_id_buf;
    }
    if (!cJSON_IsNumber(version) || version->valuedouble != API_VERSION) {
        cJSON_Delete(root);
        return api_error(req, "400 Bad Request", "unsupported_api_version",
                         "api_version must be 2", request_id);
    }
    if (!request_id
            || !cJSON_IsString(kind) || !cJSON_IsString(actor_id)
            || !actor_id->valuestring[0] || strlen(actor_id->valuestring) > PB_POLICY_OWNER_LEN
            || !cJSON_IsString(name)) {
        cJSON_Delete(root);
        return api_error(req, "400 Bad Request", "invalid_command",
                         "request_id, actor.kind, actor.id, and command.name are required",
                         request_id);
    }
    char actor_id_buf[PB_POLICY_OWNER_LEN + 1];
    snprintf(actor_id_buf, sizeof actor_id_buf, "%s", actor_id->valuestring);
    pb_source_t source;
    if (strcmp(kind->valuestring, "klipper") == 0) source = PB_SOURCE_KLIPPER;
    else if (strcmp(kind->valuestring, "web") == 0) source = PB_SOURCE_WEB;
    else {
        cJSON_Delete(root);
        return api_error(req, "400 Bad Request", "invalid_command",
                         "actor.kind must be web or klipper", request_id);
    }

    const char *cmd = name->valuestring;
    bool cacheable = strcmp(cmd, "off") != 0
        && strcmp(cmd, "drying_stop") != 0;
    replay_entry_t *prior = cacheable
        ? replay_find(actor_id_buf, request_id) : NULL;
    if (prior) {
        pb_policy_snapshot_t snap;
        pb_policy_get_snapshot(&snap);
        cJSON_Delete(root);
        if (snap.state_revision == prior->revision) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_sendstr(req, prior->response);
        }
        return api_error(req, "409 Conflict", "revision_conflict",
                         "request_id was already applied to an older state", request_id);
    }

    cJSON *expected = cJSON_GetObjectItemCaseSensitive(root, "expected_revision");
    uint32_t revision = PB_POLICY_REVISION_ANY;
    bool revision_valid = cJSON_IsNumber(expected) && expected->valuedouble >= 0
            && expected->valuedouble < UINT32_MAX
            && floor(expected->valuedouble) == expected->valuedouble;
    if (revision_valid)
        revision = (uint32_t)expected->valuedouble;

    pb_policy_result_t result = PB_POLICY_INVALID;
    double target = 0.0, threshold = 0.0, hours = 0.0;
    pb_policy_lease_t issued_lease = {0};
    if (strcmp(cmd, "off") == 0) {
        pb_policy_set_mode_off(source);
        result = PB_POLICY_OK; // revision intentionally ignored for safer action
    } else if (!revision_valid) {
        result = PB_POLICY_INVALID;
    } else if (strcmp(cmd, "power_on") == 0
            && json_number(command, "target_c", &target)) {
        result = pb_policy_set_power_on(
            (float)target, source, actor_id_buf, revision, &issued_lease);
    } else if (strcmp(cmd, "auto") == 0
            && json_number(command, "target_c", &target)
            && json_number(command, "bed_threshold_c", &threshold)) {
        result = pb_policy_set_auto(
            (float)target, (float)threshold, source, revision);
    } else if (strcmp(cmd, "drying_start") == 0
            && json_number(command, "target_c", &target)
            && json_number(command, "hours", &hours)
            && hours >= 1 && hours <= 12 && floor(hours) == hours) {
        result = pb_policy_start_drying(
            (float)target, (uint8_t)hours, source, revision);
    } else if (strcmp(cmd, "drying_stop") == 0) {
        pb_policy_stop_drying(source); // safer action, like OFF
        result = PB_POLICY_OK;
    } else if (strcmp(cmd, "clear_fault") == 0) {
        result = pb_policy_clear_fault(source, revision);
    }

    cJSON_Delete(root);
    if (result != PB_POLICY_OK) return policy_error(req, result, request_id);
    return send_command_success(
        req,
        actor_id_buf,
        request_id,
        issued_lease.id[0] ? issued_lease.id : NULL,
        cacheable);
}

static esp_err_t heartbeat_post(httpd_req_t *req)
{
    if (auth_reject(req)) return ESP_OK;
    cJSON *root = recv_json(req);
    cJSON *version = root ? cJSON_GetObjectItemCaseSensitive(root, "api_version") : NULL;
    cJSON *lease_id = root ? cJSON_GetObjectItemCaseSensitive(root, "lease_id") : NULL;
    if (!root || !cJSON_IsNumber(version) || version->valuedouble != API_VERSION
            || !cJSON_IsString(lease_id)
            || strlen(lease_id->valuestring) != PB_POLICY_LEASE_ID_LEN) {
        if (root) cJSON_Delete(root);
        return api_error(req, "400 Bad Request", "invalid_command",
                         "api_version 2 and exact lease_id are required", NULL);
    }
    pb_policy_lease_t lease = {0};
    memcpy(lease.id, lease_id->valuestring, PB_POLICY_LEASE_ID_LEN);
    cJSON_Delete(root);
    pb_policy_result_t result = pb_policy_heartbeat(&lease);
    if (result != PB_POLICY_OK) return policy_error(req, result, NULL);

    pb_policy_snapshot_t snap;
    pb_policy_get_snapshot(&snap);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "expires_in_ms", snap.lease_expires_ms);
    cJSON_AddNumberToObject(o, "state_revision", snap.state_revision);
    return send_json(req, o);
}

static esp_err_t health_get(httpd_req_t *req)
{
    wifi_ap_record_t ap = {0};
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "api_version", API_VERSION);
    cJSON_AddStringToObject(o, "boot_id", s_boot_id);
    cJSON_AddNumberToObject(o, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddNumberToObject(o, "free_heap_bytes", esp_get_free_heap_size());
    cJSON_AddNumberToObject(o, "minimum_free_heap_bytes", esp_get_minimum_free_heap_size());
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(o, "wifi_rssi_dbm", ap.rssi);
        cJSON_AddNumberToObject(o, "wifi_channel", ap.primary);
    } else {
        cJSON_AddNullToObject(o, "wifi_rssi_dbm");
        cJSON_AddNullToObject(o, "wifi_channel");
    }
    portENTER_CRITICAL(&s_sse_mux);
    unsigned clients = s_sse_clients;
    portEXIT_CRITICAL(&s_sse_mux);
    cJSON_AddNumberToObject(o, "sse_clients", clients);
    return send_json(req, o);
}

static esp_err_t sse_send(
    httpd_req_t *req,
    const char *event,
    uint32_t id,
    const pb_policy_snapshot_t *snap)
{
    cJSON *o = state_json(snap);
    if (!o) return ESP_ERR_NO_MEM;
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json) return ESP_ERR_NO_MEM;
    char head[64];
    int n = id == UINT32_MAX
        ? snprintf(head, sizeof head, "event: %s\n", event)
        : snprintf(head, sizeof head, "event: %s\nid: %lu\n", event, (unsigned long)id);
    esp_err_t r = httpd_resp_send_chunk(req, head, n);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, "data: ", 6);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, json, strlen(json));
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, "\n\n", 2);
    cJSON_free(json);
    return r;
}

static void sse_task(void *arg)
{
    httpd_req_t *req = arg;
    uint32_t last_revision = UINT32_MAX;
    int64_t last_telemetry_us = 0;
    for (;;) {
        pb_policy_snapshot_t snap;
        pb_policy_get_snapshot(&snap);
        int64_t now = esp_timer_get_time();
        esp_err_t r = ESP_OK;
        if (snap.state_revision != last_revision) {
            r = sse_send(req, "state", snap.state_revision, &snap);
            last_revision = snap.state_revision;
            last_telemetry_us = now;
        } else if (now - last_telemetry_us >= 2000000) {
            r = sse_send(req, "telemetry", UINT32_MAX, &snap);
            last_telemetry_us = now;
        }
        if (r != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    httpd_req_async_handler_complete(req);
    portENTER_CRITICAL(&s_sse_mux);
    s_sse_clients--;
    portEXIT_CRITICAL(&s_sse_mux);
    vTaskDelete(NULL);
}

static esp_err_t events_get(httpd_req_t *req)
{
    portENTER_CRITICAL(&s_sse_mux);
    bool full = s_sse_clients >= SSE_MAX_CLIENTS;
    if (!full) s_sse_clients++;
    portEXIT_CRITICAL(&s_sse_mux);
    if (full)
        return api_error(req, "503 Service Unavailable", "busy",
                         "event stream client limit reached", NULL);

    httpd_req_t *async_req = NULL;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        portENTER_CRITICAL(&s_sse_mux);
        s_sse_clients--;
        portEXIT_CRITICAL(&s_sse_mux);
        return api_error(req, "503 Service Unavailable", "busy",
                         "cannot start event stream", NULL);
    }
    httpd_resp_set_type(async_req, "text/event-stream");
    httpd_resp_set_hdr(async_req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(async_req, "Connection", "keep-alive");
    if (xTaskCreate(sse_task, "db_sse", 6144, async_req, 3, NULL) != pdPASS) {
        api_error(async_req, "503 Service Unavailable", "busy",
                  "cannot start event stream task", NULL);
        httpd_req_async_handler_complete(async_req);
        portENTER_CRITICAL(&s_sse_mux);
        s_sse_clients--;
        portEXIT_CRITICAL(&s_sse_mux);
        return ESP_OK;
    }
    return ESP_OK;
}

// Emit the current settings + their bounds as JSON (shared by GET /settings and
// the response to a successful POST /settings).
static esp_err_t settings_send(httpd_req_t *req)
{
    char buf[288];
    int n = snprintf(buf, sizeof buf,
        "{\"max\":%.1f,\"max_min\":%.1f,\"max_abs\":%.1f,"
        "\"comms_ms\":%u,\"comms_ms_min\":%u,\"comms_ms_max\":%u,"
        "\"leds_enabled\":%s}",
        (double)pb_heater_get_max_target_c(),
        (double)PB_HEATER_MIN_TARGET_C,
        (double)PB_HEATER_ABS_MAX_TARGET_C,
        (unsigned)pb_heater_get_comms_timeout_ms(),
        (unsigned)PB_HEATER_COMMS_TIMEOUT_MS_MIN,
        (unsigned)PB_HEATER_COMMS_TIMEOUT_MS_MAX,
        pb_leds_get_enabled() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

// GET /settings — current runtime-configurable safety settings + their bounds.
// Read-only, no auth (same as /status).
static esp_err_t settings_get(httpd_req_t *req)
{
    return settings_send(req);
}

// Parse a finite decimal temperature. Trailing non-whitespace is rejected so a
// partially numeric value can never silently become a safety setting.
static bool parse_temp(const char *s, float *out)
{
    if (!s || !*s) return false;
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s || !isfinite(v)) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end != '\0') return false;
    *out = v;
    return true;
}

// Parse an unsigned integer (no sign, no junk, no overflow past u32).
static bool parse_u32(const char *s, uint32_t *out)
{
    if (!s || !*s) return false;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end != '\0') return false;
    *out = (uint32_t)v;
    return true;
}

// POST /settings?max=<C>&comms_ms=<ms>&leds_enabled=<0|1> — update any subset.
// Auth-gated. Each field is optional; the safety values are clamped to the safe
// envelope by pb_heater's setters (the ceiling can never exceed 70 C; the comms
// deadman stays within [10s, 5min]). leds_enabled toggles the status-LED master
// enable. At least one recognized field must be present.
static esp_err_t settings_post(httpd_req_t *req)
{
    if (auth_reject(req)) return ESP_OK;

    char q[128];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) {
        // Fall back to a query-style body ("max=60&comms_ms=30000").
        int r = httpd_req_recv(req, q, sizeof q - 1);
        if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no settings given"); return ESP_FAIL; }
        q[r] = '\0';
    }

    bool applied = false;
    char v[24];
    if (httpd_query_key_value(q, "max", v, sizeof v) == ESP_OK) {
        float m;
        if (!parse_temp(v, &m)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad max"); return ESP_FAIL; }
        pb_heater_set_max_target_c(m);   // clamps + persists
        applied = true;
    }
    if (httpd_query_key_value(q, "comms_ms", v, sizeof v) == ESP_OK) {
        uint32_t ms;
        if (!parse_u32(v, &ms)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad comms_ms"); return ESP_FAIL; }
        pb_heater_set_comms_timeout_ms(ms);   // clamps + persists
        applied = true;
    }
    if (httpd_query_key_value(q, "leds_enabled", v, sizeof v) == ESP_OK) {
        uint32_t on;
        if (!parse_u32(v, &on)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad leds_enabled"); return ESP_FAIL; }
        pb_leds_set_enabled(on != 0);   // persists
        applied = true;
    }
    if (!applied) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no known settings (max, comms_ms, leds_enabled)");
        return ESP_FAIL;
    }
    return settings_send(req);   // echo the clamped result
}

// --- Sensor calibration (GET/POST /api/v2/calibration) ----------------------
// Per-channel temperature offset, hard-bounded to ±PB_NTC_OFFSET_MAX_C (±5 °C) in
// pb_ntc (on every set AND on NVS load). GET is read-only/open; POST is auth-gated.
static esp_err_t calibration_send(httpd_req_t *req)
{
    float ch_off  = pb_ntc_get_offset_c(PB_NTC_CHAMBER);
    float ptc_off = pb_ntc_get_offset_c(PB_NTC_PTC);
    // Live calibrated readings (NAN unless the last read was OK); the raw reading
    // is the calibrated value minus the applied offset, for the UI to show both.
    float ch_cal  = pb_ntc_last_status(PB_NTC_CHAMBER) == PB_NTC_OK
                    ? pb_ntc_smoothed_c(PB_NTC_CHAMBER) : NAN;
    float ptc_cal = pb_ntc_last_status(PB_NTC_PTC) == PB_NTC_OK
                    ? pb_ntc_smoothed_c(PB_NTC_PTC) : NAN;

    cJSON *o = cJSON_CreateObject();
    if (!o) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    cJSON_AddNumberToObject(o, "api_version", API_VERSION);
    add_num1(o, "chamber_offset_c", ch_off);
    add_num1(o, "ptc_offset_c", ptc_off);
    cJSON_AddNumberToObject(o, "min", -PB_NTC_OFFSET_MAX_C);
    cJSON_AddNumberToObject(o, "max",  PB_NTC_OFFSET_MAX_C);
    add_num1(o, "chamber_c", ch_cal);
    add_num1(o, "ptc_c", ptc_cal);
    add_num1(o, "chamber_raw_c", isfinite(ch_cal)  ? ch_cal  - ch_off  : NAN);
    add_num1(o, "ptc_raw_c",     isfinite(ptc_cal) ? ptc_cal - ptc_off : NAN);
    return send_json(req, o);
}

static esp_err_t calibration_get(httpd_req_t *req)
{
    return calibration_send(req);
}

// POST /api/v2/calibration — body {"chamber_offset_c":x,"ptc_offset_c":y} (either
// optional). Auth-gated. Each supplied offset is clamped to ±5 °C and persisted by
// pb_ntc; the clamped effective offsets are echoed back. At least one field required.
static esp_err_t calibration_post(httpd_req_t *req)
{
    if (auth_reject(req)) return ESP_OK;
    cJSON *root = recv_json(req);
    if (!root)
        return api_error(req, "400 Bad Request", "invalid_command", "invalid JSON body", NULL);

    bool applied = false;
    double v;
    if (json_number(root, "chamber_offset_c", &v)) {
        pb_ntc_set_offset_c(PB_NTC_CHAMBER, (float)v);   // clamps + persists
        applied = true;
    }
    if (json_number(root, "ptc_offset_c", &v)) {
        pb_ntc_set_offset_c(PB_NTC_PTC, (float)v);       // clamps + persists
        applied = true;
    }
    cJSON_Delete(root);
    if (!applied)
        return api_error(req, "400 Bad Request", "invalid_command",
                         "chamber_offset_c and/or ptc_offset_c required", NULL);
    return calibration_send(req);   // echo the clamped offsets
}

// POST /update — stream a new DragonBreath .bin into the inactive OTA slot, verify,
// set it as boot, and reboot. Auth-gated. SAFETY: refused while the heater is
// armed/on — a reboot mid-heat leaves the SSR state undefined until re-init, so
// the operator must turn the heater off first. Rollback (PENDING_VERIFY) means a
// bad image that crashes before app_main marks itself healthy reverts on reboot.
static bool device_armed(void);   // defined below; shared armed-mode guard

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

    // Block for ANY armed mode (chosen safety policy). Uses the same armed-mode
    // condition as restart/factory-reset so an armed AUTO waiting below the bed
    // threshold (target held at 0) is refused too, not just active output.
    if (device_armed())
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

// True whenever the device is in ANY armed mode (or the SSR is on). Armed AUTO
// deliberately holds effective_target_c at 0 while waiting below the bed
// threshold, so "armed" is inferred from the mode (the documented contract), not
// from output/target. Shared by every maintenance action that must be refused
// mid-heat (restart / factory-reset / OTA), so a reboot/erase/flash can never
// happen with the SSR in an undefined state.
static bool device_armed(void)
{
    pb_policy_snapshot_t snap;
    pb_policy_get_snapshot(&snap);
    return snap.mode != PB_MODE_OFF || snap.heater_output;
}

// True (and already responded with 409) when a mutating maintenance action must
// be refused because the device is armed/on.
static bool refuse_while_heating(httpd_req_t *req, const char *message)
{
    if (device_armed()) {
        api_error(req, "409 Conflict", "heater_active", message, NULL);
        return true;
    }
    return false;
}

// POST /api/v2/restart — auth-gated, refused while heating. Responds, then reboots
// from a delayed task so the JSON flushes before the socket is torn down.
static esp_err_t restart_post(httpd_req_t *req)
{
    if (auth_reject(req)) return ESP_OK;
    if (refuse_while_heating(req, "turn the heater off before restarting"))
        return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"restarting\"}");
    xTaskCreate(ota_reboot_task, "db_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// POST /api/v2/factory-reset?confirm=factory-reset — auth-gated, refused while
// heating, and requires the explicit confirm token (query or urlencoded body).
// Erases every key in the app_nvs namespace (Wi-Fi creds, Moonraker host, control
// token, safety limits) so the device reboots back to AP provisioning.
static esp_err_t factory_reset_post(httpd_req_t *req)
{
    if (auth_reject(req)) return ESP_OK;
    if (refuse_while_heating(req, "turn the heater off before a factory reset"))
        return ESP_OK;

    // Explicit confirmation gate: confirm=factory-reset in the query, or in an
    // application/x-www-form-urlencoded body.
    bool confirmed = false;
    char q[128] = {0};
    char v[32];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK
            && httpd_query_key_value(q, "confirm", v, sizeof v) == ESP_OK
            && strcmp(v, "factory-reset") == 0)
        confirmed = true;
    if (!confirmed && req->content_len > 0) {
        int r = httpd_req_recv(req, q, sizeof q - 1);
        if (r > 0) {
            q[r] = '\0';
            if (httpd_query_key_value(q, "confirm", v, sizeof v) == ESP_OK
                    && strcmp(v, "factory-reset") == 0)
                confirmed = true;
        }
    }
    if (!confirmed)
        return api_error(req, "400 Bad Request", "confirmation_required",
                         "factory reset requires confirm=factory-reset", NULL);

    // Fail closed: only reboot if the erase AND commit actually succeed — never
    // report success (and wipe-then-reboot) when configuration is still intact.
    nvs_handle_t h;
    esp_err_t err = nvs_open("app_nvs", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset failed (nvs err 0x%x); NOT rebooting", err);
        return api_error(req, "500 Internal Server Error", "erase_failed",
                         "factory reset failed — configuration not erased", NULL);
    }
    ESP_LOGW(TAG, "factory reset: app_nvs erased; rebooting to AP provisioning");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req,
        "{\"ok\":true,\"message\":\"factory reset; rebooting to AP provisioning\"}");
    xTaskCreate(ota_reboot_task, "db_factory", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// GET /api/v2/logs — read-only (open, like /state). Snapshots the pv_evlog ring
// (newest first) as JSON. Never energizes the heater or feeds the watchdog.
static esp_err_t logs_get(httpd_req_t *req)
{
    pv_evlog_entry_t *entries = calloc(PV_EVLOG_MAX_ENTRIES, sizeof *entries);
    if (!entries) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    size_t n = pv_evlog_snapshot(entries, PV_EVLOG_MAX_ENTRIES);
    cJSON *o = cJSON_CreateObject();
    if (!o) { free(entries); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    cJSON_AddNumberToObject(o, "api_version", API_VERSION);
    cJSON_AddNumberToObject(o, "count", n);
    cJSON *arr = cJSON_AddArrayToObject(o, "entries");
    for (size_t i = 0; arr && i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        if (!e) break;
        cJSON_AddNumberToObject(e, "ms", entries[i].ms);
        cJSON_AddStringToObject(e, "text", entries[i].text);
        cJSON_AddItemToArray(arr, e);
    }
    free(entries);
    return send_json(req, o);
}

// POST /api/v2/token — auth-gated. Body {"token":"..."} sets the control token
// (<=64 chars); {"token":""} / {"token":null} / {} clears it. Setting a token
// means subsequent mutating requests must send it (the caller keeps using it).
// The secret is never echoed back — only whether one is now configured.
static esp_err_t token_post(httpd_req_t *req)
{
    if (auth_reject(req)) return ESP_OK;
    cJSON *root = recv_json(req);
    if (!root)
        return api_error(req, "400 Bad Request", "invalid_command", "invalid JSON body", NULL);
    cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "token");
    const char *newtok = cJSON_IsString(t) ? t->valuestring : NULL;
    if (newtok && strlen(newtok) > 64) {
        cJSON_Delete(root);
        return api_error(req, "400 Bad Request", "invalid_command",
                         "token must be at most 64 characters", NULL);
    }

    nvs_handle_t h;
    if (nvs_open("app_nvs", NVS_READWRITE, &h) != ESP_OK) {
        cJSON_Delete(root);
        return api_error(req, "500 Internal Server Error", "internal", "nvs open failed", NULL);
    }
    esp_err_t err = (newtok && newtok[0])
        ? nvs_set_str(h, "ctl_token", newtok)
        : nvs_erase_key(h, "ctl_token");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;   // clearing an already-empty token
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    cJSON_Delete(root);
    if (err != ESP_OK)
        return api_error(req, "500 Internal Server Error", "internal", "nvs write failed", NULL);

    char cur[65];
    pb_httpd_ctl_token(cur, sizeof cur);
    cJSON *o = cJSON_CreateObject();
    if (!o) return ESP_FAIL;
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "token_set", cur[0] != '\0');
    return send_json(req, o);
}

esp_err_t pb_httpd_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;   // lets pb_portal add a "/*" captive catch-all
    cfg.max_uri_handlers = 22;                     // 21 used + 1 spare
    // The OTA handler hashes the image (mbedtls) with a 1 KB read buffer + the
    // app descriptor on-stack, which overflows the 4 KB default httpd task stack
    // (stack-protection panic). Give it headroom.
    cfg.stack_size = 8192;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) return err;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof s_device_id, "dragonbreath-%02x%02x%02x",
             mac[3], mac[4], mac[5]);
    snprintf(s_boot_id, sizeof s_boot_id, "%08lx%08lx%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random());

    httpd_uri_t info   = { .uri = "/api/v2/info",      .method = HTTP_GET,  .handler = info_get };
    httpd_uri_t state  = { .uri = "/api/v2/state",     .method = HTTP_GET,  .handler = state_get };
    httpd_uri_t cmd    = { .uri = "/api/v2/command",   .method = HTTP_POST, .handler = command_post };
    httpd_uri_t hb     = { .uri = "/api/v2/heartbeat", .method = HTTP_POST, .handler = heartbeat_post };
    httpd_uri_t events = { .uri = "/api/v2/events",    .method = HTTP_GET,  .handler = events_get };
    httpd_uri_t health = { .uri = "/api/v2/health",    .method = HTTP_GET,  .handler = health_get };
    httpd_uri_t upd    = { .uri = "/update",           .method = HTTP_POST, .handler = update_post };
    httpd_uri_t setg   = { .uri = "/settings",         .method = HTTP_GET,  .handler = settings_get };
    httpd_uri_t setp   = { .uri = "/settings",         .method = HTTP_POST, .handler = settings_post };
    httpd_uri_t logs   = { .uri = "/api/v2/logs",      .method = HTTP_GET,  .handler = logs_get };
    httpd_uri_t rst    = { .uri = "/api/v2/restart",   .method = HTTP_POST, .handler = restart_post };
    httpd_uri_t frst   = { .uri = "/api/v2/factory-reset", .method = HTTP_POST, .handler = factory_reset_post };
    httpd_uri_t tok    = { .uri = "/api/v2/token",     .method = HTTP_POST, .handler = token_post };
    httpd_uri_t calg   = { .uri = "/api/v2/calibration", .method = HTTP_GET,  .handler = calibration_get };
    httpd_uri_t calp   = { .uri = "/api/v2/calibration", .method = HTTP_POST, .handler = calibration_post };
    httpd_register_uri_handler(s_server, &info);
    httpd_register_uri_handler(s_server, &state);
    httpd_register_uri_handler(s_server, &cmd);
    httpd_register_uri_handler(s_server, &hb);
    httpd_register_uri_handler(s_server, &events);
    httpd_register_uri_handler(s_server, &health);
    httpd_register_uri_handler(s_server, &upd);
    httpd_register_uri_handler(s_server, &setg);
    httpd_register_uri_handler(s_server, &setp);
    httpd_register_uri_handler(s_server, &logs);
    httpd_register_uri_handler(s_server, &rst);
    httpd_register_uri_handler(s_server, &frst);
    httpd_register_uri_handler(s_server, &tok);
    httpd_register_uri_handler(s_server, &calg);
    httpd_register_uri_handler(s_server, &calp);
    ESP_LOGI(TAG, "HTTP API v2 up :80 (info/state/events/health/logs; command/heartbeat/restart/factory-reset/token/calibration; settings; OTA /update)");
    return ESP_OK;
}

httpd_handle_t pb_httpd_handle(void) { return s_server; }
