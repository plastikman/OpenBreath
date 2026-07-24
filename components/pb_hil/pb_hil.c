// SPDX-License-Identifier: MIT
#include "pb_hil.h"

#ifdef CONFIG_PB_HIL_CONSOLE

#include "pb_fan.h"
#include "pb_leds.h"
#include "pb_ntc.h"
#include "pb_policy.h"
#include "pb_buttons.h"

#include "cJSON.h"
#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "pb_hil";

#define HIL_LINE_MAX   768
#define HIL_TASK_STACK 6144

static int console_read_byte(uint8_t *byte)
{
#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return usb_serial_jtag_read_bytes(byte, 1, portMAX_DELAY);
#else
    return uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, byte, 1, portMAX_DELAY);
#endif
}

static const char *ntc_status_str(pb_ntc_status_t status)
{
    switch (status) {
        case PB_NTC_OK: return "ok";
        case PB_NTC_SHORT: return "short";
        case PB_NTC_OPEN: return "open";
        case PB_NTC_UNINIT:
        default: return "uninit";
    }
}

static const char *led_pattern_str(pb_led_pattern_t pattern)
{
    switch (pattern) {
        case PB_LED_SOLID: return "solid";
        case PB_LED_BLINK: return "blink";
        case PB_LED_BLINK_SLOW: return "blink_slow";
        case PB_LED_CODE: return "code";
        case PB_LED_OFF:
        default: return "off";
    }
}

static cJSON *state_json(void)
{
    pb_policy_snapshot_t snap;
    pb_policy_get_snapshot(&snap);

    cJSON *state = cJSON_CreateObject();
    cJSON_AddNumberToObject(state, "revision", snap.state_revision);
    cJSON_AddStringToObject(state, "mode", pb_policy_mode_str(snap.mode));
    cJSON_AddStringToObject(state, "source", pb_policy_source_str(snap.source));
    cJSON_AddNumberToObject(state, "requested_target_c", snap.requested_target_c);
    cJSON_AddNumberToObject(state, "effective_target_c", snap.effective_target_c);
    cJSON_AddBoolToObject(state, "heater_demand", snap.heater_demand);
    cJSON_AddBoolToObject(state, "heater_output", snap.heater_output);
    cJSON_AddNumberToObject(state, "fan_percent", snap.effective_fan_percent);
    cJSON_AddBoolToObject(state, "thermal_purge", snap.thermal_purge);
    cJSON_AddBoolToObject(state, "fault_latched", snap.fault_latched);
    cJSON_AddBoolToObject(state, "inhibited", snap.inhibited);
    cJSON_AddStringToObject(state, "fault_reason", snap.fault_reason);
    cJSON_AddBoolToObject(state, "moonraker_connected", snap.moonraker_connected);
    cJSON_AddNumberToObject(state, "bed_c", snap.bed_c);
    cJSON_AddBoolToObject(state, "auto_engaged", snap.auto_engaged);
    cJSON_AddBoolToObject(state, "lease_active", snap.lease_active);
    if (snap.lease_active) {
        cJSON_AddStringToObject(state, "lease_id", snap.lease_id);
        cJSON_AddNumberToObject(state, "lease_expires_ms", snap.lease_expires_ms);
    } else {
        cJSON_AddNullToObject(state, "lease_id");
        cJSON_AddNumberToObject(state, "lease_expires_ms", 0);
    }

    cJSON *sensors = cJSON_AddObjectToObject(state, "sensors");
    cJSON *chamber = cJSON_AddObjectToObject(sensors, "chamber");
    cJSON_AddStringToObject(chamber, "status", ntc_status_str(snap.chamber_status));
    if (snap.chamber_status == PB_NTC_OK && isfinite(snap.chamber_c))
        cJSON_AddNumberToObject(chamber, "temp_c", snap.chamber_c);
    else
        cJSON_AddNullToObject(chamber, "temp_c");
    cJSON *ptc = cJSON_AddObjectToObject(sensors, "ptc");
    cJSON_AddStringToObject(ptc, "status", ntc_status_str(snap.ptc_status));
    if (snap.ptc_status == PB_NTC_OK && isfinite(snap.ptc_c))
        cJSON_AddNumberToObject(ptc, "temp_c", snap.ptc_c);
    else
        cJSON_AddNullToObject(ptc, "temp_c");

    uint32_t zc_count = 0;
    uint32_t zc_interval_us = 0;
    pb_fan_zc_diag(&zc_count, &zc_interval_us);
    cJSON *io = cJSON_AddObjectToObject(state, "io");
#ifdef CONFIG_PB_HIL_DEVBOARD
    cJSON_AddStringToObject(io, "target", "devboard");
    cJSON_AddBoolToObject(io, "mains_gpio_compiled_out", true);
#else
    cJSON_AddStringToObject(io, "target", "panda");
    cJSON_AddBoolToObject(io, "mains_gpio_compiled_out", false);
#endif
    cJSON_AddNumberToObject(io, "zero_cross_count", zc_count);
    cJSON_AddNumberToObject(io, "zero_cross_interval_us", zc_interval_us);
    cJSON *leds = cJSON_AddObjectToObject(io, "leds");
    cJSON_AddStringToObject(leds, "power", led_pattern_str(pb_leds_get(PB_LED_POWER)));
    cJSON_AddStringToObject(leds, "auto", led_pattern_str(pb_leds_get(PB_LED_AUTO)));
    cJSON_AddStringToObject(leds, "on", led_pattern_str(pb_leds_get(PB_LED_ON)));
    cJSON_AddStringToObject(leds, "dry", led_pattern_str(pb_leds_get(PB_LED_DRY)));
#ifdef CONFIG_PB_HIL_DEVBOARD
    // Debounced button states, so a scenario can confirm an injected level
    // actually settled through the state machine before asserting an action.
    cJSON *buttons = cJSON_AddObjectToObject(io, "buttons");
    cJSON_AddBoolToObject(buttons, "power", pb_buttons_hil_pressed(PB_BUTTON_POWER));
    cJSON_AddBoolToObject(buttons, "auto", pb_buttons_hil_pressed(PB_BUTTON_AUTO));
    cJSON_AddBoolToObject(buttons, "on", pb_buttons_hil_pressed(PB_BUTTON_ON));
    cJSON_AddBoolToObject(buttons, "dry", pb_buttons_hil_pressed(PB_BUTTON_DRY));
#endif
    return state;
}

