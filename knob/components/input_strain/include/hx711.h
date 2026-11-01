#pragma once

/**
 * @file hx711.h
 * @brief HX711 应变 ADC 驱动 API
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hx711_init(int dout_gpio, int sck_gpio);
/** @brief DOUT 为低表示转换完成可读 */
bool hx711_is_ready(void);
/** @brief 阻塞读取一次 24 位有符号原始值 */
int32_t hx711_read_raw(void);

#ifdef __cplusplus
}
#endif
