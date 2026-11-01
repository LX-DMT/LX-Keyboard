#include "battery_policy.h"
#include <string.h>

#define LOW_MV 3750
#define LOADED_OK_MV 3800
#define REST_OK_MV 3900
static uint16_t maximum(uint16_t a,uint16_t b) { return a>b?a:b; }
void battery_policy_set_enabled(battery_policy_t *p,bool enabled)
{
    if(p->disabled==!enabled)return;
    uint32_t generation=p->generation+1;
    memset(p,0,sizeof(*p));p->disabled=!enabled;p->generation=generation;
}
uint16_t battery_policy_restore_mv(const battery_policy_t *p)
{ return maximum(maximum(REST_OK_MV,LOADED_OK_MV+p->sag_mv),p->retry_floor); }
static bool elapsed(uint32_t now,uint32_t start,uint32_t duration) { return start && now-start>=duration; }
static void reset_samples(battery_policy_t *p)
{ p->count=p->index=0;p->low_since=p->high_since=p->settle_since=p->peak_since=0; }
static void protect(battery_policy_t *p,uint32_t now,uint16_t mv,bool failed)
{
    p->state=BAT_LIGHT_OFF;p->generation++;p->trips++;
    p->before_mv=mv;p->settled=false;p->failed=failed;
    p->low_since=p->high_since=p->settle_since=p->check_since=p->peak_since=0;
    if(failed) {
        p->retry_after=now+60000;
        if(p->off_mv>mv && mv) { p->sag_mv=maximum(p->sag_mv,p->off_mv-mv);p->sag_known=true; }
    }
}
void battery_policy_step(battery_policy_t *p,uint32_t now,uint32_t sequence,bool valid,uint16_t mv,
                         bool awake,uint32_t setting,uint16_t peak_load,uint16_t applied_load,uint32_t applied_generation)
{
    if(p->disabled)return;
    if(!p->configured || setting!=p->setting) {
        if(p->configured && p->state==BAT_LIGHT_NORMAL) {
            p->sag_mv=p->retry_floor=0;p->retry_after=0;p->sag_known=false;
        }
        if(p->configured && p->state!=BAT_LIGHT_NORMAL) {
            /* Only a smaller requested load relaxes a failed attempt gate.
             * Colour/mode changes never pretend that more charge arrived. */
            if(peak_load<p->peak_load && p->peak_load) {
                p->sag_mv=(uint32_t)p->sag_mv*peak_load/p->peak_load;
                p->retry_floor=0;p->failed=false;
            } else { p->sag_mv=maximum(p->sag_mv,300);p->sag_known=true; }
            if(p->state==BAT_LIGHT_CHECK) protect(p,now,0,false);
            p->before_mv=0;p->settled=false;
        }
        p->configured=true;p->setting=setting;p->peak_load=peak_load;p->generation++;
        reset_samples(p);
    }
    if(p->awake!=awake) {
        reset_samples(p);
        if(p->state==BAT_LIGHT_CHECK) protect(p,now,0,false);
        if(p->state==BAT_LIGHT_OFF) {
            /* Idle invalidates an unfinished before/after measurement, not
             * the estimate already learned for the unchanged light setting.
             * A measured zero sag is also valid; it is not "unknown". */
            p->before_mv=0;p->settled=false;
            if(!p->sag_known) { p->sag_mv=maximum(p->sag_mv,300);p->sag_known=true; }
        }
        p->awake=awake;
    }
    if(!awake || !peak_load) { reset_samples(p);p->sequence=sequence;return; }
    if(!valid || mv<2500 || mv>4500) {
        reset_samples(p);
        /* A failed sensor cannot approve a trial that has already lit LEDs. */
        if(p->state==BAT_LIGHT_CHECK) protect(p,now,0,true);
        return;
    }
    if(sequence==p->sequence) return;
    if(p->last_ms && now-p->last_ms>1500) reset_samples(p);
    p->sequence=sequence;p->last_ms=now;
    p->history[p->index]=mv;p->index=(p->index+1)%3;
    if(p->count<3) p->count++;
    if(p->count<3) return;
    uint16_t a=p->history[0],b=p->history[1],c=p->history[2],tmp;
    if(a>b) {tmp=a;a=b;b=tmp;} if(b>c) {tmp=b;b=c;c=tmp;} if(a>b)b=a;
    p->filtered=b;
    if(p->state==BAT_LIGHT_NORMAL) {
        if(b<LOW_MV) {
            if(!p->low_since)p->low_since=now;
            if(elapsed(now,p->low_since,5000)) protect(p,now,b,false);
        } else p->low_since=0;
    } else if(p->state==BAT_LIGHT_OFF) {
        /* Start unloading measurements only after a complete black frame. */
        if(applied_generation!=p->generation) { p->settle_since=p->high_since=0;return; }
        if(!p->settled) {
            if(!p->settle_since)p->settle_since=now;
            if(!elapsed(now,p->settle_since,10000))return;
            if(p->before_mv) { p->sag_mv=maximum(p->sag_mv,b>p->before_mv?b-p->before_mv:0);p->sag_known=true; }
            else if(!p->sag_known) { p->sag_mv=300;p->sag_known=true; }
            if(p->failed) { p->retry_floor=maximum(p->retry_floor,b+50);p->failed=false; }
            p->settled=true;
        }
        uint16_t threshold=battery_policy_restore_mv(p);
        bool cooling=p->retry_after && (int32_t)(now-p->retry_after)<0;
        if(b>=threshold && b>=p->retry_floor && !cooling) {
            if(!p->high_since)p->high_since=now;
            if(elapsed(now,p->high_since,20000)) {
                p->state=BAT_LIGHT_CHECK;p->generation++;p->attempts++;
                p->check_since=p->peak_since=0;p->off_mv=b;p->high_since=0;
            }
        } else p->high_since=0;
    } else {
        if(applied_generation!=p->generation)return;
        if(!p->check_since)p->check_since=now;
        if(b<LOADED_OK_MV) { protect(p,now,b,true);return; }
        /* Dynamic effects must have actually reached a representative peak.
         * Keep checking quietly until audio/breathing supplies one. */
        if(!p->peak_since && (uint32_t)applied_load*10>=(uint32_t)peak_load*8) p->peak_since=now;
        if(elapsed(now,p->peak_since,5000)) {
            p->state=BAT_LIGHT_NORMAL;p->generation++;p->restores++;
            p->failed=false;p->retry_floor=0;p->retry_after=0;p->low_since=0;
        }
    }
}

