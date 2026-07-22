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
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t pb_httpd_start(void);

// The shared HTTP server handle (valid after pb_httpd_start). pb_portal
// registers its config/captive handlers on this same server. NULL if not started.
httpd_handle_t pb_httpd_handle(void);
