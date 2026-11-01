#pragma once

/**
 * @file ble_dial.h
 * @brief BLE Surface Dial HID 模式 API
 */

#include <stdbool.h>

#include "esp_err.h"
#include "knob_events.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_dial_init(void);
/** @brief 处理旋转/按压事件并发送 HID（仅 active 时） */
void ble_dial_on_event(const knob_event_t *event);
void ble_dial_set_active(bool active);
bool ble_dial_is_connected(void);

#ifdef __cplusplus
}
#endif
