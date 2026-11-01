#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
esp_err_t wake_word_start(void);
void wake_word_reset(void);
void wake_word_suspend(void);
bool wake_word_ready(void);
bool wake_word_feed(const int16_t *pcm,size_t n);
void wake_word_stats(void);
