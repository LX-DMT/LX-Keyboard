#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
esp_err_t voice_capture_start(void);
void voice_set_text_handler(bool (*submit)(const char *),bool (*busy)(void),void (*cancel)(void));
/* Request is nonblocking; the voice owner performs the power transition. */
void voice_audio_enable(bool enabled);
bool voice_audio_ready(void);
bool voice_busy(void);
bool voice_audio_stopped(void);
void voice_ptt(bool down);
void voice_cancel(void);
void voice_suspend(void);
void voice_wake_enable(bool enabled);
esp_err_t voice_chat_request(void);
esp_err_t voice_replay(uint8_t action,const int16_t *pcm,size_t n);
esp_err_t voice_test_capture(unsigned duration_ms);
esp_err_t voice_test_codec(unsigned duration_ms);
esp_err_t voice_test_opus(void);
esp_err_t voice_test_chat(void);
esp_err_t voice_chat_setup(bool activate);
esp_err_t voice_chat_setup_ui(uint32_t owner);
esp_err_t voice_chat_probe(void);
esp_err_t voice_chat_diagnostic(bool local);
esp_err_t voice_test_activation(void);
esp_err_t voice_test_aec(void);
esp_err_t voice_test_asr(void);
/* Explicit timed microphone -> official STT -> normal text output test. */
esp_err_t voice_test_dictation(unsigned duration_ms);
void voice_capture_stats(void);
void voice_capture_dump(void);
