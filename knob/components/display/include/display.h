#pragma once

/**
 * @file display.h
 * @brief 圆屏与 LVGL 显示层 API
 */

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化 SPI 面板、LVGL 与 2ms tick 定时器 */
esp_err_t display_init(void);
/** @brief 获取 LVGL 递归互斥锁（UI 线程调用） */
void display_lock(void);
void display_unlock(void);
/** @brief 返回已注册的 LVGL 显示设备 */
lv_disp_t *display_get_lv_disp(void);

#ifdef __cplusplus
}
#endif
