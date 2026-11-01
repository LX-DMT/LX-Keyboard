#pragma once
#include <stdbool.h>
#include "esp_err.h"
/* Runs in the voice task; no firmware download and no credential logging. */
esp_err_t chat_official_setup(bool activate,bool (*cancelled)(void));
unsigned chat_official_selftest(void);
