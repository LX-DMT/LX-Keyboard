#pragma once
void radio_keys_cancel_pending(void);
#include "esp_err.h"
#include "input_core.h"
#include "hid_mouse.h"
void radio_consumer_submit(uint16_t usage);
void radio_mouse_submit(const lx_mouse_sample_t *sample);
void radio_host_stats(const uint8_t values[3]);
void radio_set_stats_handler(void (*handler)(const uint8_t values[3]));
esp_err_t radio_keys_start(bool receiver);
void radio_keys_submit(const lx_key_report_t *report,uint32_t sequence,int64_t event_us);
bool radio_keys_text_room(bool *pending);
bool radio_input_active(void);
void radio_keys_stats(void);
void radio_keys_pause(unsigned milliseconds);
esp_err_t radio_keys_rate(unsigned mbps);
void radio_keys_trace_clear(void);
void radio_keys_trace_print(void);
