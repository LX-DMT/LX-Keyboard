#include <stdio.h>
#include <string.h>
#include "ble_keys.h"
#include "bringup_usb.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "mouse_queue.h"
#include "consumer_queue.h"
#include "link_control.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* IDF's store initializer is exported but absent from its public header. */
void ble_store_config_init(void);

typedef struct { lx_key_report_t report; uint32_t sequence; int64_t event_us; } item_t;
typedef struct { uint32_t sequence; int64_t event_us,sent_us,accepted_us; } trace_t;
static trace_t *s_trace;
static unsigned s_trace_count;
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static item_t s_queue[32],s_latest;
static int64_t s_key_delivered_us;
static unsigned s_head,s_count;
static TaskHandle_t s_task;
static uint16_t s_conn=BLE_HS_CONN_HANDLE_NONE,s_key_handle,s_boot_handle,s_mouse_handle,s_consumer_handle;
static bool s_mouse_sub,s_consumer_sub;
static uint16_t s_consumer_current;
static lx_mouse_queue_t s_mouse;
static lx_consumer_queue_t s_consumer;
void ble_consumer_submit(uint16_t usage)
{
    portENTER_CRITICAL(&s_lock);lx_consumer_push(&s_consumer,usage);portEXIT_CRITICAL(&s_lock);
    if(s_task) xTaskNotifyGive(s_task);
}
static lx_mouse_report_t s_mouse_current;
static uint16_t s_interval;
static uint16_t s_latency;
static bool s_encrypted,s_key_sub,s_boot_sub,s_resync,s_suspended;
static bool s_cancel_pending;
static volatile bool s_stopping;
static volatile bool s_host_synced;
static int s_sleep=-1;
static uint8_t s_protocol=1,s_leds;
static uint32_t s_sent,s_full,s_errors;
static esp_power_level_t s_power=ESP_PWR_LVL_P20;
static unsigned s_strong_samples;
static esp_err_t s_power_result=ESP_OK;
static esp_err_t s_adv_power_result=ESP_OK;
static lx_key_report_t s_current;
static const uint8_t s_info[]={0x11,0x01,0,3};
static const uint8_t s_pnp[]={2,0x3a,0x30,0x01,0x40,0x00,0x01};
static const uint8_t s_key_ref[]={1,1},s_led_ref[]={1,2},s_mouse_ref[]={2,1},s_consumer_ref[]={3,1};
enum { INFO=1,MAP,KEYS,LEDS,PROTOCOL,CONTROL,MOUSE,CONSUMER,PNP };

