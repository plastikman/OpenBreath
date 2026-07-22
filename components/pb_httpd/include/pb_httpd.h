// SPDX-License-Identifier: MIT
// pb_httpd — tiny HTTP control API so a Klipper-side helper can surface the
// chamber in Fluidd (temp + settable target) and map M141/M191 to OpenBreath.
//
//   GET  /status       -> {temp,ptc,target,heating,fault,max}  READ-ONLY, no side effects
//   POST /target?t=<C> -> set chamber setpoint in C (0=off); also counts as liveness
//   POST /heartbeat    -> controller liveness only (pet the comms watchdog)
//   POST /reset        -> clear a latched safety fault (over-temp / sensor / comms)
//
// Liveness is explicit: the controller must POST /heartbeat (or /target)
// regularly while it wants heat; if it stops, the heater comms-watchdog latches
// off. GET /status never feeds the watchdog. Start after WiFi is up.
//
// CSRF / control gate: every mutating endpoint (/target, /reset, /heartbeat, and
// the portal's STA-mode /save) requires the custom header "X-OpenBreath-Auth".
// A cross-origin HTML form cannot set a custom header and we never enable CORS,
// so an ordinary drive-by page cannot command the heater or rewrite its Wi-Fi
// config. GET /status stays open (read-only). This is CSRF hardening for a
// trusted LAN, not transport security.
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"
#include <stddef.h>
#include <stdbool.h>

#define PB_AUTH_HEADER "X-OpenBreath-Auth"

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
