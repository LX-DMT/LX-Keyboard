#pragma once
#include "input_core.h"

/* One BMP scalar -> Windows EnableHexNumpad Alt-plus sequence. Caller owns
 * pacing and cancellation; no blocking, allocation or transport dependency. */
typedef struct { uint16_t code; uint8_t step; bool active; } lx_unicode_t;
bool lx_unicode_begin(lx_unicode_t *state,uint32_t code);
bool lx_unicode_next(lx_unicode_t *state,lx_key_report_t *report);
unsigned lx_unicode_selftest(void);
