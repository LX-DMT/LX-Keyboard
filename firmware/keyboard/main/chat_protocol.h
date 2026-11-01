#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Official binary framing: v1 raw Opus, v2 16-byte, v3 4-byte header. */
size_t chat_audio_header(unsigned version);
bool chat_audio_wrap(unsigned version,uint8_t *packet,size_t payload,uint32_t timestamp);
bool chat_audio_unwrap(unsigned version,const uint8_t *packet,size_t n,const uint8_t **audio,size_t *size);
#define CHAT_MESSAGE_MAX 4096
typedef struct {
    uint8_t bytes[CHAT_MESSAGE_MAX+1];
    size_t used,offset,payload;
    uint8_t opcode;
    bool active;
} chat_rx_t;
/* 1 = complete message; 0 = more data/control frame; -1 = invalid/too large. */
int chat_rx_feed(chat_rx_t *r,uint8_t opcode,bool fin,size_t payload,size_t offset,const void *data,size_t n);
typedef enum { CHAT_IGNORE,CHAT_HELLO,CHAT_TTS_START,CHAT_TTS_STOP,CHAT_GOODBYE,CHAT_INVALID,CHAT_STT } chat_kind_t;
typedef struct { chat_kind_t kind;unsigned rate;char session[97]; } chat_message_t;
chat_message_t chat_json(const char *text,size_t n);
/* STT only: byte count, 0 for empty/non-STT, -1 for malformed/too long. */
int chat_stt_text(const char *json,size_t n,char *out,size_t cap);
unsigned chat_protocol_selftest(void);
