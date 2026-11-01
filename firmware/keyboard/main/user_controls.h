#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
esp_err_t user_controls_start(void);
esp_err_t user_controls_command(uint8_t type,const uint8_t *payload,size_t n);
bool user_controls_sound_on(void);
void user_controls_stats(void);
/* mode: steady, breathing, audio, blink, rainbow, off. speed: 1..100. */
typedef struct { uint8_t mode,h,s,v,speed; } user_lighting_t;
esp_err_t user_controls_lighting_get(user_lighting_t *out);
esp_err_t user_controls_lighting_set(const user_lighting_t *value);
