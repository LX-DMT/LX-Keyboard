#include <stdio.h>
#include <stdatomic.h>
#include "power_policy.h"
#include "voice_capture.h"
#include "network_link.h"
#include "ble_keys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define DARK_US (180LL*1000000)
#define OFF_US (1800LL*1000000)
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last,s_preview_until;
static bool s_mouse_held,s_committed,s_manual;
typedef struct {
    bool initialized,raw,stable,armed,fired;
    int64_t changed,pressed;
} button_t;
static button_t s_button;
static unsigned button_update(button_t *b,bool down,int64_t now)
{
    if(!b->initialized) { *b=(button_t){.initialized=true,.raw=down,.stable=down,.changed=now};return 0; }
    if(down!=b->raw) { b->raw=down;b->changed=now; }
    if(now-b->changed<50000) return 0;
    unsigned event=0;
    if(b->stable!=down) { b->stable=down;event=1;if(down && b->armed) b->pressed=now; }
    if(!down) { b->armed=true;b->fired=false;b->pressed=0; }
    if(down && b->armed && !b->fired && now-b->pressed>=2000000) { b->fired=true;event=2; }
    return event;
}
static atomic_bool s_dark;
static uint8_t s_mode;
static esp_err_t (*s_request)(void);
static int64_t s_next_request,s_next_wifi,s_next_ble;
static int s_wifi_ps=-1;
static unsigned state_at(int64_t idle,bool busy)
{ return busy?0:idle>=OFF_US?2:idle>=DARK_US?1:0; }
void power_policy_init(uint8_t mode,esp_err_t (*request)(void))
{ s_mode=mode;s_request=request;s_last=esp_timer_get_time(); }
void power_policy_activity(void)
{
    portENTER_CRITICAL(&s_lock);
    if(!s_committed) { s_last=esp_timer_get_time();s_preview_until=0; }
    portEXIT_CRITICAL(&s_lock);
}
void power_policy_mouse(bool held,bool moved)
{
    portENTER_CRITICAL(&s_lock);
    if(!s_committed && (held!=s_mouse_held || moved || held)) {
        s_last=esp_timer_get_time();s_preview_until=0;
    }
    s_mouse_held=held;portEXIT_CRITICAL(&s_lock);
}
void power_policy_button(bool down)
{
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    unsigned event=button_update(&s_button,down,now);
    if(!s_committed && event) { s_last=now;s_preview_until=0;if(event==2) s_manual=true; }
    portEXIT_CRITICAL(&s_lock);
}
bool power_policy_dark(void) { return atomic_load(&s_dark); }
bool power_policy_shutting_down(void)
{ portENTER_CRITICAL(&s_lock);bool value=s_committed;portEXIT_CRITICAL(&s_lock);return value; }
bool power_policy_commit_shutdown(void)
{
    bool busy=voice_busy();int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    bool commit=!s_committed && (s_manual || state_at(now-s_last,busy || s_mouse_held)==2);
    if(commit) s_committed=true;
    portEXIT_CRITICAL(&s_lock);return commit;
}
void power_policy_tick(void)
{
    int64_t now=esp_timer_get_time();bool busy=voice_busy();
    portENTER_CRITICAL(&s_lock);
    busy|=s_mouse_held;
    if(busy && !s_committed) { s_last=now;s_preview_until=0; }
    int64_t idle=now-s_last;
    bool preview=s_preview_until>now,committed=s_committed,manual=s_manual;
    portEXIT_CRITICAL(&s_lock);
    unsigned state=state_at(idle,busy);
    bool dark=committed || state>=1 || preview;
    if(atomic_exchange(&s_dark,dark)!=dark)
        printf("LX_POWER lights=%s idle_ms=%lld\n",dark?"off":"on",(long long)(idle/1000));
    if(committed) return;
    if(s_mode==1) ble_keys_sleep(!busy && idle>=2000000);
    if(s_mode==1 && now>=s_next_ble) { s_next_ble=now+5000000;ble_keys_power_tick(); }
    if((state==2 || manual) && now>=s_next_request && s_request) {
        s_next_request=now+1000000;
        printf("LX_POWER shutdown_request=%s\n",esp_err_to_name(s_request()));
    }
    /* Only a background router connection may sleep. HID Wi-Fi / ESP-NOW
     * remain awake; the receiver's shared network defaults are unchanged. */
    bool save=(s_mode==0 || s_mode==1) && !busy;
    if((s_wifi_ps!=(int)save || network_power_save_enabled()!=save) && (!save || now>=s_next_wifi)) {
        s_next_wifi=now+1000000;
        esp_err_t rc=network_power_save(save);
        if(rc==ESP_OK) { s_wifi_ps=save;printf("LX_POWER wifi_ps=%u\n",save); }
    }
}
void power_policy_lights_test(void)
{
    /* Ten-second light-only lease; never advances the real shutdown timer. */
    portENTER_CRITICAL(&s_lock);
    if(!s_committed) s_preview_until=esp_timer_get_time()+10000000;
    portEXIT_CRITICAL(&s_lock);
}
void power_policy_stats(void)
{
    portENTER_CRITICAL(&s_lock);
    int64_t idle=esp_timer_get_time()-s_last;bool held=s_mouse_held,off=s_committed,armed=s_button.armed,down=s_button.stable,manual=s_manual;
    portEXIT_CRITICAL(&s_lock);
    printf("LX_POWER idle_ms=%lld lights_off=%u lights_after_s=180 shutdown_after_s=1800 committed=%u mouse_held=%u voice_busy=%u wifi_ps=%d cpu_mhz=240\n",
        (long long)(idle/1000),power_policy_dark(),off,held,voice_busy(),s_wifi_ps);
    printf("LX_POWER_BUTTON source=scan raw_bit=69 active_low=1 armed=%u down=%u requested=%u hold_ms=2000\n",armed,down,manual);
}
unsigned power_policy_selftest(void)
{
    const int64_t ages[]={0,DARK_US-1,DARK_US,DARK_US+1,OFF_US-1,OFF_US,OFF_US+1,48LL*3600*1000000};
    const unsigned expected[]={0,0,1,1,1,2,2,2};unsigned errors=0;
    for(unsigned i=0;i<sizeof(ages)/sizeof(ages[0]);i++) {
        if(state_at(ages[i],false)!=expected[i]) errors++;
        if(state_at(ages[i],true)!=0) errors++;
    }
    button_t b={0};
    if(button_update(&b,true,0) || button_update(&b,true,10000000) || b.armed) errors++;
    button_update(&b,false,11000000);button_update(&b,false,11050000);
    if(!b.armed) errors++;
    button_update(&b,true,12000000);button_update(&b,true,12050000);
    if(button_update(&b,true,14049999)==2 || button_update(&b,true,14050000)!=2) errors++;
    if(button_update(&b,true,18000000)==2) errors++;
    /* A short press or contact bounce never completes a hold. */
    button_update(&b,false,19000000);button_update(&b,false,19050000);
    button_update(&b,true,20000000);button_update(&b,true,20050000);
    button_update(&b,false,21000000);button_update(&b,false,21050000);
    if(button_update(&b,false,23000000)==2) errors++;
    button_update(&b,true,24000000);button_update(&b,false,24020000);
    if(button_update(&b,false,27000000)==2) errors++;
    return errors;
}
