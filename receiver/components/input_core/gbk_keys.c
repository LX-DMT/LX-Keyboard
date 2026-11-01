#include <stddef.h>
#include "gbk_keys.h"
static const struct { uint16_t unicode,gbk; } table[]={
#include "gbk_table.inc"
};
bool lx_gbk_encode(uint32_t code,uint16_t *value)
{
    if(code>=0x20 && code<=0x7e) { *value=(uint16_t)code;return true; }
    if(code<0x80 || code>0xffff || (code>=0xd800 && code<=0xdfff)) return false;
    size_t low=0,high=sizeof(table)/sizeof(table[0]);
    while(low<high) {
        size_t mid=low+(high-low)/2;
        if(table[mid].unicode<code) low=mid+1;else high=mid;
    }
    if(low>=sizeof(table)/sizeof(table[0])) return false;
    if(table[low].unicode!=code) return false;
    *value=table[low].gbk;return true;
}
void lx_gbk_begin_value(lx_gbk_t *s,uint16_t value)
{
    *s=(lx_gbk_t){.active=true};
    uint8_t reverse[5];
    do { reverse[s->count++]=value%10;value/=10; } while(value);
    for(unsigned i=0;i<s->count;i++) s->digits[i]=reverse[s->count-1-i];
}
bool lx_gbk_next(lx_gbk_t *s,lx_key_report_t *report)
{
    if(!s->active) return false;
    *report=(lx_key_report_t){0};
    unsigned step=s->step++;
    if(step==2*s->count+1) { s->active=false;return true; }
    report->modifiers=4;
    if(step&1) {
        unsigned digit=s->digits[(step-1)/2];
        report->keys[0]=digit?0x58+digit:0x62;
    }
    return true;
}
unsigned lx_gbk_selftest(void)
{
    unsigned errors=0;uint16_t value=0;
    const uint16_t unicode[]={0x4f60,0x597d,0xff0c,0x3002,0x4e02,'A'};
    const uint16_t gbk[]={0xc4e3,0xbac3,0xa3ac,0xa1a3,0x8140,65};
    for(unsigned i=0;i<sizeof(gbk)/sizeof(gbk[0]);i++)
        if(!lx_gbk_encode(unicode[i],&value) || value!=gbk[i]) errors++;
    if(lx_gbk_encode(0xd800,&value) || lx_gbk_encode(0x1f600,&value) || lx_gbk_encode(0x2603,&value)) errors++;
    lx_gbk_t s;lx_key_report_t report;unsigned count=0;
    const uint8_t expected[]={0,0x5d,0,0x62,0,0x5c,0,0x62,0,0x5b,0,0}; /* 50403 */
    lx_gbk_begin_value(&s,0xc4e3);
    while(lx_gbk_next(&s,&report)) {
        if(count>=12 || report.keys[0]!=expected[count] || report.modifiers!=(count==11?0:4)) errors++;
        count++;
    }
    if(count!=12) errors++;
    return errors;
}