static void emit(cJSON *response)
{
    char *json = cJSON_PrintUnformatted(response);
    if (json) {
        printf("DBHIL %s\n", json);
        fflush(stdout);
        cJSON_free(json);
    }
    cJSON_Delete(response);
}

static cJSON *response_new(const cJSON *request)
{
    cJSON *response = cJSON_CreateObject();
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "id");
    if (id) cJSON_AddItemToObject(response, "id", cJSON_Duplicate(id, true));
    return response;
}

static void response_error(cJSON *response, const char *error)
{
    cJSON_AddBoolToObject(response, "ok", false);
    cJSON_AddStringToObject(response, "error", error);
    cJSON_AddItemToObject(response, "state", state_json());
}

static bool json_number(const cJSON *root, const char *key, double *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) return false;
    *out = item->valuedouble;
    return true;
}

#ifdef CONFIG_PB_HIL_DEVBOARD
static bool parse_ntc_status(const char *name, pb_ntc_status_t *status)
{
    if (strcmp(name, "ok") == 0) *status = PB_NTC_OK;
    else if (strcmp(name, "open") == 0) *status = PB_NTC_OPEN;
    else if (strcmp(name, "short") == 0) *status = PB_NTC_SHORT;
    else if (strcmp(name, "uninit") == 0) *status = PB_NTC_UNINIT;
    else return false;
    return true;
}

static bool parse_button_id(const char *name, pb_button_id_t *id)
{
    if (strcmp(name, "power") == 0) *id = PB_BUTTON_POWER;
    else if (strcmp(name, "auto") == 0) *id = PB_BUTTON_AUTO;
    else if (strcmp(name, "on") == 0) *id = PB_BUTTON_ON;
    else if (strcmp(name, "dry") == 0) *id = PB_BUTTON_DRY;
    else return false;
    return true;
}
#endif

