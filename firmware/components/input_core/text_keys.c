#include <string.h>
#include "text_keys.h"
bool lx_text_busy(const lx_text_t *s) { return s->index<s->count || s->unicode.active || s->gbk.active || s->direct_release || s->pinyin_active; }
bool lx_text_composing(const lx_text_t *s) { return s->pinyin_active && s->pinyin_step>0 && s->pinyin_step<=14; }
void lx_text_cancel(lx_text_t *s) { s->count=s->index=0;s->unicode.active=false;s->gbk.active=false;s->direct_release=false;s->pinyin_active=false; }
bool lx_text_begin(lx_text_t *s,const char *utf8)
{ return lx_text_begin_encoded(s,utf8,LX_TEXT_UNICODE); }
bool lx_text_begin_encoded(lx_text_t *s,const char *utf8,lx_text_encoding_t encoding)
{
    if(encoding!=LX_TEXT_UNICODE && encoding!=LX_TEXT_GBK && encoding!=LX_TEXT_PINYIN) return false;
    lx_text_t next={.encoding=encoding};const unsigned char *p=(const unsigned char*)utf8;
    if(lx_text_busy(s)) return false;
    while(*p) {
        uint32_t code=*p++;
        if(code>=0xc2 && code<=0xdf) {
            if((*p&0xc0)!=0x80) return false;
            code=((code&31)<<6)|(*p++&63);
        } else if(code>=0xe0 && code<=0xef) {
            unsigned lead=code;
            if((*p&0xc0)!=0x80 || (lead==0xe0 && *p<0xa0) || (lead==0xed && *p>=0xa0)) return false;
            code=((code&15)<<6)|(*p++&63);
            if((*p&0xc0)!=0x80) return false;
            code=(code<<6)|(*p++&63);
        } else if(code>=0x80) return false;
        if(code==13) { code=10;if(*p==10) p++; }
        if((code<32 && code!=10 && code!=9) || code==127 || next.count==LX_TEXT_MAX) return false;
        uint16_t encoded=(uint16_t)code;
        if(encoding==LX_TEXT_GBK && code!=10 && code!=9 && !lx_gbk_encode(code,&encoded)) return false;
        next.codes[next.count++]=encoded;
    }
    if(!next.count) return false;
    *s=next;return true;
}
static bool pinyin_next(lx_text_t *s,lx_key_report_t *report)
{
    unsigned step=s->pinyin_step++;
    if(step==15) s->pinyin_active=false;
    if(step&1) return true;
    /* Microsoft Pinyin, Chinese/full-Pinyin mode: vuc4f60 + Space -> 你.
     * All printable characters take this route, preserving ASCII punctuation
     * and case regardless of the IME's Chinese punctuation preference. */
    const uint8_t prefix[]={0x19,0x18,0x06}; /* v u c */
    unsigned key=step/2;
    if(key<3) report->keys[0]=prefix[key];
    else if(key==7) report->keys[0]=0x2c;
    else {
        unsigned digit=(s->pinyin_code>>(12-4*(key-3)))&15;
        report->keys[0]=digit==0?0x27:digit<10?0x1d+digit:0x04+digit-10;
    }
    return true;
}
bool lx_text_next(lx_text_t *s,lx_key_report_t *report)
{
    if(!lx_text_busy(s)) return false;
    *report=(lx_key_report_t){0};
    if(s->direct_release) { s->direct_release=false;return true; }
    if(s->unicode.active) return lx_unicode_next(&s->unicode,report);
    if(s->gbk.active) return lx_gbk_next(&s->gbk,report);
    if(s->pinyin_active) return pinyin_next(s,report);
    unsigned code=s->codes[s->index++];
    if(code==10 || code==9) { report->keys[0]=code==10?0x28:0x2b;s->direct_release=true;return true; }
    if(s->encoding==LX_TEXT_GBK) { lx_gbk_begin_value(&s->gbk,code);return lx_gbk_next(&s->gbk,report); }
    if(s->encoding==LX_TEXT_PINYIN) { s->pinyin_code=code;s->pinyin_step=0;s->pinyin_active=true;return pinyin_next(s,report); }
    lx_unicode_begin(&s->unicode,code);return lx_unicode_next(&s->unicode,report);
}
unsigned lx_text_selftest(void)
{
    unsigned errors=0;lx_text_t s={0};lx_key_report_t report;
    if(!lx_text_begin(&s,"A\xe4\xbd\xa0\r\n\t") || s.count!=4 || s.codes[1]!=0x4f60) errors++;
    if(lx_text_begin(&s,"B")) errors++;
    unsigned count=0;while(lx_text_next(&s,&report)) count++;
    if(count!=28 || report.keys[0] || report.modifiers || lx_text_busy(&s)) errors++;
    const char *invalid[]={"\xc0\xaf","\xed\xa0\x80","\xf0\x9f\x98\x80","ok\xe4\xbd","\x1b",""};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) if(lx_text_begin(&s,invalid[i]) || lx_text_busy(&s)) errors++;
    char large[LX_TEXT_MAX+2];memset(large,'A',sizeof(large)-1);large[sizeof(large)-1]=0;
    if(lx_text_begin(&s,large)) errors++;
    if(!lx_text_begin(&s,"ok")) errors++;
    lx_text_next(&s,&report);lx_text_cancel(&s);
    if(lx_text_busy(&s) || lx_text_next(&s,&report)) errors++;
    if(!lx_text_begin_encoded(&s,"\xe4\xbd\xa0" "A\r\n\t",LX_TEXT_GBK) || s.codes[0]!=0xc4e3) errors++;
    count=0;while(lx_text_next(&s,&report)) count++;
    if(count!=22 || report.modifiers || report.keys[0]) errors++;
    if(lx_text_begin_encoded(&s,"ok\xe2\x98\x83",LX_TEXT_GBK) || lx_text_busy(&s)) errors++;
    if(!lx_text_begin_encoded(&s,"\xe4\xbd\xa0",LX_TEXT_GBK)) errors++;
    lx_text_next(&s,&report);lx_text_cancel(&s);
    if(lx_text_busy(&s) || lx_text_next(&s,&report)) errors++;
    if(!lx_text_begin_encoded(&s,"\xe4\xbd\xa0",LX_TEXT_PINYIN)) errors++;
    const uint8_t vuc[]={0x19,0,0x18,0,0x06,0,0x21,0,0x09,0,0x23,0,0x27,0,0x2c,0};
    count=0;while(lx_text_next(&s,&report)) {
        if(count>=sizeof(vuc) || report.keys[0]!=vuc[count] || report.modifiers) errors++;
        if(lx_text_composing(&s)!=(count<14)) errors++;
        count++;
    }
    if(count!=sizeof(vuc)) errors++;
    /* Cancel every unfinished prefix; never resume or leave a held key. */
    for(unsigned at=1;at<=16;at++) {
        lx_text_begin_encoded(&s,"\xe4\xbd\xa0",LX_TEXT_PINYIN);
        for(unsigned j=0;j<at;j++) lx_text_next(&s,&report);
        if(lx_text_composing(&s)!=(at<=14)) errors++;
        lx_text_cancel(&s);
        if(lx_text_busy(&s) || lx_text_composing(&s) || lx_text_next(&s,&report)) errors++;
    }
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)
        if(lx_text_begin_encoded(&s,invalid[i],LX_TEXT_PINYIN) || lx_text_busy(&s)) errors++;
    /* Repeated 0 digits must remain distinct downs; no held-key autorepeat. */
    if(!lx_text_begin_encoded(&s,"\xe1\x80\x80",LX_TEXT_PINYIN)) errors++;
    const uint8_t repeat[]={0x19,0,0x18,0,0x06,0,0x1e,0,0x27,0,0x27,0,0x27,0,0x2c,0};
    count=0;while(lx_text_next(&s,&report)) {
        if(count>=sizeof(repeat) || report.keys[0]!=repeat[count] || report.modifiers) errors++;
        count++;
    }
    if(count!=sizeof(repeat) || report.keys[0]) errors++;
    return errors;
}
