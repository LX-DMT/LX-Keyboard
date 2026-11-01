/**
 * @file hx711.c
 * @brief HX711 24 位 ADC 位 bang 读取
 *
 * DOUT 就绪低电平有效；SCK 脉冲读 24 位有符号值，增益 128 一次脉冲。
 */

#include "hx711.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"

static int s_dout = -1;
static int s_sck = -1;

esp_err_t hx711_init(int dout_gpio, int sck_gpio)
{
    s_dout = dout_gpio;
    s_sck = sck_gpio;

    gpio_config_t dout_cfg = {
        .pin_bit_mask = 1ULL << dout_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t sck_cfg = {
        .pin_bit_mask = 1ULL << sck_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&dout_cfg));
    ESP_ERROR_CHECK(gpio_config(&sck_cfg));
    gpio_set_level(sck_gpio, 0);
    return ESP_OK;
}

bool hx711_is_ready(void)
{
    return gpio_get_level(s_dout) == 0;
}

int32_t hx711_read_raw(void)
{
    int32_t value = 0;

    while (!hx711_is_ready()) {
        esp_rom_delay_us(10);
    }

    for (int i = 0; i < 24; i++) {
        gpio_set_level(s_sck, 1);
        esp_rom_delay_us(1);
        value = (value << 1) | gpio_get_level(s_dout);
        gpio_set_level(s_sck, 0);
        esp_rom_delay_us(1);
    }

    gpio_set_level(s_sck, 1);
    esp_rom_delay_us(1);
    gpio_set_level(s_sck, 0);
    esp_rom_delay_us(1);

    if (value & 0x800000) {
        value |= ~0xFFFFFF;
    }
    return value;
}
