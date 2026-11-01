#pragma once
void ble_keys_cancel_pending(void);
#include <stdbool.h>
#include "hid_mouse.h"
void ble_consumer_submit(uint16_t usage);
void ble_mouse_submit(const lx_mouse_sample_t *sample);
void ble_keys_power(bool high);
void ble_keys_shutdown(void);
void ble_keys_power_tick(void);
unsigned ble_keys_policy_selftest(void);
void ble_keys_sleep(bool enabled);
#include "input_core.h"
#include "esp_err.h"
esp_err_t ble_keys_start(void);
void ble_keys_submit(const lx_key_report_t *report,uint32_t sequence,int64_t event_us);
bool ble_keys_text_room(bool *pending);
void ble_keys_stats(void);
void ble_keys_trace_clear(void);
void ble_keys_trace_print(void);
