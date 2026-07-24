#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Mirrors the real ceiling: pb_policy clamps persisted parameters against it.
#define PB_HEATER_ABS_MAX_TARGET_C 70.0f

esp_err_t pb_heater_set_target_c(float target_c);
float pb_heater_get_target_c(void);
float pb_heater_get_max_target_c(void);
uint32_t pb_heater_get_comms_timeout_ms(void);
void pb_heater_notify_link_alive(void);
void pb_heater_tick(void);
void pb_heater_emergency_off(const char *reason);
void pb_heater_request_panic_off(const char *reason);
void pb_heater_clear_fault(void);
bool pb_heater_is_inhibited(void);
bool pb_heater_is_faulted(void);
const char *pb_heater_fault_reason(void);
bool pb_heater_is_on(void);
bool pb_heater_heat_mode(void);
