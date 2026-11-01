#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
esp_err_t knob_link_start(void);
void knob_host_stats(const uint8_t values[3]);
void knob_power_preview(bool pressed);
void knob_link_stats(void);
void knob_set_handler(esp_err_t (*handler)(uint8_t,const uint8_t *,size_t));
esp_err_t knob_inject(const uint8_t *bytes,size_t n);
