// SPDX-License-Identifier: MIT
// Minimal UDP DNS server that answers every A-record with a fixed IPv4 address
// (the AP gateway), so captive-portal probes resolve to the config page.
// Adapted from OpenVent's pv_dns (generic infra).
#pragma once

#include "esp_err.h"
#include <stdint.h>

esp_err_t pb_dns_start(uint32_t redirect_ip);   // ip in network byte order
void      pb_dns_stop(void);
