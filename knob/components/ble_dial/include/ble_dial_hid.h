#pragma once

/**
 * @file ble_dial_hid.h
 * @brief BLE HID 底层：GATT 服务与报告发送
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_DIAL_KEY_PRESS   0x01
#define BLE_DIAL_KEY_RELEASE 0x00
#define BLE_DIAL_KEY_R       0xC8
#define BLE_DIAL_KEY_L       0x38

esp_err_t ble_dial_hid_init(void);
void ble_dial_hid_set_active(bool active);
bool ble_dial_hid_is_connected(void);
void ble_dial_hid_send_report(uint8_t key);

#ifdef __cplusplus
}
#endif
