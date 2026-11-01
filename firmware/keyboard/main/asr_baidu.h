#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
bool asr_baidu_configured(void);
esp_err_t asr_baidu_setting(uint8_t operation,const uint8_t *value,size_t n);
esp_err_t asr_baidu_recognize(const int16_t *pcm,size_t samples,char *text,size_t capacity);
unsigned asr_baidu_selftest(void);
