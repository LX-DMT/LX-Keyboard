#pragma once
#include "hid_mouse.h"
/* Caller supplies a short lock. Motion coalesces only within one button state. */
typedef struct { uint8_t buttons; int8_t x,y,wheel,pan; } lx_mouse_report_t;
typedef struct {
    lx_mouse_sample_t items[16];
    unsigned head,count;
    uint32_t overflow;
    uint8_t latest_buttons;
} lx_mouse_queue_t;
void lx_mouse_queue_push(lx_mouse_queue_t *queue,const lx_mouse_sample_t *sample);
bool lx_mouse_queue_take(lx_mouse_queue_t *queue,lx_mouse_report_t *report);
void lx_mouse_queue_resync(lx_mouse_queue_t *queue);
unsigned lx_mouse_queue_selftest(void);
