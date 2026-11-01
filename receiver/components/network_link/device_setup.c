#include <string.h>
#include "device_setup.h"
#include "bringup_usb.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"

/* One NVS blob is the transaction boundary. Unsealed work rolls back on boot.
 * Legacy lx_net credentials are read-only during setup and never exported. */
typedef struct { uint8_t version, pending; lx_setup_config_t current, previous; } record_t;
static record_t s_record;
static lx_setup_config_t s_stage;
static bool s_receiver, s_session, s_changed_wifi, s_tested, s_committed, s_wifi_staged;
static uint8_t s_token[16];
static int64_t s_deadline;
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_request[LX_PAYLOAD_MAX];
static size_t s_length;
static uint32_t s_sequence;
static bool s_queued;
static wifi_ap_record_t *s_aps;
static uint16_t s_ap_count;

static esp_err_t save_record(const record_t *record)
{
    nvs_handle_t h;esp_err_t rc=nvs_open("lx_setup",NVS_READWRITE,&h);
    if(rc!=ESP_OK) return rc;
    rc=nvs_set_blob(h,"config",record,sizeof(*record));
    if(rc==ESP_OK) rc=nvs_commit(h);
    nvs_close(h);return rc;
}
void device_setup_init(bool receiver)
{
    s_receiver=receiver;s_record.version=1;s_record.current.version=1;
    nvs_handle_t h;size_t n=sizeof(s_record);record_t record={0};
    if(nvs_open("lx_setup",NVS_READONLY,&h)==ESP_OK) {
        esp_err_t rc=nvs_get_blob(h,"config",&record,&n);nvs_close(h);
        if(rc==ESP_OK && n==sizeof(record) && record.version==1 && record.current.version==1) {
            if(record.pending) { record.current=record.previous;record.pending=0;save_record(&record); }
            s_record=record;
        }
    }
}
bool device_setup_bound(void) { return (s_record.current.flags&6)!=0; }
bool device_setup_busy(void) { return s_session; }
bool device_setup_profile(char ssid[33],char password[64])
{
    if(!(s_record.current.flags&1)) return false;
    memcpy(ssid,s_record.current.ssid,33);memcpy(password,s_record.current.password,64);return true;
}
bool device_setup_peer(uint8_t mac[6],uint8_t key[16],bool *encrypted)
{
    *encrypted=(s_record.current.flags&2)!=0;
    if(*encrypted) { memcpy(mac,s_record.current.peer,6);memcpy(key,s_record.current.pair_key,16);return true; }
    /* Public builds require explicit provisioning; no development-board fallback. */
    memset(mac,0,6);memset(key,0,16);return false;
}
bool device_setup_pc(uint8_t id[16],uint8_t key[32])
{
    if(!(s_record.current.flags&4)) return false;
    memcpy(id,s_record.current.pc_id,16);memcpy(key,s_record.current.pc_key,32);return true;
}
static esp_err_t rollback(void)
{
    if(s_committed) {
        record_t old=s_record;old.current=old.previous;old.pending=0;
        esp_err_t rc=save_record(&old);if(rc!=ESP_OK) return rc;
        s_record=old;s_committed=false;
    }
    if(s_changed_wifi) network_trial_end();
    s_session=false;s_changed_wifi=false;s_tested=false;
    free(s_aps);s_aps=NULL;s_ap_count=0;
    memset(&s_stage,0,sizeof(s_stage));memset(s_token,0,sizeof(s_token));return ESP_OK;
}
void device_setup_receive(const lx_packet_t *p)
{
    if(!p->length || p->length>LX_PAYLOAD_MAX) return;
    bool accepted=false;
    portENTER_CRITICAL(&s_lock);
    if(!s_queued) { memcpy(s_request,p->payload,p->length);s_length=p->length;s_sequence=p->sequence;s_queued=true;accepted=true; }
    portEXIT_CRITICAL(&s_lock);
    if(!accepted) { uint8_t reply[]={0,3,1};bringup_usb_packet(LX_MSG_SETUP_REPLY,p->sequence,reply,3); }
}
static bool nonzero(const uint8_t *p,size_t n) { uint8_t v=0;while(n--) v|=*p++;return v!=0; }
static esp_err_t command(const uint8_t *p,size_t n,uint8_t *out,size_t *length)
{
    uint8_t op=p[0];*length=0;
    if(op==1 && n==1) {
        out[0]=1;out[1]=s_receiver;out[2]=s_record.current.flags;
        out[3]=network_profile_present();esp_read_mac(out+4,ESP_MAC_WIFI_STA);
        network_status(out+10);memcpy(out+18,s_record.current.peer,6);
        memcpy(out+24,s_record.current.pc_id,16);out[40]=s_record.pending;*length=41;return ESP_OK;
    }
    if(op==2 && n==17 && nonzero(p+1,16)) {
        if(s_session) return !memcmp(s_token,p+1,16)?ESP_OK:ESP_ERR_INVALID_STATE;
        s_stage=s_record.current;s_session=true;s_tested=false;s_changed_wifi=false;s_committed=false;s_wifi_staged=false;
        /* Migrate the board's own saved network without exporting credentials
         * to the host. It remains staged until the normal trial/commit path. */
        if(!(s_stage.flags&1)) {
            nvs_handle_t h;
            if(nvs_open("lx_net",NVS_READONLY,&h)==ESP_OK) {
                size_t length=sizeof(s_stage.ssid);
                esp_err_t ssid=nvs_get_str(h,"ssid",s_stage.ssid,&length);length=sizeof(s_stage.password);
                esp_err_t password=nvs_get_str(h,"pass",s_stage.password,&length);nvs_close(h);
                if(ssid==ESP_OK && s_stage.ssid[0] && (password==ESP_OK || password==ESP_ERR_NVS_NOT_FOUND)) s_stage.flags|=1;
                else { memset(s_stage.ssid,0,sizeof(s_stage.ssid));memset(s_stage.password,0,sizeof(s_stage.password)); }
            }
        }
        memcpy(s_token,p+1,16);s_deadline=esp_timer_get_time()+120000000;return ESP_OK;
    }
    if(!s_session || n<17 || memcmp(p+1,s_token,16)) return ESP_ERR_INVALID_STATE;
    s_deadline=esp_timer_get_time()+120000000;p+=17;n-=17;
    if(op==9 && !n) return rollback();
    if(op==7 && !n) { network_status(out);out[8]=s_tested;out[9]=s_committed;*length=10;return ESP_OK; }
    if(s_committed && op!=10) return ESP_ERR_INVALID_STATE;
    if(op==3 && n>=2 && p[0]>0 && p[0]<=32 && (p[1]==0 || (p[1]>=8 && p[1]<=63)) && n==(size_t)(2+p[0]+p[1]) && !memchr(p+2,0,n-2)) {
        memset(s_stage.ssid,0,33);memset(s_stage.password,0,64);
        memcpy(s_stage.ssid,p+2,p[0]);memcpy(s_stage.password,p+2+p[0],p[1]);s_stage.flags|=1;s_tested=false;s_wifi_staged=true;return ESP_OK;
    }
    if(op==4 && n==22 && !(p[0]&1) && nonzero(p,6) && nonzero(p+6,16)) {
        uint8_t own[6];esp_read_mac(own,ESP_MAC_WIFI_STA);if(!memcmp(own,p,6)) return ESP_ERR_INVALID_ARG;
        memcpy(s_stage.peer,p,6);memcpy(s_stage.pair_key,p+6,16);s_stage.flags|=2;return ESP_OK;
    }
    if(op==5) return ESP_ERR_NOT_SUPPORTED; /* No PC pairing in this edition. */
    if(op==6 && n==1 && p[0]==1 && !s_wifi_staged) { s_tested=true;return ESP_OK; }
    if(op==6 && !n) {
        if(!(s_stage.flags&1)) { s_tested=network_ready();return s_tested?ESP_OK:ESP_ERR_INVALID_STATE; }
        s_changed_wifi=true;s_tested=false;
        esp_err_t rc=network_trial(s_stage.ssid,s_stage.password);
        if(rc==ESP_OK) s_tested=true;
        else { network_trial_end();s_changed_wifi=false; }
        return rc;
    }
    if(op==8 && !n && s_tested && (!s_changed_wifi || network_ready())) {
        record_t next={.version=1,.pending=1,.current=s_stage,.previous=s_record.current};
        esp_err_t rc=save_record(&next);if(rc==ESP_OK) { s_record=next;s_committed=true; }return rc;
    }
    if(op==10 && !n && s_committed) {
        record_t next=s_record;next.pending=0;memset(&next.previous,0,sizeof(next.previous));
        esp_err_t rc=save_record(&next);
        if(rc==ESP_OK) { s_record=next;s_session=false;s_committed=false;s_changed_wifi=false;free(s_aps);s_aps=NULL;s_ap_count=0;memset(&s_stage,0,sizeof(s_stage));memset(s_token,0,16); }
        return rc;
    }
    if(op==11 && !n) {
        if(!s_aps) s_aps=heap_caps_calloc(12,sizeof(*s_aps),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!s_aps) return ESP_ERR_NO_MEM;
        wifi_scan_config_t config={0};s_ap_count=0;
        esp_err_t rc=esp_wifi_scan_start(&config,true);if(rc!=ESP_OK) return rc;
        s_ap_count=12;rc=esp_wifi_scan_get_ap_records(&s_ap_count,s_aps);
        out[0]=s_ap_count;*length=1;return rc;
    }
    if(op==12 && n==1 && p[0]<s_ap_count) {
        wifi_ap_record_t *ap=&s_aps[p[0]];out[0]=ap->primary;out[1]=(uint8_t)ap->rssi;out[2]=ap->authmode;
        size_t len=strnlen((char*)ap->ssid,32);memcpy(out+3,ap->ssid,len);*length=3+len;return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}
void device_setup_poll(void)
{
    if(s_session && esp_timer_get_time()>s_deadline && rollback()!=ESP_OK) return;
    uint8_t request[LX_PAYLOAD_MAX],reply[LX_PAYLOAD_MAX];size_t n=0;uint32_t sequence=0;
    portENTER_CRITICAL(&s_lock);
    if(s_queued) { n=s_length;sequence=s_sequence;memcpy(request,s_request,n);memset(s_request,0,sizeof(s_request));s_queued=false; }
    portEXIT_CRITICAL(&s_lock);
    if(n) {
        size_t length=0;esp_err_t rc=command(request,n,reply+3,&length);
        reply[0]=request[0];reply[1]=rc;reply[2]=rc>>8;
        bringup_usb_packet(LX_MSG_SETUP_REPLY,sequence,reply,length+3);memset(request,0,sizeof(request));
    }
    if(s_session && esp_timer_get_time()>s_deadline) rollback();
}
