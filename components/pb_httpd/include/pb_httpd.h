// SPDX-License-Identifier: MIT
// pb_httpd — authoritative DragonBreath HTTP/JSON API v2.
//
//   GET  /api/v2/info
//   GET  /api/v2/state
//   GET  /api/v2/events       Server-Sent Events: state + telemetry snapshots
//   GET  /api/v2/health
//   POST /api/v2/command      revision-aware mode/control command
//   POST /api/v2/heartbeat    refresh exactly one device-issued lease
//
// The alpha /status, /target, /heartbeat, and /reset contract is deliberately
// removed. Read-only requests have no control side effects. A remote POWER_ON
// session stays alive only when its exact device-issued lease is heartbeated.
//
// CSRF / control gate: every mutating endpoint (command, heartbeat, and
// the portal's STA-mode /save) requires the custom header "X-DragonBreath-Auth".
// A cross-origin HTML form cannot set a custom header and we never enable CORS,
// so an ordinary drive-by page cannot command the heater or rewrite its Wi-Fi
// config. Read-only API routes stay open. This is CSRF hardening for a
// trusted LAN, not transport security.
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"
#include <stddef.h>
#include <stdbool.h>

#define PB_AUTH_HEADER "X-DragonBreath-Auth"

esp_err_t pb_httpd_start(void);

// The shared HTTP server handle (valid after pb_httpd_start). pb_portal
// registers its config/captive handlers on this same server. NULL if not started.
httpd_handle_t pb_httpd_handle(void);

// True if the request carries a valid PB_AUTH_HEADER. If a control token is
// configured (NVS app_nvs/"ctl_token"), the header must equal it; otherwise any
// non-empty value passes (presence-only CSRF gate).
bool pb_httpd_auth_ok(httpd_req_t *req);

// Copy the configured control token into out ("" if none). The same-origin
// dashboard embeds this so its own fetch()es carry the correct header value.
void pb_httpd_ctl_token(char *out, size_t outsz);
