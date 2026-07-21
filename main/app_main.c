// SPDX-License-Identifier: MIT
// OpenBreath — open firmware for the BIGTREETECH Panda Breath (ESP32-C3).
//
// Init order is safety-first: the heater SSR is forced OFF before anything can
// request heat, and the control loop enforces the over-temp cutoffs every tick.
// Networking (WiFi + captive portal + Moonraker) is intended to be imported from
// the OpenVent shared core; until then the device runs its local safety loop and
// holds the heater off (no controller -> comms watchdog keeps it off anyway).
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "pb_board.h"
#include "pb_ntc.h"
#include "pb_heater.h"
#include "pb_fan.h"
#include "pb_policy.h"

static const char *TAG = "openbreath";

#define PB_TICK_PERIOD_MS 500   // 2 Hz control loop

static void control_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(PB_TICK_PERIOD_MS);
    TickType_t last = xTaskGetTickCount();
    int dbg = 0;
    for (;;) {
        pb_policy_tick();
        // Bring-up diagnostic: log smoothed temps every ~2s so we can validate
        // the NTC path (Rref strap + room-temp sanity) on first flash.
        if (++dbg >= 4) {
            dbg = 0;
            ESP_LOGI(TAG, "temps: chamber=%.1f C, ptc=%.1f C  (heater %s)",
                     pb_ntc_smoothed_c(PB_NTC_CHAMBER),
                     pb_ntc_smoothed_c(PB_NTC_PTC),
                     pb_heater_is_on() ? "ON" : "off");
        }
        vTaskDelayUntil(&last, period);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "OpenBreath starting");

    pb_board_init();

    // Heater first, guaranteed OFF, before any other subsystem exists.
    ESP_ERROR_CHECK(pb_heater_init());

    ESP_ERROR_CHECK(pb_ntc_init());
    ESP_ERROR_CHECK(pb_fan_init());
    ESP_ERROR_CHECK(pb_policy_init());

    // TODO: import + start the OpenVent shared core here:
    //   pv_wifi_start();      // STA + AP fallback, NVS creds
    //   pv_portal_start();    // captive-portal WiFi/config web UI
    //   pv_moonraker_start(); // WS client -> feeds pb_policy_apply()

    xTaskCreate(control_task, "pb_control", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "control loop running at %d ms; heater held OFF until a controller connects",
             PB_TICK_PERIOD_MS);
}
