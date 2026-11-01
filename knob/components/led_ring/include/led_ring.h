#pragma once

/**
 * @file led_ring.h
 * @brief 灯环 WS2812 控制 API
 */

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t led_ring_init(void);
esp_err_t led_ring_start(void);
/** @brief Surface Dial 模式开关，控制呼吸动画 */
void led_ring_set_dial_active(bool active);

#ifdef __cplusplus
}
#endif
