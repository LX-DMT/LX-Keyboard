/**
 * @file board.c
 * @brief 板级 GPIO 初始化与外设使能
 *
 * 当前实现：RF 使能脚、LED 灯环电源脚。引脚定义见 board_pins.h。
 */

#include "board.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "board";

/** @brief 配置 RF 使能与 LED 电源 GPIO，默认关闭 */
esp_err_t board_init(void)
{
    gpio_config_t rf_cfg = {
        .pin_bit_mask = 1ULL << BOARD_RF_ENABLE_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rf_cfg));
    board_set_rf_enable(false);

    gpio_config_t led_pwr_cfg = {
        .pin_bit_mask = 1ULL << BOARD_LED_RING_PIN_PWR,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_pwr_cfg));
    gpio_set_level(BOARD_LED_RING_PIN_PWR, 0);

    ESP_LOGI(TAG, "Board GPIO initialized");
    return ESP_OK;
}

/**
 * @brief Surface Dial 模式下拉高 RF 使能，其余模式关闭
 * @param enable true=使能外设射频相关电路
 */
void board_set_rf_enable(bool enable)
{
    gpio_set_level(BOARD_RF_ENABLE_PIN, enable ? 1 : 0);
}
