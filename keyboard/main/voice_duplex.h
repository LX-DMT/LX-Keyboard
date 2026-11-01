#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct voice_duplex voice_duplex_t;
voice_duplex_t *voice_duplex_start(void);
esp_err_t voice_duplex_read(voice_duplex_t *d,int16_t pcm[320]);
esp_err_t voice_duplex_write(voice_duplex_t *d,const int16_t *pcm,size_t n);
void voice_duplex_clear(voice_duplex_t *d);
void voice_duplex_reply(voice_duplex_t *d,bool finished);
unsigned voice_playout_selftest(void);
size_t voice_duplex_pending(voice_duplex_t *d);
bool voice_duplex_has_space(voice_duplex_t *d);
esp_err_t voice_duplex_drain(voice_duplex_t *d,bool (*cancelled)(void));
void voice_duplex_stop(voice_duplex_t *d);
esp_err_t voice_duplex_test(bool (*cancelled)(void));
