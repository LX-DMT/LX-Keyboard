#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lx_protocol.h"
void bringup_usb_set_handler(lx_packet_callback_t handler,void *arg);
bool bringup_usb_packet(uint8_t type,uint32_t sequence,const void *payload,uint16_t length);
esp_err_t bringup_usb_start(bool receiver);
bool bringup_usb_mounted(void);
const uint8_t *bringup_hid_descriptor(uint16_t *length);
