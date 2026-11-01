#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct { void *encoder,*decoder;unsigned encode_max_us,decode_max_us,encode_frames;uint64_t encode_total_us; } voice_opus_t;
esp_err_t voice_opus_open(voice_opus_t *c,unsigned input_rate);
void voice_opus_close(voice_opus_t *c);
esp_err_t voice_opus_encode(voice_opus_t *c,const int16_t *pcm,size_t samples,uint8_t *out,size_t capacity,size_t *n);
esp_err_t voice_opus_decode(voice_opus_t *c,const uint8_t *packet,size_t n,int16_t *pcm,size_t capacity,size_t *samples);
unsigned voice_opus_selftest(void);
