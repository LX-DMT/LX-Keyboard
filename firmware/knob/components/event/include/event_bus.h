#pragma once

/**
 * @file event_bus.h
 * @brief 旋钮事件总线 API
 */

#include "knob_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化事件队列 */
esp_err_t event_bus_init(void);

/** @brief 返回内部 FreeRTOS 队列句柄 */
QueueHandle_t event_bus_get_queue(void);

/**
 * @brief 发布事件
 * @param event 事件内容
 * @param timeout_ticks 发送超时
 */
esp_err_t event_bus_publish(const knob_event_t *event, TickType_t timeout_ticks);

/**
 * @brief 订阅（接收）事件
 * @param event 输出缓冲区
 * @param timeout_ticks 接收超时，portMAX_DELAY 永久阻塞
 */
bool event_bus_subscribe(knob_event_t *event, TickType_t timeout_ticks);

#ifdef __cplusplus
}
#endif
