#include <stdio.h>
#include <string.h>
#include "battery_led.h"
#include "battery_policy.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static battery_policy_t s_policy;
static struct {
    bool valid,frame_valid;
    uint16_t mv,frame_mv;
    int16_t raw_ma;
    int64_t sampled_us;
    unsigned samples,errors,renders,other_lit;
    unsigned frame_errors;
    uint32_t applied_generation;
    uint16_t applied_load;
    bool frame_blocked,frame_idle;
    uint8_t level,brightness,pixels[4][3];
    esp_err_t output;
} s={.applied_generation=UINT32_MAX};
static unsigned level(uint16_t mv)
{
    return (mv>=3600)+(mv>=3750)+(mv>=3900)+(mv>=4050);
}
bool battery_led_protection_enabled(void)
{
    portENTER_CRITICAL(&s_lock);bool enabled=!s_policy.disabled;portEXIT_CRITICAL(&s_lock);return enabled;
}
void battery_led_protection_set(bool enabled)
{
    portENTER_CRITICAL(&s_lock);battery_policy_set_enabled(&s_policy,enabled);portEXIT_CRITICAL(&s_lock);
}
static void overlay(uint8_t pixels[BOARD_LED_COUNT][3],bool valid,uint16_t mv,uint8_t key_v,bool blocked)
{
    if(blocked)memset(pixels,0,BOARD_LED_COUNT*3);
    unsigned count=valid?level(mv):0;
    /* Old indicator rule: half of the 128/255 capped maximum is the floor.
     * Use the selected brightness, never the breathing/audio effect envelope. */
    uint8_t v=key_v<64?64:key_v>128?128:key_v;
    for(unsigned i=0;i<BATTERY_LED_COUNT;i++) {
        uint8_t *p=pixels[BATTERY_LED_FIRST+i];
        p[0]=v;p[1]=p[2]=i<count?v:0;
    }
}
void battery_led_sample(bool valid,uint16_t mv,int16_t raw_ma)
{
    portENTER_CRITICAL(&s_lock);
    s.valid=valid;s.mv=mv;s.sampled_us=esp_timer_get_time();s.samples++;
    s.raw_ma=raw_ma;
    if(!valid) s.errors++;
    portEXIT_CRITICAL(&s_lock);
}
void battery_led_tick(bool awake,uint32_t setting,uint16_t peak_load)
{
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    unsigned old=s_policy.state;
    battery_policy_step(&s_policy,(uint32_t)(now/1000),s.samples,
        s.valid && s.sampled_us && now-s.sampled_us<=1500000,s.mv,
        awake,setting,peak_load,s.applied_load,s.applied_generation);
    unsigned state=s_policy.state,gen=s_policy.generation,mv=s_policy.filtered,sag=s_policy.sag_mv;
    portEXIT_CRITICAL(&s_lock);
    if(old!=state)printf("LX_BATTERY_POLICY state=%u generation=%u filtered_mv=%u sag_mv=%u\n",state,gen,mv,sag);
}
bool battery_led_refresh_needed(void)
{
    portENTER_CRITICAL(&s_lock);
    bool needed=s_policy.generation!=s.applied_generation || s.output!=ESP_OK;
    portEXIT_CRITICAL(&s_lock);return needed;
}
bool battery_led_blocked(void)
{
    portENTER_CRITICAL(&s_lock);
    bool blocked=s_policy.state==BAT_LIGHT_OFF;
    portEXIT_CRITICAL(&s_lock);return blocked;
}
void battery_led_overlay(uint8_t pixels[BOARD_LED_COUNT][3],uint8_t key_v,battery_led_frame_t *frame)
{
    portENTER_CRITICAL(&s_lock);
    bool valid=s.valid && s.sampled_us && esp_timer_get_time()-s.sampled_us<=1500000;
    uint16_t mv=s.mv;
    frame->generation=s_policy.generation;frame->blocked=s_policy.state==BAT_LIGHT_OFF;frame->idle=false;
    portEXIT_CRITICAL(&s_lock);
    overlay(pixels,valid,mv,key_v,frame->blocked);
    unsigned other_lit=0;
    for(unsigned i=0;i<BOARD_LED_COUNT;i++)
        if((i<BATTERY_LED_FIRST || i>=BATTERY_LED_FIRST+BATTERY_LED_COUNT) &&
           (pixels[i][0] || pixels[i][1] || pixels[i][2])) other_lit++;
    frame->brightness=pixels[BATTERY_LED_FIRST][0];
    frame->mv=mv;frame->valid=valid;frame->other_lit=other_lit;
    frame->load=(unsigned)pixels[0][0]+pixels[0][1]+pixels[0][2];
    memcpy(frame->pixels,pixels[BATTERY_LED_FIRST],sizeof(frame->pixels));
}
void battery_led_result(esp_err_t result,const battery_led_frame_t *frame)
{
    portENTER_CRITICAL(&s_lock);s.output=result;s.renders++;
    if(result==ESP_OK && frame) {
        s.level=frame->valid?level(frame->mv):0;s.brightness=frame->brightness;
        s.frame_mv=frame->mv;s.frame_valid=frame->valid;s.frame_blocked=frame->blocked;
        s.frame_idle=frame->idle;
        s.other_lit=frame->other_lit;s.applied_load=frame->load;s.applied_generation=frame->generation;
        memcpy(s.pixels,frame->pixels,sizeof(s.pixels));
    } else s.frame_errors++;
    portEXIT_CRITICAL(&s_lock);
}
void battery_led_stats(void)
{
    portENTER_CRITICAL(&s_lock);__typeof__(s) copy=s;battery_policy_t policy=s_policy;portEXIT_CRITICAL(&s_lock);
    int64_t age=copy.sampled_us?(esp_timer_get_time()-copy.sampled_us)/1000:-1;
    printf("LX_BATTERY bus_mv=%u valid=%u age_ms=%lld level=%u brightness=%u indices=80,81,82,83 samples=%u errors=%u renders=%u output=%s pixels=",
        copy.mv,copy.valid && age>=0 && age<=1500,(long long)age,copy.level,copy.brightness,
        copy.samples,copy.errors,copy.renders,esp_err_to_name(copy.output));
    for(unsigned i=0;i<4;i++) printf("%s%02x%02x%02x",i?",":"",copy.pixels[i][0],copy.pixels[i][1],copy.pixels[i][2]);
    printf(" frame_mv=%u frame_valid=%u other_lit=%u raw_ma=%d display_ma=%d low_voltage_lighting=adaptive state=%u frame_low=%u generation=%lu applied_generation=%lu filtered_mv=%u sag_mv=%u retry_floor=%u trips=%u attempts=%u restores=%u frame_errors=%u restore_mv=%u sag_known=%u frame_idle=%u visual=not_measured low_voltage_shutdown=not_enabled protection_enabled=%u\n",
           copy.frame_mv,copy.frame_valid,copy.other_lit,copy.raw_ma,-(int)copy.raw_ma,
           policy.state,copy.frame_blocked,(unsigned long)policy.generation,(unsigned long)copy.applied_generation,
           policy.filtered,policy.sag_mv,policy.retry_floor,policy.trips,policy.attempts,policy.restores,copy.frame_errors,
           battery_policy_restore_mv(&policy),policy.sag_known,copy.frame_idle,!policy.disabled);
}
unsigned battery_led_selftest(void)
{
    /* Boundary cases and preservation of every non-indicator pixel. */
    const uint16_t mv[]={0,3599,3600,3749,3750,3899,3900,4049,4050,4200};
    const uint8_t expected[]={0,0,1,1,2,2,3,3,4,4};
    unsigned errors=battery_policy_selftest();
    for(unsigned i=0;i<sizeof(mv)/sizeof(mv[0]);i++) {
        uint8_t pixels[BOARD_LED_COUNT][3];memset(pixels,17,sizeof(pixels));
        overlay(pixels,true,mv[i],0,false);
        for(unsigned p=0;p<BOARD_LED_COUNT;p++) {
            bool indicator=p>=BATTERY_LED_FIRST && p<BATTERY_LED_FIRST+4;
            bool white=indicator && p-BATTERY_LED_FIRST<expected[i];
            if(pixels[p][0]!=(indicator?64:17) || pixels[p][1]!=(indicator?(white?64:0):17) || pixels[p][2]!=pixels[p][1]) errors++;
        }
    }
    uint8_t pixels[BOARD_LED_COUNT][3]={0};
    overlay(pixels,false,4200,128,false);
    for(unsigned p=80;p<84;p++) if(pixels[p][0]!=128 || pixels[p][1] || pixels[p][2]) errors++;
    overlay(pixels,true,4200,96,false);
    for(unsigned p=80;p<84;p++) for(unsigned c=0;c<3;c++) if(pixels[p][c]!=96) errors++;
    /* Multiple full off/restore frames, including a black user-selected effect. */
    for(unsigned turn=0;turn<6;turn++) {
        uint8_t value=turn==5?0:37;
        memset(pixels,value,sizeof(pixels));overlay(pixels,true,3600,96,true);
        for(unsigned p=0;p<BOARD_LED_COUNT;p++) {
            if(p>=80 && p<84)continue;
            for(unsigned c=0;c<3;c++)if(pixels[p][c])errors++;
        }
        memset(pixels,value,sizeof(pixels));overlay(pixels,true,4100,96,false);
        for(unsigned p=0;p<BOARD_LED_COUNT;p++)for(unsigned c=0;c<3;c++)
            if(pixels[p][c]!=((p>=80 && p<84)?96:value))errors++;
    }
    return errors;
}
