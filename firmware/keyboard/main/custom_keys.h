#pragma once
#include "input_core.h"
#include "lx_protocol.h"
#include "esp_err.h"
void custom_keys_init(void);
void custom_keys_get(uint8_t actions[LX_CUSTOM_KEY_COUNT]);
esp_err_t custom_keys_save(const uint8_t actions[LX_CUSTOM_KEY_COUNT]);
void custom_keys_receive(const lx_packet_t *packet);
void custom_keys_print(void);
