#include <string.h>
#include "unicode_keys.h"
bool lx_unicode_begin(lx_unicode_t *s,uint32_t code)
{
    /* Limit initial hardware HID path to printable BMP scalars. */
    if(code<0x20 || code>0xffff || (code>=0xd800 && code<=0xdfff)) return false;
    *s=(lx_unicode_t){.code=(uint16_t)code,.active=true};
    return true;
}
bool lx_unicode_next(lx_unicode_t *s,lx_key_report_t *report)
{
    if(!s->active) return false;
    *report=(lx_key_report_t){0};
    unsigned step=s->step++;
    if(step==11) { s->active=false; return true; }
    report->modifiers=4; /* Left Alt */
    if(step==1) report->keys[0]=0x57; /* Keypad + */
    if(step>=3 && step<=9 && (step&1)) {
        unsigned digit=(s->code>>(12-4*((step-3)/2)))&15;
        report->keys[0]=digit==0?0x62:digit<10?0x58+digit:0x04+digit-10;
    }
    return true;
}
unsigned lx_unicode_selftest(void)
{
    lx_unicode_t s={0}; lx_key_report_t r;
    unsigned errors=0,count=0;
    const uint8_t expected[]={0,0x57,0,0x5c,0,0x09,0,0x5e,0,0x62,0,0};
    if(!lx_unicode_begin(&s,0x4f60)) errors++;
    while(lx_unicode_next(&s,&r)) {
        if(count>=12 || r.keys[0]!=expected[count] || r.modifiers!=(count==11?0:4)) errors++;
        count++;
    }
    if(count!=12 || lx_unicode_begin(&s,0xd800) || lx_unicode_begin(&s,0x1f600) || lx_unicode_begin(&s,0)) errors++;
    return errors;
}