/* Synthetic voltages only: never touches live settings, LEDs or the sensor. */
static void samples(battery_policy_t *p,uint32_t *now,unsigned n,uint16_t mv,bool awake,unsigned setting,unsigned load,unsigned applied)
{
    for(unsigned i=0;i<n;i++) { *now+=500;battery_policy_step(p,*now,*now,true,mv,awake,setting,load,applied,p->generation); }
}
unsigned battery_policy_selftest(void)
{
    unsigned errors=0;uint32_t now=0;battery_policy_t p={0};
    samples(&p,&now,12,3600,true,1,300,300);if(p.state!=BAT_LIGHT_NORMAL)errors++;
    samples(&p,&now,2,3600,true,1,300,300);if(p.state!=BAT_LIGHT_OFF)errors++;
    samples(&p,&now,180,3880,true,1,300,0);if(p.state!=BAT_LIGHT_OFF || p.attempts || p.sag_mv!=280)errors++;
    samples(&p,&now,44,4120,true,1,300,0);if(p.state!=BAT_LIGHT_CHECK)errors++;
    samples(&p,&now,16,3860,true,1,300,300);if(p.state!=BAT_LIGHT_NORMAL || p.restores!=1)errors++;
    /* Weak source: a failed trial cannot repeatedly flash at the same voltage. */
    samples(&p,&now,16,3600,true,1,300,300);
    samples(&p,&now,24,3880,true,1,300,0);
    samples(&p,&now,44,4120,true,1,300,0);
    samples(&p,&now,5,3700,true,1,300,300);if(p.state!=BAT_LIGHT_OFF)errors++;
    unsigned attempts=p.attempts;
    samples(&p,&now,240,4120,true,1,300,0);if(p.state!=BAT_LIGHT_OFF || p.attempts!=attempts)errors++;
    /* Lower brightness allows an automatic re-evaluation, without unplugging. */
    samples(&p,&now,70,4120,true,2,100,0);if(p.state!=BAT_LIGHT_CHECK)errors++;
    samples(&p,&now,20,3980,true,2,100,10);if(p.state!=BAT_LIGHT_CHECK)errors++;
    samples(&p,&now,14,3980,true,2,100,100);if(p.state!=BAT_LIGHT_NORMAL)errors++;
    /* Missing samples break continuity, and idle state never enables light. */
    memset(&p,0,sizeof(p));now=0;
    samples(&p,&now,8,3600,true,1,300,300);
    battery_policy_step(&p,now+20,now,false,0,true,1,300,300,p.generation);
    samples(&p,&now,8,3600,true,1,300,300);if(p.state!=BAT_LIGHT_NORMAL)errors++;
    samples(&p,&now,100,3600,false,1,300,0);if(p.state!=BAT_LIGHT_NORMAL)errors++;
    samples(&p,&now,16,3600,true,1,300,300);if(p.state!=BAT_LIGHT_OFF)errors++;
    samples(&p,&now,100,4250,false,1,300,0);if(p.state!=BAT_LIGHT_OFF)errors++;
    /* No restored output acknowledgement means no successful recovery. */
    samples(&p,&now,70,4250,true,1,300,0);
    for(unsigned i=0;i<40;i++) {now+=500;battery_policy_step(&p,now,now,true,4050,true,1,300,300,p.generation-1);}
    if(p.state!=BAT_LIGHT_CHECK || p.restores)errors++;
    /* Regression: idle/wake cannot raise a known 0/100mV estimate to 300mV.
     * Above the original threshold it must still perform a loaded trial. */
    for(unsigned sag=0;sag<=100;sag+=100) {
        memset(&p,0,sizeof(p));now=0;
        samples(&p,&now,16,3700,true,1,300,300);
        samples(&p,&now,26,3700+sag,true,1,300,0);
        if(!p.sag_known || p.sag_mv!=sag)errors++;
        samples(&p,&now,10,3950,false,1,300,0);
        if(p.sag_mv!=sag || battery_policy_restore_mv(&p)!=3900)errors++;
        samples(&p,&now,70,3950,true,1,300,0);
        if(p.state!=BAT_LIGHT_CHECK || p.sag_mv!=sag)errors++;
        samples(&p,&now,16,3850,true,1,300,300);
        if(p.state!=BAT_LIGHT_NORMAL || p.restores!=1)errors++;
    }
    /* An interrupted first measurement still uses the conservative fallback. */
    memset(&p,0,sizeof(p));now=0;
    samples(&p,&now,16,3700,true,1,300,300);
    samples(&p,&now,10,3950,false,1,300,0);
    if(p.sag_mv!=300 || battery_policy_restore_mv(&p)!=4100)errors++;
    /* Disabling releases an existing low-voltage block and prevents a new one.
     * Re-enabling uses fresh samples and the original debounce/thresholds. */
    memset(&p,0,sizeof(p));now=0;
    samples(&p,&now,16,3600,true,1,300,300);
    if(p.state!=BAT_LIGHT_OFF || p.disabled)errors++;
    uint32_t generation=p.generation;
    battery_policy_set_enabled(&p,false);
    if(p.state!=BAT_LIGHT_NORMAL || !p.disabled || p.generation==generation)errors++;
    samples(&p,&now,120,3400,true,1,300,300);
    if(p.state!=BAT_LIGHT_NORMAL)errors++;
    generation=p.generation;battery_policy_set_enabled(&p,false);
    if(p.generation!=generation)errors++;
    battery_policy_set_enabled(&p,true);
    samples(&p,&now,4,3600,true,1,300,300);
    if(p.state!=BAT_LIGHT_NORMAL || p.disabled)errors++;
    samples(&p,&now,16,3600,true,1,300,300);
    if(p.state!=BAT_LIGHT_OFF)errors++;
    return errors;
}
