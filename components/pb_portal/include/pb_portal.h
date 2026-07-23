// SPDX-License-Identifier: MIT
// pb_portal — captive-portal / config web UI for DragonBreath. Registers its
// handlers on the shared pb_httpd server and (in AP mode) runs a captive DNS.
// Lets the user provision WiFi + Moonraker from a browser — nothing hardcoded.
// Call after pb_httpd_start().
#pragma once
#include "esp_err.h"

esp_err_t pb_portal_start(void);
