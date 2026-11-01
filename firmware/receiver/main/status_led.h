#pragma once
#include "esp_err.h"

esp_err_t status_led_start(void);
void status_led_ready(void);
void status_led_fault(void);
void status_led_print(void);
