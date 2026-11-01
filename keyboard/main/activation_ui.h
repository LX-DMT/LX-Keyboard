#pragma once
#include "lx_protocol.h"
#include "esp_err.h"
#include <stdint.h>
void activation_ui_receive(const lx_packet_t *packet);
esp_err_t activation_ui_run(uint32_t owner,bool allowed,bool (*cancelled)(void));
void activation_ui_code(const char *code);
void activation_ui_digit(int digit);
void activation_ui_overlay(uint8_t (*pixels)[3],unsigned count);
uint32_t activation_ui_revision(void);
unsigned activation_ui_selftest(void);
