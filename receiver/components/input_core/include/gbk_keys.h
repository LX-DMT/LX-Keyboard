#pragma once
#include "input_core.h"

/* GBK Alt-decimal compatibility path. Requires host Num Lock on.
 * Conversion is done before queueing text, outside the scan critical section. */
typedef struct { uint8_t digits[5],count,step; bool active; } lx_gbk_t;
bool lx_gbk_encode(uint32_t code,uint16_t *value);
void lx_gbk_begin_value(lx_gbk_t *state,uint16_t value);
bool lx_gbk_next(lx_gbk_t *state,lx_key_report_t *report);
unsigned lx_gbk_selftest(void);