static int access_value(uint16_t conn,uint16_t attr,struct ble_gatt_access_ctxt *ctxt,void *arg)
{
    (void)conn;(void)attr;
    uintptr_t kind=(uintptr_t)arg;
    if(ctxt->op==BLE_GATT_ACCESS_OP_READ_DSC)
        return os_mbuf_append(ctxt->om,arg,2)==0?0:BLE_ATT_ERR_INSUFFICIENT_RES;
    if(ctxt->op==BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t value;
        if(OS_MBUF_PKTLEN(ctxt->om)!=1 || ble_hs_mbuf_to_flat(ctxt->om,&value,1,NULL)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if((kind==PROTOCOL || kind==CONTROL) && value>1) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        portENTER_CRITICAL(&s_lock);
        if(kind==LEDS) s_leds=value;
        else if(kind==PROTOCOL) { s_protocol=value;s_resync=true; }
        else if(kind==CONTROL) { s_suspended=value==0;s_resync=true; }
        portEXIT_CRITICAL(&s_lock);
        return 0;
    }
    const void *data=NULL;uint16_t length=0;
    lx_key_report_t current;uint16_t consumer;
    portENTER_CRITICAL(&s_lock);current=s_current;consumer=s_consumer_current;portEXIT_CRITICAL(&s_lock);
    switch(kind) {
        case INFO:data=s_info;length=sizeof(s_info);break;
        case MAP:data=bringup_hid_descriptor(&length);break;
        case KEYS:data=&current;length=8;break;
        case LEDS:data=&s_leds;length=1;break;
        case PROTOCOL:data=&s_protocol;length=1;break;
        case PNP:data=s_pnp;length=sizeof(s_pnp);break;
        case MOUSE:data=&s_mouse_current;length=5;break;
        case CONSUMER:data=&consumer;length=2;break;
        default:return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om,data,length)==0?0:BLE_ATT_ERR_INSUFFICIENT_RES;
}
#define REF(value) (struct ble_gatt_dsc_def[]){{.uuid=BLE_UUID16_DECLARE(0x2908),.att_flags=BLE_ATT_F_READ,.access_cb=access_value,.arg=(void *)(value)},{0}}
#define READ BLE_GATT_CHR_F_READ
#define IN (READ|BLE_GATT_CHR_F_NOTIFY|BLE_GATT_CHR_F_READ_ENC)
#define OUT (READ|BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_NO_RSP|BLE_GATT_CHR_F_WRITE_ENC)
static const ble_uuid128_t control_service=BLE_UUID128_INIT(0x01,0x00,0xa1,0x2a,0x8b,0x7c,0x12,0xa6,0x45,0x4e,0x63,0x48,0x01,0xc0,0xb8,0x7d);
static const ble_uuid128_t control_characteristic=BLE_UUID128_INIT(0x01,0x00,0xa1,0x2a,0x8b,0x7c,0x12,0xa6,0x45,0x4e,0x63,0x48,0x02,0xc0,0xb8,0x7d);
static int access_control(uint16_t conn,uint16_t attr,struct ble_gatt_access_ctxt *ctxt,void *arg)
{
    (void)conn;(void)attr;(void)arg;
    uint8_t reply[10];
    if(ctxt->op==BLE_GATT_ACCESS_OP_READ_CHR) {
        link_control_apply(NULL,0,reply);
        return os_mbuf_append(ctxt->om,reply,sizeof(reply))?BLE_ATT_ERR_INSUFFICIENT_RES:0;
    }
    if(ctxt->op!=BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t data[16];uint16_t length=OS_MBUF_PKTLEN(ctxt->om);
    if(!length || length>sizeof(data) || ble_hs_mbuf_to_flat(ctxt->om,data,sizeof(data),NULL)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    return link_control_apply(data,length,reply)==ESP_OK?0:BLE_ATT_ERR_UNLIKELY;
}
static const struct ble_gatt_svc_def services[]={
    {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=BLE_UUID16_DECLARE(0x1812),.characteristics=(struct ble_gatt_chr_def[]){
        {.uuid=BLE_UUID16_DECLARE(0x2a4a),.access_cb=access_value,.arg=(void*)INFO,.flags=READ},
        {.uuid=BLE_UUID16_DECLARE(0x2a4b),.access_cb=access_value,.arg=(void*)MAP,.flags=READ},
        {.uuid=BLE_UUID16_DECLARE(0x2a4c),.access_cb=access_value,.arg=(void*)CONTROL,.flags=BLE_GATT_CHR_F_WRITE_NO_RSP},
        {.uuid=BLE_UUID16_DECLARE(0x2a4e),.access_cb=access_value,.arg=(void*)PROTOCOL,.flags=READ|BLE_GATT_CHR_F_WRITE_NO_RSP},
        {.uuid=BLE_UUID16_DECLARE(0x2a4d),.access_cb=access_value,.arg=(void*)KEYS,.flags=IN,.val_handle=&s_key_handle,.descriptors=REF(s_key_ref)},
        {.uuid=BLE_UUID16_DECLARE(0x2a4d),.access_cb=access_value,.arg=(void*)LEDS,.flags=OUT,.descriptors=REF(s_led_ref)},
        {.uuid=BLE_UUID16_DECLARE(0x2a4d),.access_cb=access_value,.arg=(void*)MOUSE,.flags=IN,.val_handle=&s_mouse_handle,.descriptors=REF(s_mouse_ref)},
        {.uuid=BLE_UUID16_DECLARE(0x2a4d),.access_cb=access_value,.arg=(void*)CONSUMER,.flags=IN,.val_handle=&s_consumer_handle,.descriptors=REF(s_consumer_ref)},
        {.uuid=BLE_UUID16_DECLARE(0x2a22),.access_cb=access_value,.arg=(void*)KEYS,.flags=IN,.val_handle=&s_boot_handle},
        {.uuid=BLE_UUID16_DECLARE(0x2a32),.access_cb=access_value,.arg=(void*)LEDS,.flags=OUT},{0}}},
    {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=BLE_UUID16_DECLARE(0x180a),.characteristics=(struct ble_gatt_chr_def[]){
        {.uuid=BLE_UUID16_DECLARE(0x2a50),.access_cb=access_value,.arg=(void*)PNP,.flags=READ},{0}}},
    {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&control_service.u,.characteristics=(struct ble_gatt_chr_def[]){
        {.uuid=&control_characteristic.u,.access_cb=access_control,.flags=READ|BLE_GATT_CHR_F_READ_ENC|BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_ENC},{0}}},{0}
};
static void advertise(void);
static int gap_event(struct ble_gap_event *event,void *arg)
{
    (void)arg;
    switch(event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if(event->connect.status) { advertise();break; }
        portENTER_CRITICAL(&s_lock);
        s_conn=event->connect.conn_handle;s_protocol=1;s_encrypted=false;s_resync=true;s_key_sub=false;s_boot_sub=false;s_mouse_sub=false;s_consumer_sub=false;s_suspended=false;
        s_strong_samples=0;
        portEXIT_CRITICAL(&s_lock);
        /* Do not inherit a previous near-field connection's reduced power. */
        ble_keys_power(true);
        { struct ble_gap_upd_params params={.itvl_min=6,.itvl_max=6,.latency=0,.supervision_timeout=400};
          int rc=ble_gap_update_params(s_conn,&params);
          printf("LX_BLE interval_request_us=7500 result=%d\n",rc); }
        ble_gap_security_initiate(s_conn);
        { int rc=ble_gap_set_prefered_le_phy(s_conn,BLE_GAP_LE_PHY_1M_MASK,BLE_GAP_LE_PHY_1M_MASK,0);
          printf("LX_BLE phy_request=1M result=%d\n",rc); }
        printf("LX_BLE connected handle=%u\n",s_conn);break;
    case BLE_GAP_EVENT_DISCONNECT:
        portENTER_CRITICAL(&s_lock);
        s_conn=BLE_HS_CONN_HANDLE_NONE;s_encrypted=false;s_key_sub=false;s_boot_sub=false;s_mouse_sub=false;s_consumer_sub=false;
        s_strong_samples=0;
        portEXIT_CRITICAL(&s_lock);
        ble_keys_power(true);
        printf("LX_BLE disconnected reason=%d\n",event->disconnect.reason);advertise();break;
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        printf("LX_BLE phy_update=%d tx_phy=%u rx_phy=%u\n",event->phy_updated.status,event->phy_updated.tx_phy,event->phy_updated.rx_phy);break;
    case BLE_GAP_EVENT_ENC_CHANGE:
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        if(!ble_gap_conn_find(s_conn,&desc)) {
            portENTER_CRITICAL(&s_lock);s_encrypted=desc.sec_state.encrypted;s_interval=desc.conn_itvl;s_latency=desc.conn_latency;portEXIT_CRITICAL(&s_lock);
            printf("LX_BLE encrypted=%u interval_us=%u latency=%u\n",s_encrypted,s_interval*1250,s_latency);
        }break; }
    case BLE_GAP_EVENT_SUBSCRIBE:
        portENTER_CRITICAL(&s_lock);
        if(event->subscribe.attr_handle==s_key_handle) s_key_sub=event->subscribe.cur_notify;
        if(event->subscribe.attr_handle==s_boot_handle) s_boot_sub=event->subscribe.cur_notify;
        if(event->subscribe.attr_handle==s_mouse_handle) s_mouse_sub=event->subscribe.cur_notify;
        if(event->subscribe.attr_handle==s_consumer_handle) s_consumer_sub=event->subscribe.cur_notify;
        s_resync=true;
        portEXIT_CRITICAL(&s_lock);break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if(!ble_gap_conn_find(s_conn,&desc)) ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY; }
    default:break;
    }
    if(s_task) xTaskNotifyGive(s_task);
    return 0;
}
static void advertise(void)
{
    if(s_stopping) return;
    static ble_uuid16_t hid=BLE_UUID16_INIT(0x1812);
    struct ble_hs_adv_fields fields={.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP,
        .name=(uint8_t*)"LX Keyboard Dev",.name_len=15,.name_is_complete=1,
        .appearance=0x3c1,.appearance_is_present=1,.uuids16=&hid,.num_uuids16=1,.uuids16_is_complete=1};
    struct ble_gap_adv_params params={.conn_mode=BLE_GAP_CONN_MODE_UND,.disc_mode=BLE_GAP_DISC_MODE_GEN,
                                      .itvl_min=48,.itvl_max=80};
    int rc=ble_gap_adv_set_fields(&fields);
    if(!rc) rc=ble_gap_adv_start(BLE_OWN_ADDR_RANDOM,NULL,BLE_HS_FOREVER,&params,gap_event,NULL);
    if(!rc) {
        /* Apply to the concrete advertising handle after host reset and
         * parameter creation, not just the pre-host default power setting. */
        int before=esp_ble_tx_power_get_enhanced(ESP_BLE_ENHANCED_PWR_TYPE_ADV,0);
        s_adv_power_result=esp_ble_tx_power_set_enhanced(ESP_BLE_ENHANCED_PWR_TYPE_ADV,0,ESP_PWR_LVL_P20);
        printf("LX_BLE adv_power_result=%s before_level=%d level=%d\n",esp_err_to_name(s_adv_power_result),before,
            (int)esp_ble_tx_power_get_enhanced(ESP_BLE_ENHANCED_PWR_TYPE_ADV,0));
    }
    printf("LX_BLE advertising=%d\n",rc);
}
static void sync_host(void)
{
    s_host_synced=true;
    ble_keys_power(true);
    /* HID needs little bandwidth. Prefer 1M sensitivity over 2M throughput;
     * the central still decides the negotiated PHY. No coded-PHY requirement. */
    int phy_rc=ble_gap_set_prefered_default_le_phy(BLE_GAP_LE_PHY_1M_MASK,BLE_GAP_LE_PHY_1M_MASK);
    printf("LX_BLE phy_default=1M result=%d\n",phy_rc);
    uint8_t mac[6],address[6];esp_read_mac(mac,ESP_MAC_BT);
    for(unsigned i=0;i<6;i++) address[i]=mac[5-i];
    address[5]|=0xc0; /* Stable development identity, distinct from old firmware. */
    int rc=ble_hs_id_set_rnd(address);
    printf("LX_BLE identity=%02x:%02x:%02x:%02x:%02x:%02x result=%d\n",
        address[5],address[4],address[3],address[2],address[1],address[0],rc);
    if(!rc) advertise();
}
static void host_task(void *arg) { (void)arg;nimble_port_run();nimble_port_freertos_deinit(); }
static bool ready(void)
{
    portENTER_CRITICAL(&s_lock);
    bool ready=s_conn!=BLE_HS_CONN_HANDLE_NONE && s_encrypted && !s_suspended && (s_protocol?s_key_sub:s_boot_sub);
    portEXIT_CRITICAL(&s_lock);return ready;
}
static bool send_report(const lx_key_report_t *report)
{
    struct os_mbuf *packet=ble_hs_mbuf_from_flat(report,8);
    if(!packet) return false;
    portENTER_CRITICAL(&s_lock);uint16_t conn=s_conn,handle=s_protocol?s_key_handle:s_boot_handle;portEXIT_CRITICAL(&s_lock);
    int rc=ble_gatts_notify_custom(conn,handle,packet);
    portENTER_CRITICAL(&s_lock);
    if(rc) s_errors++;
    else { s_current=*report;s_sent++; }
    portEXIT_CRITICAL(&s_lock);return rc==0;
}
static void output_task(void *arg)
{
    (void)arg;bool connected=false,have=false,release_first=false;
    item_t item={0},snapshot={0};
    lx_mouse_report_t mouse={0};bool mouse_have=false;
    uint16_t consumer=0;bool consumer_have=false;
    int64_t mouse_next=0;
    while(1) {
        if(!ready()) {
            connected=false;have=false;mouse_have=false;consumer_have=false;
            portENTER_CRITICAL(&s_lock);s_count=0;s_head=0;s_mouse.count=0;s_consumer.count=0;portEXIT_CRITICAL(&s_lock);
        } else {
            portENTER_CRITICAL(&s_lock);
            bool resync=s_resync;s_resync=false;
            if(s_cancel_pending) { have=false;release_first=false;s_cancel_pending=false; }
            if(!connected || resync) { s_count=0;s_head=0;snapshot=s_latest;lx_mouse_queue_resync(&s_mouse);mouse_have=false;lx_consumer_resync(&s_consumer);consumer_have=false; }
            portEXIT_CRITICAL(&s_lock);
            if(!connected || resync) { connected=true;release_first=true;have=false; }
            if(release_first) {
                lx_key_report_t release={0};
                if(send_report(&release)) { release_first=false;item=snapshot;have=true; }
            } else {
                if(!have) { portENTER_CRITICAL(&s_lock);
                    if(s_count) { item=s_queue[s_head];s_head=(s_head+1)%32;s_count--;have=true; }
                    portEXIT_CRITICAL(&s_lock); }
                if(have) {
                    int64_t sent=esp_timer_get_time();
                    if(send_report(&item.report)) {
                        portENTER_CRITICAL(&s_lock);
                        s_key_delivered_us=item.event_us;
                        if(s_trace && s_trace_count<1200) s_trace[s_trace_count++]=(trace_t){item.sequence,item.event_us,sent,esp_timer_get_time()};
                        portEXIT_CRITICAL(&s_lock);have=false;
                    }
                } else {
                    portENTER_CRITICAL(&s_lock);
                    bool consumer_ready=s_consumer_sub && s_protocol;
                    if(consumer_ready && !consumer_have) consumer_have=lx_consumer_take(&s_consumer,&consumer);
                    bool mouse_ready=s_mouse_sub && s_protocol && esp_timer_get_time()>=mouse_next;
                    if(mouse_ready && !mouse_have) mouse_have=lx_mouse_queue_take(&s_mouse,&mouse);
                    uint16_t conn=s_conn;
                    portEXIT_CRITICAL(&s_lock);
                    if(consumer_ready && consumer_have) {
                        struct os_mbuf *packet=ble_hs_mbuf_from_flat(&consumer,2);
                        if(packet && !ble_gatts_notify_custom(conn,s_consumer_handle,packet)) {
                            portENTER_CRITICAL(&s_lock);s_consumer_current=consumer;portEXIT_CRITICAL(&s_lock);
                            consumer_have=false;
                        }
                    } else if(mouse_ready && mouse_have) {
                        struct os_mbuf *packet=ble_hs_mbuf_from_flat(&mouse,sizeof(mouse));
                        if(packet && !ble_gatts_notify_custom(conn,s_mouse_handle,packet)) {
                            portENTER_CRITICAL(&s_lock);s_mouse_current=mouse;portEXIT_CRITICAL(&s_lock);
                            /* Queue before the next 7.5ms BLE event; an 8ms gate
                             * repeatedly misses it. Keyboard still wins above. */
                            mouse_have=false;mouse_next=esp_timer_get_time()+4000;
                        }
                    }
                }
            }
        }
        ulTaskNotifyTake(pdTRUE,1);
    }
}
void ble_mouse_submit(const lx_mouse_sample_t *sample)
{
    portENTER_CRITICAL(&s_lock);lx_mouse_queue_push(&s_mouse,sample);portEXIT_CRITICAL(&s_lock);
    if(s_task) xTaskNotifyGive(s_task);
}
void ble_keys_submit(const lx_key_report_t *report,uint32_t sequence,int64_t event_us)
{
    item_t item={.report=*report,.sequence=sequence,.event_us=event_us};
    portENTER_CRITICAL(&s_lock);s_latest=item;
    if(s_count<32) { s_queue[(s_head+s_count)%32]=item;s_count++; }
    else { s_full++;s_resync=true; }
    portEXIT_CRITICAL(&s_lock);if(s_task) xTaskNotifyGive(s_task);
}
bool ble_keys_text_room(bool *pending)
{
    portENTER_CRITICAL(&s_lock);bool room=s_count<2;
    *pending=s_count || s_latest.event_us>s_key_delivered_us;
    portEXIT_CRITICAL(&s_lock);return room;
}
void ble_keys_cancel_pending(void)
{
    item_t release={.event_us=esp_timer_get_time()};
    portENTER_CRITICAL(&s_lock);
    s_head=0;s_count=1;s_queue[0]=s_latest=release;s_resync=false;s_cancel_pending=true;
    portEXIT_CRITICAL(&s_lock);
    if(s_task) xTaskNotifyGive(s_task);
}
esp_err_t ble_keys_start(void)
{
    ESP_ERROR_CHECK(nimble_port_init());
    ESP_ERROR_CHECK(esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT,s_power));
    ESP_ERROR_CHECK(esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,s_power));
    esp_log_level_set("NimBLE",ESP_LOG_WARN);
    ble_svc_gap_init();ble_svc_gatt_init();
    ble_svc_gap_device_name_set("LX Keyboard Dev");ble_svc_gap_device_appearance_set(0x3c1);
    ble_hs_cfg.sync_cb=sync_host;ble_hs_cfg.sm_io_cap=BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding=1;ble_hs_cfg.sm_sc=1;
    ble_hs_cfg.sm_our_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb=ble_store_util_status_rr;ble_store_config_init();
    if(ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services)) return ESP_FAIL;
    if(xTaskCreatePinnedToCore(output_task,"ble_keys",4096,NULL,11,&s_task,1)!=pdPASS) return ESP_ERR_NO_MEM;
    nimble_port_freertos_init(host_task);return ESP_OK;
}
void ble_keys_stats(void)
{
    int8_t rssi=127;uint8_t tx_phy=0,rx_phy=0;
    int rssi_rc=BLE_HS_ENOTCONN,phy_rc=BLE_HS_ENOTCONN;
    esp_power_level_t conn_power=ESP_PWR_LVL_INVALID;
    if(s_conn!=BLE_HS_CONN_HANDLE_NONE) {
        rssi_rc=ble_gap_conn_rssi(s_conn,&rssi);
        phy_rc=ble_gap_read_le_phy(s_conn,&tx_phy,&rx_phy);
        conn_power=esp_ble_tx_power_get_enhanced(ESP_BLE_ENHANCED_PWR_TYPE_CONN,s_conn);
    }
    printf("LX_BLE ready=%u connection=%u encrypted=%u interval_us=%u latency=%u sent=%lu queue_full=%lu errors=%lu leds=%u rssi=%d\n",
        ready(),s_conn,s_encrypted,s_interval*1250,s_latency,(unsigned long)s_sent,(unsigned long)s_full,(unsigned long)s_errors,s_leds,rssi);
    printf("LX_BLE_POWER dbm=%u modem_sleep=%d policy=range_guard result=%s conn_level=%d adv_level=%d strong_samples=%u adv_result=%s\n",
        s_power==ESP_PWR_LVL_P20?20:9,s_sleep,esp_err_to_name(s_power_result),(int)conn_power,
        (int)esp_ble_tx_power_get_enhanced(ESP_BLE_ENHANCED_PWR_TYPE_ADV,0),s_strong_samples,esp_err_to_name(s_adv_power_result));
    printf("LX_BLE_LINK rssi_valid=%u tx_phy=%u rx_phy=%u phy_result=%d\n",rssi_rc==0 && rssi>=-127 && rssi<=20,tx_phy,rx_phy,phy_rc);
}
void ble_keys_power(bool high)
{
    esp_power_level_t power=high?ESP_PWR_LVL_P20:ESP_PWR_LVL_P9;
    esp_err_t result=esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT,power);
    if(result==ESP_OK && s_conn!=BLE_HS_CONN_HANDLE_NONE)
        result=esp_ble_tx_power_set_enhanced(ESP_BLE_ENHANCED_PWR_TYPE_CONN,s_conn,power);
    if(result==ESP_OK) s_power=power;
    s_power_result=result;
    printf("LX_BLE power_dbm=%u result=%s\n",high?20:9,esp_err_to_name(result));
}
void ble_keys_shutdown(void)
{
    s_stopping=true;ble_gap_adv_stop();
    if(s_conn!=BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn,BLE_ERR_REM_USER_CONN_TERM);
}
static bool range_high(bool high,bool valid,int rssi,unsigned *strong)
{
    /* HCI RSSI 127 means unavailable, not an exceptionally strong signal.
     * Four consecutive 5-second polls are needed before saving power. */
    if(!valid || rssi < -127 || rssi > 20 || rssi<=-65) { *strong=0;return true; }
    if(!high) { *strong=0;return false; }
    if(rssi<-55) { *strong=0;return true; }
    if(*strong<4) ++*strong;
    return *strong<4;
}
unsigned ble_keys_policy_selftest(void)
{
    unsigned failures=0,n=0;
    for(unsigned i=0;i<3;i++) failures+=!range_high(true,true,-40,&n);
    failures+=range_high(true,true,-40,&n);
    failures+=!range_high(false,true,-65,&n);
    failures+=!range_high(false,true,127,&n);
    failures+=!range_high(false,false,0,&n);
    failures+=!range_high(true,true,-55,&n);
    failures+=!range_high(true,true,-56,&n);failures+=n!=0;
    failures+=range_high(false,true,-64,&n);
    failures+=!range_high(true,true,-100,&n);
    return failures;
}
void ble_keys_power_tick(void)
{
    /* controls runs before nimble_port_init; controller power reads are unsafe
     * until the host has completed its initial HCI reset/synchronization. */
    if(s_stopping || !s_host_synced) return;
    uint16_t conn=s_conn;int8_t rssi=127;
    bool valid=conn!=BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_rssi(conn,&rssi)==0;
    portENTER_CRITICAL(&s_lock);
    bool high=range_high(s_power==ESP_PWR_LVL_P20,valid && conn==s_conn,rssi,&s_strong_samples);
    portEXIT_CRITICAL(&s_lock);
    if(high!=(s_power==ESP_PWR_LVL_P20) || s_power_result!=ESP_OK) ble_keys_power(high);
}
void ble_keys_sleep(bool enabled)
{
    if(s_stopping || s_sleep==(int)enabled) return;
    esp_err_t rc=enabled?esp_bt_sleep_enable():esp_bt_sleep_disable();
    if(rc==ESP_OK) { s_sleep=enabled;printf("LX_BLE modem_sleep=%u\n",enabled); }
}
void ble_keys_trace_clear(void)
{
    if(!s_trace) { trace_t *trace=heap_caps_calloc(1200,sizeof(trace_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        portENTER_CRITICAL(&s_lock);s_trace=trace;portEXIT_CRITICAL(&s_lock); }
    portENTER_CRITICAL(&s_lock);s_trace_count=0;portEXIT_CRITICAL(&s_lock);
}
void ble_keys_trace_print(void)
{
    portENTER_CRITICAL(&s_lock);unsigned count=s_trace_count;portEXIT_CRITICAL(&s_lock);
    for(unsigned i=0;i<count;i++) {
        portENTER_CRITICAL(&s_lock);trace_t t=s_trace[i];portEXIT_CRITICAL(&s_lock);
        /* accepted_us is NimBLE submission, not over-the-air completion. */
        printf("LX_TRACE %lu %lld %lld %lld\n",(unsigned long)t.sequence,(long long)t.event_us,(long long)t.sent_us,(long long)t.accepted_us);
    }
    printf("LX_TRACE_END %u\n",count);
}