static pb_policy_result_t run_mode_command(
    const cJSON *request, pb_policy_lease_t *lease)
{
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(request, "mode");
    if (!cJSON_IsString(mode)) return PB_POLICY_INVALID;
    if (strcmp(mode->valuestring, "off") == 0) {
        pb_policy_set_mode_off(PB_SOURCE_WEB);
        return PB_POLICY_OK;
    }

    double target = 0.0;
    if (!json_number(request, "target_c", &target)) return PB_POLICY_INVALID;
    if (strcmp(mode->valuestring, "power_on") == 0) {
        return pb_policy_set_power_on(
            (float)target, PB_SOURCE_WEB, "hil-serial",
            PB_POLICY_REVISION_ANY, lease);
    }
    if (strcmp(mode->valuestring, "auto") == 0) {
        double threshold = 0.0;
        if (!json_number(request, "bed_threshold_c", &threshold))
            return PB_POLICY_INVALID;
        return pb_policy_set_auto(
            (float)target, (float)threshold, PB_SOURCE_WEB,
            PB_POLICY_REVISION_ANY);
    }
    if (strcmp(mode->valuestring, "drying") == 0) {
        double hours = 0.0;
        if (!json_number(request, "hours", &hours)
                || hours < 1.0 || hours > 12.0 || floor(hours) != hours)
            return PB_POLICY_INVALID;
        return pb_policy_start_drying(
            (float)target, (uint8_t)hours, PB_SOURCE_WEB,
            PB_POLICY_REVISION_ANY);
    }
    return PB_POLICY_INVALID;
}

static void handle_request(const cJSON *request)
{
    cJSON *response = response_new(request);
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(request, "cmd");
    if (!cJSON_IsString(cmd)) {
        response_error(response, "missing_cmd");
        emit(response);
        return;
    }

    if (strcmp(cmd->valuestring, "ping") == 0
            || strcmp(cmd->valuestring, "state") == 0) {
        cJSON_AddBoolToObject(response, "ok", true);
    } else if (strcmp(cmd->valuestring, "off") == 0) {
        pb_policy_set_mode_off(PB_SOURCE_WEB);
        cJSON_AddBoolToObject(response, "ok", true);
    } else if (strcmp(cmd->valuestring, "mode") == 0) {
        pb_policy_lease_t lease = {0};
        pb_policy_result_t result = run_mode_command(request, &lease);
        cJSON_AddBoolToObject(response, "ok", result == PB_POLICY_OK);
        cJSON_AddStringToObject(response, "result", pb_policy_result_str(result));
        if (lease.id[0]) cJSON_AddStringToObject(response, "lease_id", lease.id);
    } else if (strcmp(cmd->valuestring, "heartbeat") == 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "lease_id");
        pb_policy_lease_t lease = {0};
        if (cJSON_IsString(id))
            snprintf(lease.id, sizeof lease.id, "%s", id->valuestring);
        pb_policy_result_t result = pb_policy_heartbeat(&lease);
        cJSON_AddBoolToObject(response, "ok", result == PB_POLICY_OK);
        cJSON_AddStringToObject(response, "result", pb_policy_result_str(result));
    } else if (strcmp(cmd->valuestring, "clear_fault") == 0) {
        pb_policy_snapshot_t snap;
        pb_policy_get_snapshot(&snap);
        pb_policy_result_t result =
            pb_policy_clear_fault(PB_SOURCE_WEB, snap.state_revision);
        cJSON_AddBoolToObject(response, "ok", result == PB_POLICY_OK);
        cJSON_AddStringToObject(response, "result", pb_policy_result_str(result));
#ifdef CONFIG_PB_HIL_DEVBOARD
    } else if (strcmp(cmd->valuestring, "sensor") == 0) {
        const cJSON *channel = cJSON_GetObjectItemCaseSensitive(request, "channel");
        const cJSON *status_item = cJSON_GetObjectItemCaseSensitive(request, "status");
        pb_ntc_status_t status = PB_NTC_UNINIT;
        double temp_c = 25.0;
        bool valid = cJSON_IsString(channel) && cJSON_IsString(status_item)
            && parse_ntc_status(status_item->valuestring, &status);
        if (status == PB_NTC_OK)
            valid = valid && json_number(request, "temp_c", &temp_c);
        pb_ntc_channel_t ch = PB_NTC_CHAMBER;
        if (valid && strcmp(channel->valuestring, "ptc") == 0)
            ch = PB_NTC_PTC;
        else if (!valid || strcmp(channel->valuestring, "chamber") != 0)
            valid = false;
        if (valid) {
            pb_ntc_hil_set(ch, status, (float)temp_c);
            cJSON_AddBoolToObject(response, "ok", true);
        } else {
            response_error(response, "invalid_sensor");
            emit(response);
            return;
        }
    } else if (strcmp(cmd->valuestring, "env") == 0) {
        const cJSON *connected =
            cJSON_GetObjectItemCaseSensitive(request, "connected");
        double bed_c = 0.0;
        if (!cJSON_IsBool(connected) || !json_number(request, "bed_c", &bed_c)) {
            response_error(response, "invalid_env");
            emit(response);
            return;
        }
        pb_policy_set_env((float)bed_c, cJSON_IsTrue(connected));
        cJSON_AddBoolToObject(response, "ok", true);
    } else if (strcmp(cmd->valuestring, "zero_cross") == 0) {
        double count = 1.0;
        double interval_us = 10000.0;
        const cJSON *count_item =
            cJSON_GetObjectItemCaseSensitive(request, "count");
        if (count_item && !json_number(request, "count", &count)) count = 0.0;
        const cJSON *interval_item =
            cJSON_GetObjectItemCaseSensitive(request, "interval_us");
        if (interval_item && !json_number(request, "interval_us", &interval_us))
            interval_us = 0.0;
        if (count < 1.0 || count > UINT32_MAX || floor(count) != count
                || interval_us < 0.0 || interval_us > UINT32_MAX) {
            response_error(response, "invalid_zero_cross");
            emit(response);
            return;
        }
        pb_fan_hil_zero_cross((uint32_t)count, (uint32_t)interval_us);
        cJSON_AddBoolToObject(response, "ok", true);
    } else if (strcmp(cmd->valuestring, "button") == 0) {
        // Inject a RAW electrical level so the real debounce/long-press timing
        // in pb_buttons runs -- scenarios drive levels + waits, not events.
        const cJSON *button_item =
            cJSON_GetObjectItemCaseSensitive(request, "button");
        double level = 0.0;
        pb_button_id_t id;
        bool valid = cJSON_IsString(button_item)
            && json_number(request, "level", &level)
            && (level == 0.0 || level == 1.0)
            && parse_button_id(button_item->valuestring, &id);
        if (!valid) {
            response_error(response, "invalid_button");
            emit(response);
            return;
        }
        pb_buttons_hil_set_level(id, (int)level);
        cJSON_AddBoolToObject(response, "ok", true);
#endif
    } else {
        response_error(response, "unsupported_cmd");
        emit(response);
        return;
    }

    cJSON_AddItemToObject(response, "state", state_json());
    emit(response);
}

