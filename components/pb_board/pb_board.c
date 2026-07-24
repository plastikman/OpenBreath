// SPDX-License-Identifier: MIT
#include "pb_board.h"
#include "esp_log.h"

static const char *TAG = "pb_board";

void pb_board_init(void)
{
#ifdef CONFIG_PB_HIL_DEVBOARD
    ESP_LOGW(TAG, "HIL dev-board target: production board GPIO init compiled out");
#else
    const gpio_config_t leds = {
        .pin_bit_mask = (1ULL << PB_GPIO_LED_K1) |
                        (1ULL << PB_GPIO_LED_K2) |
                        (1ULL << PB_GPIO_LED_K3),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&leds);
    gpio_set_level(PB_GPIO_LED_K1, 0);
    gpio_set_level(PB_GPIO_LED_K2, 0);
    gpio_set_level(PB_GPIO_LED_K3, 0);
    ESP_LOGI(TAG, "board init: LEDs off; heater/fan owned by their components");
#endif
}

int pb_board_rref_kohm(void)
{
#ifdef CONFIG_PB_HIL_DEVBOARD
    return 82;
#else
    const gpio_config_t strap = {
        .pin_bit_mask = (1ULL << PB_GPIO_RREF_STRAP),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&strap);
    int level = gpio_get_level(PB_GPIO_RREF_STRAP);
    int rref = (level == 0) ? 82 : 33;
    ESP_LOGI(TAG, "Rref strap (GPIO19)=%d -> %d kOhm", level, rref);
    return rref;
#endif
}
