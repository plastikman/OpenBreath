#pragma once
#include <stdint.h>
#include "esp_err.h"

// Declarations only -- the host test implements these over an in-memory store so
// it can inspect what was committed and force write failures.
#define ESP_ERR_NVS_NOT_FOUND 0x1102

typedef enum {
    NVS_READONLY,
    NVS_READWRITE,
} nvs_open_mode_t;

typedef uint32_t nvs_handle_t;

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t mode,
                   nvs_handle_t *out_handle);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
