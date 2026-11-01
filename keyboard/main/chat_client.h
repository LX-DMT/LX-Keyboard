#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define CHAT_URI_CAP 512
#define CHAT_TOKEN_CAP 2048
esp_err_t chat_client_id(char uuid[37]);
esp_err_t chat_store_profile(const char *uri,const char *token,unsigned version);
esp_err_t chat_cloud_enable(bool enabled);
bool chat_configured(void);
esp_err_t chat_recognize(const int16_t *pcm,size_t samples,char *text,size_t capacity,bool (*cancelled)(void));
/* Reader supplies up to 960 samples; zero samples means end of utterance.
 * Submit runs on the voice task as soon as STT arrives, before socket cleanup. */
typedef esp_err_t (*chat_pcm_reader_t)(void *context,int16_t *pcm,size_t *samples);
esp_err_t chat_recognize_stream(chat_pcm_reader_t read,void *context,
    bool (*submit)(void *,const char *),bool (*cancelled)(void));
esp_err_t chat_recognize_test(bool (*cancelled)(void));
esp_err_t chat_setting(uint8_t operation,const uint8_t *value,size_t n);
/* Called only from the low-priority voice task. */
/* Optional, explicit diagnostic only: AEC output PCM accepted by WS send.
 * Normal/wake-triggered conversations pass NULL and retain no PCM. */
typedef struct { int16_t *pcm;size_t capacity,used; } chat_capture_t;
esp_err_t chat_run(bool (*cancelled)(void),chat_capture_t *capture);
esp_err_t chat_probe(bool (*cancelled)(void));
esp_err_t chat_local_test(bool (*cancelled)(void),chat_capture_t *capture);
void chat_stats(void);