static void hil_task(void *arg)
{
    (void)arg;
    char line[HIL_LINE_MAX];
    size_t used = 0;

    for (;;) {
        uint8_t byte = 0;
        int got = console_read_byte(&byte);
        if (got != 1) continue;
        if (byte == '\r') continue;
        if (byte != '\n') {
            if (used + 1 < sizeof line) line[used++] = (char)byte;
            else used = 0;
            continue;
        }
        line[used] = '\0';
        used = 0;
        if (!line[0]) continue;
        cJSON *request = cJSON_Parse(line);
        if (!request || !cJSON_IsObject(request)) {
            cJSON *response = cJSON_CreateObject();
            response_error(response, "invalid_json");
            emit(response);
            cJSON_Delete(request);
            continue;
        }
        handle_request(request);
        cJSON_Delete(request);
    }
}

esp_err_t pb_hil_start(void)
{
#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = 2048,
        .rx_buffer_size = 2048,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    usb_serial_jtag_vfs_use_driver();
#else
    esp_err_t err = uart_driver_install(
        CONFIG_ESP_CONSOLE_UART_NUM, 2048, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
#endif
    if (xTaskCreate(hil_task, "pb_hil", HIL_TASK_STACK, NULL, 8, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
#ifdef CONFIG_PB_HIL_DEVBOARD
    ESP_LOGW(TAG, "serial HIL ready (devboard; mains GPIO compiled out)");
#else
    ESP_LOGW(TAG, "serial HIL ready (Panda hardware active)");
#endif
    return ESP_OK;
}

#else

esp_err_t pb_hil_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
