#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define LX_DISCOVERY_PORT 39572
#define LX_DISCOVERY_SIZE 14
/* Query identifies the intended receiver; reply identifies the actual board.
 * This is device selection on the LAN, not authentication. */
static inline void lx_discovery_encode(uint8_t bytes[LX_DISCOVERY_SIZE],bool reply,const uint8_t mac[6])
{
    memcpy(bytes,reply?"LXDISC1!":"LXDISC1?",8);memcpy(bytes+8,mac,6);
}
static inline bool lx_discovery_matches(const uint8_t *bytes,size_t size,bool reply,const uint8_t mac[6])
{
    return size==LX_DISCOVERY_SIZE && !memcmp(bytes,reply?"LXDISC1!":"LXDISC1?",8) && !memcmp(bytes+8,mac,6);
}
