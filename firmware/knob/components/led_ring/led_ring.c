/**
 * @file led_ring.c
 * @brief WS2812 灯环：Surface Dial 模式呼吸效果
 *
 * 独立任务 core 1；Dial 激活时全环 HSV 呼吸，否则熄灯。
 */

#include "led_ring.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "led_ring";

static led_strip_handle_t s_strip;
static TaskHandle_t s_led_task;
static volatile bool s_dial_active;
static uint8_t s_dial_hue;
static uint8_t s_dial_phase;
static uint16_t s_dial_pause_ms;

#define DIAL_BREATH_V_MIN       4
#define DIAL_BREATH_V_MAX       90
#define DIAL_BREATH_PHASE_MAX   128
#define DIAL_BREATH_TICK_MS     55
#define DIAL_BREATH_PAUSE_MS    2000

static void led_ring_apply_off(void)
{
    if (!s_strip) {
        return;
    }
    for (int i = 0; i < BOARD_LED_RING_COUNT; i++) {
        led_strip_set_pixel(s_strip, i, 0, 0, 0);
    }
    led_strip_refresh(s_strip);
}

/** @brief HSV(0..255) 转 RGB，供 led_strip_set_pixel 使用 */
static void led_ring_hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s == 0) {
        *r = *g = *b = v;
        return;
    }

    uint8_t region = h / 43;
    uint8_t remainder = (uint8_t)((h - (region * 43)) * 6);

    uint8_t p = (uint8_t)(((uint16_t)v * (255 - s)) >> 8);
    uint8_t q = (uint8_t)(((uint16_t)v * (255 - ((uint16_t)s * remainder >> 8))) >> 8);
    uint8_t t = (uint8_t)(((uint16_t)v * (255 - ((uint16_t)s * (255 - remainder) >> 8))) >> 8);

    switch (region) {
    case 0:
        *r = v;
        *g = t;
        *b = p;
        break;
    case 1:
        *r = q;
        *g = v;
        *b = p;
        break;
    case 2:
        *r = p;
        *g = v;
        *b = t;
        break;
    case 3:
        *r = p;
        *g = q;
        *b = v;
        break;
    case 4:
        *r = t;
        *g = p;
        *b = v;
        break;
    default:
        *r = v;
        *g = p;
        *b = q;
        break;
    }
}

static void led_ring_apply_dial_breath(void)
{
    if (!s_strip) {
        return;
    }

    const uint8_t half = DIAL_BREATH_PHASE_MAX / 2;
    uint8_t p = s_dial_phase;
    uint8_t v;
    if (p < half) {
        v = (uint8_t)(DIAL_BREATH_V_MIN +
                      ((uint16_t)p * (DIAL_BREATH_V_MAX - DIAL_BREATH_V_MIN)) / half);
    } else {
        v = (uint8_t)(DIAL_BREATH_V_MIN +
                      ((uint16_t)(DIAL_BREATH_PHASE_MAX - 1 - p) *
                       (DIAL_BREATH_V_MAX - DIAL_BREATH_V_MIN)) /
                          half);
    }

    uint8_t prev = s_dial_phase;
    s_dial_phase = (uint8_t)((s_dial_phase + 1) % DIAL_BREATH_PHASE_MAX);
    if (s_dial_phase < prev) {
        s_dial_hue = (uint8_t)(s_dial_hue + 37);
        s_dial_pause_ms = DIAL_BREATH_PAUSE_MS;
        led_ring_apply_off();
        return;
    }

    uint8_t r;
    uint8_t g;
    uint8_t b;
    led_ring_hsv_to_rgb(s_dial_hue, 255, v, &r, &g, &b);

    for (int i = 0; i < BOARD_LED_RING_COUNT; i++) {
        led_strip_set_pixel(s_strip, i, r, g, b);
    }
    led_strip_refresh(s_strip);
}

static void led_ring_reset_breath_state(void)
{
    s_dial_hue = 142;
    s_dial_phase = 0;
    s_dial_pause_ms = 0;
}

static void led_ring_task(void *arg)
{
    (void)arg;

    while (true) {
        if (!s_dial_active) {
            led_ring_apply_off();
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DIAL_BREATH_TICK_MS));
            continue;
        }

        if (s_dial_pause_ms > 0) {
            if (s_dial_pause_ms <= DIAL_BREATH_TICK_MS) {
                s_dial_pause_ms = 0;
                s_dial_phase = 0;
            } else {
                s_dial_pause_ms = (uint16_t)(s_dial_pause_ms - DIAL_BREATH_TICK_MS);
            }
        } else {
            led_ring_apply_dial_breath();
        }

        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DIAL_BREATH_TICK_MS));
    }
}

esp_err_t led_ring_init(void)
{
    gpio_set_level(BOARD_LED_RING_PIN_PWR, 1);

    led_strip_config_t strip_config = {
        .strip_gpio_num = BOARD_LED_RING_PIN_DATA,
        .max_leds = BOARD_LED_RING_COUNT,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    led_ring_apply_off();
    ESP_LOGI(TAG, "LED ring init (%d LEDs)", BOARD_LED_RING_COUNT);
    return ESP_OK;
}

esp_err_t led_ring_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(led_ring_task, "led_ring", 3072, NULL, 0, &s_led_task, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void led_ring_set_dial_active(bool active)
{
    if (active) {
        led_ring_reset_breath_state();
    } else {
        s_dial_pause_ms = 0;
        s_dial_phase = 0;
    }
    s_dial_active = active;
    if (s_led_task != NULL) {
        xTaskNotifyGive(s_led_task);
    }
}
