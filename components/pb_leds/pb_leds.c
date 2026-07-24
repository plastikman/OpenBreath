// SPDX-License-Identifier: MIT
#include "pb_leds.h"

#include <stdatomic.h>

#include "pb_board.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "pb_leds";

// Persisted master-enable flag (shared app_nvs namespace). Default ON.
#define NVS_NS            "app_nvs"
#define KEY_LEDS_ENABLED  "leds_enabled"   // u32: 0 = off, nonzero = on

// Read from the driver task every tick and set by the HTTP task; atomic so the
// output can be gated without a lock. Defaults ON until pb_leds_load_config().
static _Atomic bool s_enabled = true;

// 50 ms base tick. Blink half-periods and the CODE timing are all counted in ticks.
#define TICK_MS           50
#define BLINK_TICKS        4    // 200 ms half-period  (~2.5 Hz)
#define BLINK_SLOW_TICKS  10    // 500 ms half-period  (~1 Hz)
#define CODE_ON_TICKS      3    // 150 ms per pulse (on)
#define CODE_OFF_TICKS     3    // 150 ms between pulses within a burst
#define CODE_GAP_TICKS    16    // 800 ms quiet gap after each burst

#ifndef CONFIG_PB_HIL_DEVBOARD
static const gpio_num_t s_gpio[PB_LED_COUNT] = {
    PB_GPIO_LED_K1, PB_GPIO_LED_K2, PB_GPIO_LED_K3, PB_GPIO_LED_POWER,
};
#endif

// The "Power" LED (index 3 / GPIO21) shares the UART0 console TX pin, so it is
// only claimed + driven when CONFIG_PB_POWER_LED is set (release builds). When
// unset, that pin stays the serial console and this LED index is skipped.
#ifdef CONFIG_PB_POWER_LED
#define PB_LED_DRIVEN(i)  (1)
#else
#define PB_LED_DRIVEN(i)  ((i) != PB_LED_K4)
#endif

// Pattern + CODE pulse-count are the only shared state; both atomic so any task
// can set them without a lock. All sequencing/phase lives in the driver task.
static _Atomic pb_led_pattern_t s_pat[PB_LED_COUNT];
static _Atomic unsigned         s_code[PB_LED_COUNT];
static TaskHandle_t             s_task;

static void led_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    // Per-LED CODE sequencer phase (task-local — never touched by setters).
    struct { unsigned pulse; unsigned sub; } cs[PB_LED_COUNT] = {0};

    for (;;) {
        for (int i = 0; i < PB_LED_COUNT; i++) {
            if (!PB_LED_DRIVEN(i)) continue;   // GPIO21 left as console TX
            pb_led_pattern_t p = atomic_load(&s_pat[i]);
            int on = 0;
            switch (p) {
            case PB_LED_SOLID:
                on = 1;
                break;
            case PB_LED_BLINK:
                on = ((tick / BLINK_TICKS) & 1u) == 0;
                break;
            case PB_LED_BLINK_SLOW:
                on = ((tick / BLINK_SLOW_TICKS) & 1u) == 0;
                break;
            case PB_LED_CODE: {
                unsigned n = atomic_load(&s_code[i]);
                if (n == 0) { on = 0; cs[i].pulse = 0; cs[i].sub = 0; break; }
                if (cs[i].pulse < n) {                       // inside the burst
                    on = (cs[i].sub < CODE_ON_TICKS);
                    if (++cs[i].sub >= CODE_ON_TICKS + CODE_OFF_TICKS) {
                        cs[i].sub = 0;
                        cs[i].pulse++;
                    }
                } else {                                     // quiet gap, then repeat
                    on = 0;
                    if (++cs[i].sub >= CODE_GAP_TICKS) {
                        cs[i].sub = 0;
                        cs[i].pulse = 0;
                    }
                }
                break;
            }
            case PB_LED_OFF:
            default:
                on = 0;
                cs[i].pulse = 0;   // reset the sequencer so re-entry starts clean
                cs[i].sub = 0;
                break;
            }
            // Master-enable gate: patterns above keep sequencing (so pb_policy's
            // indication is preserved), but when disabled the physical output is
            // forced off — the LEDs simply are not driven.
            if (!atomic_load(&s_enabled)) on = 0;
#ifndef CONFIG_PB_HIL_DEVBOARD
            gpio_set_level(s_gpio[i], on ? 1 : 0);
#else
            (void)on;
#endif
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t pb_leds_start(void)
{
    if (s_task) return ESP_ERR_INVALID_STATE;

#ifndef CONFIG_PB_HIL_DEVBOARD
    uint64_t mask = 0;
    for (int i = 0; i < PB_LED_COUNT; i++)
        if (PB_LED_DRIVEN(i)) mask |= 1ULL << s_gpio[i];   // skip GPIO21 unless enabled
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) return err;
#endif
    for (int i = 0; i < PB_LED_COUNT; i++) {
        atomic_store(&s_pat[i], PB_LED_OFF);
        atomic_store(&s_code[i], 0);
#ifndef CONFIG_PB_HIL_DEVBOARD
        if (PB_LED_DRIVEN(i)) gpio_set_level(s_gpio[i], 0);
#endif
    }

    if (xTaskCreate(led_task, "pb_leds", 2048, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
#ifdef CONFIG_PB_HIL_DEVBOARD
    ESP_LOGW(TAG, "HIL dev-board backend: LED GPIO writes compiled out");
#else
    ESP_LOGI(TAG, "started");
#endif
    return ESP_OK;
}

void pb_leds_set(pb_led_id_t id, pb_led_pattern_t pattern)
{
    if (id < 0 || id >= PB_LED_COUNT) return;
    atomic_store(&s_pat[id], pattern);
}

void pb_leds_set_code(pb_led_id_t id, uint8_t pulses)
{
    if (id < 0 || id >= PB_LED_COUNT) return;
    atomic_store(&s_code[id], pulses);
    atomic_store(&s_pat[id], PB_LED_CODE);
}

pb_led_pattern_t pb_leds_get(pb_led_id_t id)
{
    if (id < 0 || id >= PB_LED_COUNT) return PB_LED_OFF;
    return atomic_load(&s_pat[id]);
}

void pb_leds_load_config(void)
{
    bool enabled = true;                 // default ON if unset / NVS unavailable
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t v;
        if (nvs_get_u32(h, KEY_LEDS_ENABLED, &v) == ESP_OK) enabled = (v != 0);
        nvs_close(h);
    }
    atomic_store(&s_enabled, enabled);
    ESP_LOGI(TAG, "status LEDs %s", enabled ? "enabled" : "disabled");
}

esp_err_t pb_leds_set_enabled(bool enabled)
{
    atomic_store(&s_enabled, enabled);   // driver task honors it on the next tick
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, KEY_LEDS_ENABLED, enabled ? 1u : 0u);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "status LEDs %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

bool pb_leds_get_enabled(void) { return atomic_load(&s_enabled); }
