// SPDX-License-Identifier: MIT
// pb_httpd — tiny HTTP control API so a Klipper-side helper can surface the
// chamber in Fluidd (temp + settable target) and map M141/M191 to OpenBreath.
//
//   GET  /status            -> {"temp":33.4,"ptc":33.0,"target":0.0,
//                               "heating":false,"max":70.0}
//   POST /target?t=<C>      -> set chamber setpoint in C (0 = off). Also GET.
//                             -> {"target":<C>}
//
// Every request also feeds the heater comms-watchdog: the controller (the
// Klipper module) is expected to poll /status regularly, so if it stops the
// heater latches off. Start after WiFi is up.
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t pb_httpd_start(void);

// The shared HTTP server handle (valid after pb_httpd_start). pb_portal
// registers its config/captive handlers on this same server. NULL if not started.
httpd_handle_t pb_httpd_handle(void);
