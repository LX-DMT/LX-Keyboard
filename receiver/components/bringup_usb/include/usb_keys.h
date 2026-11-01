#pragma once
void usb_keys_cancel_pending(void);
#include "input_core.h"
#include "esp_err.h"
#include "hid_mouse.h"
void usb_consumer_submit(uint16_t usage);
void usb_mouse_submit(const lx_mouse_sample_t *sample);

esp_err_t usb_keys_start(void);
void usb_keys_submit(const lx_key_report_t *report,uint32_t sequence,int64_t event_us);
bool usb_keys_text_room(bool *pending);
void usb_keys_print_stats(void);
void usb_keys_trace_clear(void);
void usb_keys_trace_print(void);
uint8_t usb_keys_leds(void);
