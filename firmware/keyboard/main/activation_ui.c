#include <string.h>
#include "activation_ui.h"
#include "chat_official.h"
#include "chat_client.h"
#include "voice_capture.h"
#include "network_link.h"
#include "bringup_usb.h"
#include "power_policy.h"
#include "user_controls.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

/* USB-only, explicit start. A matching client poll renews a 5s lease.
 * State is RAM-only: idle/requesting/code/ready/error/cancelled/expired. */
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static struct { uint8_t state;int digit;char code[7];uint32_t owner,revision;
    esp_err_t error;int64_t lease,started;bool active,worker,running,queued; } s={.digit=-1};
static bool (*s_cancelled)(void);
static void finish_locked(uint8_t state,esp_err_t error)
{ s.active=false;s.state=state;s.error=error;s.digit=-1;memset(s.code,0,sizeof(s.code));s.revision++;if(!s.running)s.worker=false;s.queued=false; }
static void expire_locked(void)
{
    int64_t now=esp_timer_get_time();
    if(s.active && now>s.lease)finish_locked(6,ESP_ERR_TIMEOUT);
    else if(s.active && !s.running && now-s.started>10000000)finish_locked(4,ESP_ERR_TIMEOUT);
}
static bool cancelled(void)
{
    portENTER_CRITICAL(&s_lock);expire_locked();bool stop=!s.active;portEXIT_CRITICAL(&s_lock);
    return stop || (s_cancelled && s_cancelled());
}
uint32_t activation_ui_revision(void)
{
    /* Called by the existing controls task. Audio initialization resets the
     * voice queue: submit only after that generation is fully ready. */
    portENTER_CRITICAL(&s_lock);expire_locked();uint32_t owner=0;
    if(s.active && s.worker && !s.running && !s.queued && voice_audio_ready()) {owner=s.owner;s.queued=true;}
    portEXIT_CRITICAL(&s_lock);
    if(owner) {
        esp_err_t rc=voice_chat_setup_ui(owner);
        if(rc!=ESP_OK) {portENTER_CRITICAL(&s_lock);if(s.owner==owner && s.active)finish_locked(4,rc);portEXIT_CRITICAL(&s_lock);}
    }
    portENTER_CRITICAL(&s_lock);uint32_t rev=s.revision;portEXIT_CRITICAL(&s_lock);return rev;
}
void activation_ui_code(const char *code)
{
    portENTER_CRITICAL(&s_lock);expire_locked();
    if(s.active && s.running) { s.state=2;memcpy(s.code,code,6);s.code[6]=0; }
    portEXIT_CRITICAL(&s_lock);
}
void activation_ui_digit(int digit)
{
    portENTER_CRITICAL(&s_lock);expire_locked();
    if(s.active && s.running) { s.digit=digit;s.revision++; }
    portEXIT_CRITICAL(&s_lock);
}
static void overlay(uint8_t (*pixels)[3],unsigned count,bool active,int digit)
{
    if(!active)return;
    /* Reference app_led matrix row 1: `=26, 1=25,...9=17, 0=16 (zero based).
     * Battery overlay runs afterwards, including its low-voltage protection. */
    memset(pixels,0,count*3);
    if(digit>=0 && digit<=9) { unsigned index=digit?26-digit:16;
        if(index<count)memset(pixels[index],192,3); }
}
void activation_ui_overlay(uint8_t (*pixels)[3],unsigned count)
{
    portENTER_CRITICAL(&s_lock);expire_locked();bool active=s.active;int digit=s.digit;portEXIT_CRITICAL(&s_lock);
    overlay(pixels,count,active,digit);
}
unsigned activation_ui_selftest(void)
{
    unsigned errors=0;uint8_t pixels[96][3];
    memset(pixels,7,sizeof(pixels));overlay(pixels,96,false,1);
    for(unsigned i=0;i<sizeof(pixels);i++)if(((uint8_t *)pixels)[i]!=7)errors++;
    const unsigned indices[]={16,25,24,23,22,21,20,19,18,17};
    for(unsigned digit=0;digit<10;digit++) {
        overlay(pixels,96,true,digit);
        for(unsigned i=0;i<96;i++)for(unsigned c=0;c<3;c++)
            if(pixels[i][c]!=(i==indices[digit]?192:0))errors++;
        overlay(pixels,96,true,-1);
        for(unsigned i=0;i<sizeof(pixels);i++)if(((uint8_t *)pixels)[i])errors++;
    }
    portENTER_CRITICAL(&s_lock);
    if(s.active || s.worker)errors++;
    else {
        __typeof__(s) before=s;
        s.active=true;s.worker=true;s.running=false;s.lease=-1;s.digit=1;memcpy(s.code,"112200",7);
        expire_locked();
        if(s.active || s.worker || s.digit!=-1 || s.code[0] || s.state!=6)errors++;
        s.active=true;s.worker=true;s.running=false;s.lease=esp_timer_get_time()+5000000;s.started=esp_timer_get_time()-11000000;
        expire_locked();
        if(s.active || s.worker || s.state!=4)errors++;
        s.active=true;s.worker=true;s.running=true;
        finish_locked(5,ESP_OK);
        if(s.active || !s.worker || s.digit!=-1 || s.code[0] || s.state!=5)errors++;
        s=before;
    }
    portEXIT_CRITICAL(&s_lock);return errors;
}
esp_err_t activation_ui_run(uint32_t owner,bool allowed,bool (*stop)(void))
{
    portENTER_CRITICAL(&s_lock);expire_locked();
    bool valid=s.active && s.owner==owner && s.worker;
    if(valid)s.running=true;
    portEXIT_CRITICAL(&s_lock);
    if(!valid)return ESP_ERR_INVALID_STATE;
    s_cancelled=stop;esp_err_t rc=ESP_ERR_INVALID_STATE;
    if(allowed && !cancelled())rc=chat_official_setup(false,cancelled);
    if(rc==ESP_OK)rc=cancelled()?ESP_ERR_INVALID_STATE:chat_cloud_enable(true);
    portENTER_CRITICAL(&s_lock);expire_locked();
    if(s.active)finish_locked(rc==ESP_OK?3:4,rc);
    s.worker=false;s.running=false;portEXIT_CRITICAL(&s_lock);s_cancelled=NULL;return rc;
}
void activation_ui_receive(const lx_packet_t *packet)
{
    if(packet->length!=6 || packet->payload[0]!=1 || packet->payload[1]>2)return;
    const uint8_t *p=packet->payload;uint32_t owner=(uint32_t)p[2]|(uint32_t)p[3]<<8|(uint32_t)p[4]<<16|(uint32_t)p[5]<<24;
    uint8_t op=p[1];esp_err_t result=ESP_OK;bool start=false;
    portENTER_CRITICAL(&s_lock);expire_locked();
    if(op==1) {
        if(!owner || s.active || s.worker)result=ESP_ERR_INVALID_STATE;
        else {s.owner=owner;s.active=true;s.worker=true;s.queued=false;s.state=1;s.error=0;s.digit=-1;
            memset(s.code,0,sizeof(s.code));s.started=esp_timer_get_time();s.lease=s.started+5000000;s.revision++;start=true;}
    } else if(op==2) {
        if(owner && owner==s.owner && s.active)finish_locked(5,ESP_OK);
        else if(owner!=s.owner)result=ESP_ERR_INVALID_STATE;
    } else if(owner && owner==s.owner && s.active)s.lease=esp_timer_get_time()+5000000;
    portEXIT_CRITICAL(&s_lock);
    if(start) {
        if(!network_ready())result=ESP_ERR_WIFI_NOT_CONNECT;
        else if(voice_busy() || power_policy_shutting_down())result=ESP_ERR_INVALID_STATE;
        else {
            /* Same persisted preference and audio power lifecycle as knob ON.
             * The controls tick submits the job after audio initialization. */
            const uint8_t on[3]={8,0,0};
            result=user_controls_command(0x30,on,sizeof(on));
        }
        if(result!=ESP_OK) {portENTER_CRITICAL(&s_lock);finish_locked(4,result);s.worker=false;portEXIT_CRITICAL(&s_lock);}
        else power_policy_activity();
    }
    uint8_t reply[18]={1};
    portENTER_CRITICAL(&s_lock);expire_locked();reply[1]=s.state;reply[2]=s.active;reply[3]=(uint8_t)s.digit;
    memcpy(reply+4,s.code,6);reply[10]=(uint8_t)s.error;reply[11]=(uint8_t)(s.error>>8);
    for(unsigned i=0;i<4;i++)reply[12+i]=s.owner>>(8*i);
    portEXIT_CRITICAL(&s_lock);
    reply[16]=(uint8_t)result;reply[17]=(uint8_t)(result>>8);
    bringup_usb_packet(LX_MSG_ACTIVATION_REPLY,packet->sequence,reply,sizeof(reply));
}
