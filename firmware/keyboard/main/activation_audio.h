#pragma once
#include <stdbool.h>
#include "esp_err.h"
esp_err_t activation_audio_play(const char *code,bool (*cancelled)(void));
