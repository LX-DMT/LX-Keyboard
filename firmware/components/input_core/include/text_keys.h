#pragma once
#include "unicode_keys.h"
#include "gbk_keys.h"
#define LX_TEXT_MAX 256
typedef enum { LX_TEXT_UNICODE=0,LX_TEXT_GBK=1,LX_TEXT_PINYIN=2 } lx_text_encoding_t;
typedef struct {
    uint16_t codes[LX_TEXT_MAX];
    unsigned count,index;
    lx_unicode_t unicode;
    lx_gbk_t gbk;
    lx_text_encoding_t encoding;
    bool direct_release;
    bool pinyin_active;
    uint8_t pinyin_step;
    uint16_t pinyin_code;
} lx_text_t;
/* Strict BMP UTF-8, plus CR/LF/TAB. Validate the entire string before output. */
bool lx_text_begin(lx_text_t *state,const char *utf8);
bool lx_text_begin_encoded(lx_text_t *state,const char *utf8,lx_text_encoding_t encoding);
bool lx_text_next(lx_text_t *state,lx_key_report_t *report);
bool lx_text_busy(const lx_text_t *state);
void lx_text_cancel(lx_text_t *state);
/* A cancelled VUC composition needs Escape before the user's next key. */
bool lx_text_composing(const lx_text_t *state);
unsigned lx_text_selftest(void);
