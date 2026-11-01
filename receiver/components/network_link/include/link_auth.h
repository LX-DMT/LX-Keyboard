#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool link_hmac(const uint8_t *key,size_t key_size,const uint8_t *data,size_t size,uint8_t out[32]);
bool link_equal(const uint8_t *a,const uint8_t *b,size_t n);
bool link_auth_socket(int fd,bool receiver);
uint32_t link_find_bound_peer(void);
bool link_discovery_reply(uint8_t *bytes,size_t length);
