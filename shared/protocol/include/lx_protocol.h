#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define LX_PACKET_MAX 250
#define LX_PAYLOAD_MAX 234
enum { LX_MSG_KEYBOARD=1,LX_MSG_ACK=2,LX_MSG_KEEPALIVE=3,LX_MSG_LED_STATE=4,
       LX_MSG_MOUSE=5,LX_MSG_CONSUMER=6,LX_MSG_HELLO=7,LX_MSG_RELEASE=8,
       LX_MSG_HOST_STATS=9,LX_MSG_TEXT=10,LX_MSG_CONFIG=11,
       LX_MSG_CONTROL=12,LX_MSG_CONTROL_REPLY=13,LX_MSG_SETUP=14,LX_MSG_SETUP_REPLY=15,
       LX_MSG_ACTIVATION=16,LX_MSG_ACTIVATION_REPLY=17,
       LX_MSG_CUSTOM_KEYS=18,LX_MSG_CUSTOM_KEYS_REPLY=19,
       LX_MSG_LIGHTING=20,LX_MSG_LIGHTING_REPLY=21 };
typedef struct { uint8_t type; uint16_t length; uint32_t sequence,session; const uint8_t *payload; } lx_packet_t;
size_t lx_packet_encode(uint8_t *out,size_t capacity,uint8_t type,uint32_t sequence,
                        uint32_t session,const void *payload,uint16_t length);
bool lx_packet_decode(const uint8_t *data,size_t length,lx_packet_t *out);
typedef struct { uint8_t bytes[LX_PACKET_MAX];size_t length; } lx_stream_t;
typedef void (*lx_packet_callback_t)(const lx_packet_t *packet,void *arg);
void lx_stream_feed(lx_stream_t *stream,const uint8_t *data,size_t length,lx_packet_callback_t callback,void *arg);
unsigned lx_protocol_selftest(void);
