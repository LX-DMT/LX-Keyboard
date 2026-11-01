#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化板级 GPIO（RF 使能、LED 电源等） */
esp_err_t board_init(void);

/** @brief 控制 RF/外设使能脚（Surface Dial 模式使用） */
void board_set_rf_enable(bool enable);

#ifdef __cplusplus
}
#endif
