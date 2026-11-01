#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct { uint16_t bit; uint8_t report_id,width,kind; bool signed_value; } lx_mouse_field_t;
typedef struct {
    lx_mouse_field_t fields[32];
    uint16_t bits[8];
    uint8_t ids[8],reports,count;
    bool report_ids;
} lx_mouse_map_t;
typedef struct { int32_t x,y,wheel,pan; uint8_t buttons,button_mask; } lx_mouse_sample_t;
bool lx_mouse_parse(lx_mouse_map_t *map,const uint8_t *descriptor,size_t length);
bool lx_mouse_decode(const lx_mouse_map_t *map,const uint8_t *report,size_t length,lx_mouse_sample_t *sample);
unsigned lx_mouse_selftest(void);
