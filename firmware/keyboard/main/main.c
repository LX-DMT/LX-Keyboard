#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "board.h"
#include "battery_led.h"
#include "activation_ui.h"
#include "power_policy.h"
#include "bringup_usb.h"
#include "usb_keys.h"
#include "input_core.h"
#include "custom_keys.h"
#include "lighting_settings.h"
#include "unicode_keys.h"
#include "text_keys.h"
#include "voice_focus.h"
#include "voice_capture.h"
#include "voice_duplex.h"
#include "chat_official.h"
#include "asr_baidu.h"
#include "chat_client.h"
#include "chat_protocol.h"
#include "voice_opus.h"
#include "radio_keys.h"
#include "lx_protocol.h"
#include "ble_keys.h"
#include "hid_mouse.h"
#include "mouse_queue.h"
#include "consumer_queue.h"
#include "knob_link.h"
#include "user_controls.h"
#include "network_link.h"
#include "device_setup.h"
#include "link_control.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"

static bool s_host;
static uint8_t s_mode;
static volatile bool s_switching;
static lx_key_report_t s_test_report;
static bool s_test_physical;
static int64_t s_test_until;
static int64_t s_mouse_test_until;
static int64_t s_consumer_test_until;
static uint16_t s_consumer_test;
static void consumer_input(uint16_t usage);
static void mouse_input(const lx_mouse_sample_t *sample);
static uint32_t s_test_sequence;
static TaskHandle_t s_load_task;
static QueueHandle_t s_modes;
static esp_err_t request_mode(uint8_t mode)
{
    if(mode>3 && mode!=254 && mode!=255) return ESP_ERR_NOT_SUPPORTED;
    if(power_policy_shutting_down()) return ESP_ERR_INVALID_STATE;
    if(mode<=3) power_policy_activity();
    return s_modes && xQueueOverwrite(s_modes,&mode)==pdTRUE?ESP_OK:ESP_ERR_INVALID_STATE;
}
static esp_err_t request_shutdown(void) { return request_mode(254); }
static esp_err_t knob_command(uint8_t type,const uint8_t *p,size_t n)
{
    if(type==0x10 && n==3 && p[0]<=3) return request_mode(p[0]);
    return user_controls_command(type,p,n);
}
static volatile bool s_load_stop;
static uint32_t s_unicode_request;
static bool s_unicode_cancel;
static lx_text_t s_text;
static lx_text_encoding_t s_text_encoding=LX_TEXT_GBK;
static bool s_physical;
static bool s_pinyin_pending;
static bool s_voice_text;
static unsigned s_text_interval_us=2000;
/* Fast complete key edges within a code; allow the native IME to commit each
 * character before the next VUC. Unbounded bursts delayed Windows rendering. */
static unsigned s_text_gap_us=60000;
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static bool text_request_impl(const char *utf8,bool voice)
{
    portENTER_CRITICAL(&s_lock);lx_text_encoding_t encoding=s_text_encoding;portEXIT_CRITICAL(&s_lock);
    /* All four modes deliver HID through the configured input method. */
    lx_text_t text={0};if(!lx_text_begin_encoded(&text,utf8,encoding)) return false;
    portENTER_CRITICAL(&s_lock);
    bool accept=(!voice || voice_audio_ready()) && !s_switching && !s_physical && !s_unicode_cancel && !lx_text_busy(&s_text) && s_text_encoding==encoding;
    if(accept) {
        s_text=text;
        if(accept) s_voice_text=voice;
    }
    portEXIT_CRITICAL(&s_lock);
    return accept;
}
static bool text_request(const char *utf8) { return text_request_impl(utf8,false); }
static bool voice_text_request(const char *utf8) { return text_request_impl(utf8,true); }
static void voice_text_cancel(void)
{
    portENTER_CRITICAL(&s_lock);
    if(s_voice_text) { s_unicode_cancel=true;s_unicode_request=0; }
    portEXIT_CRITICAL(&s_lock);
}
static esp_err_t text_encoding_setting(uint8_t encoding)
{
    if(encoding>LX_TEXT_PINYIN) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;esp_err_t result=nvs_open("lx_dev",NVS_READWRITE,&h);
    if(result!=ESP_OK) return result;
    result=nvs_set_u8(h,"text_encoding",encoding);
    if(result==ESP_OK) result=nvs_commit(h);
    nvs_close(h);
    if(result==ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_text_encoding=encoding;s_unicode_cancel=true;s_unicode_request=0;
        /* The scan task clears any uncommitted IME composition before cancel. */
        portEXIT_CRITICAL(&s_lock);
    }
    return result;
}
static bool text_busy(void)
{
    portENTER_CRITICAL(&s_lock);bool busy=lx_text_busy(&s_text);portEXIT_CRITICAL(&s_lock);return busy;
}
static esp_err_t voice_setting(uint8_t op,const uint8_t *data,size_t n)
{
    if(op==12) return n==1?text_encoding_setting(data[0]):ESP_ERR_INVALID_ARG;
    return op<=9?asr_baidu_setting(op,data,n):chat_setting(op,data,n);
}
static struct {
    uint32_t samples,errors,changed,max_read_us,max_gap_us,over_2ms;
    uint32_t gap_buckets[41];
    uint8_t latest[BOARD_SCAN_BYTES];
} s_stats;

static void keyboard_submit(const lx_key_report_t *report,uint32_t sequence,int64_t event_us)
{
    if(s_mode==1) ble_keys_submit(report,sequence,event_us);
    else if(s_mode==2) wifi_keys_submit(report,sequence,event_us);
    else if(s_mode==3) radio_keys_submit(report,sequence,event_us);
    else usb_keys_submit(report,sequence,event_us);
}
static void ime_cancel(int64_t event_us)
{
    lx_key_report_t escape={.keys={0x29}},release={0};
    keyboard_submit(&escape,0,event_us);keyboard_submit(&release,0,event_us);
}
static bool text_output_room(bool *pending)
{
    if(s_mode==1) return ble_keys_text_room(pending);
    if(s_mode==2) return wifi_keys_text_room(pending);
    if(s_mode==3) return radio_keys_text_room(pending);
    return usb_keys_text_room(pending);
}

static void scan_task(void *arg)
{
    (void)arg;
    TickType_t next=xTaskGetTickCount();
    int64_t previous=0;
    uint8_t raw[BOARD_SCAN_BYTES];
    lx_input_t input;
    lx_input_init(&input,true); /* FPC9: six switches at raw bits 82..87. */
    bool last_power_preview=false,last_ptt_activity=false;
    lx_key_report_t last={0};
    uint32_t sequence=0;
    lx_unicode_t unicode={0};
    lx_key_report_t generated={0};
    int64_t unicode_next=0;
    bool automatic_pending=false;
    uint16_t last_consumer=0;
    while(1) {
        const int64_t begin=esp_timer_get_time();
        esp_err_t result=board_scan(raw);
        const uint32_t duration=(uint32_t)(esp_timer_get_time()-begin);
        uint32_t gap=previous?(uint32_t)(begin-previous):0;
        previous=begin;
        portENTER_CRITICAL(&s_lock);
        bool mouse_release=s_mouse_test_until && begin>=s_mouse_test_until;
        if(mouse_release) s_mouse_test_until=0;
        portEXIT_CRITICAL(&s_lock);
        if(mouse_release) { lx_mouse_sample_t release={0};mouse_input(&release); }
        portENTER_CRITICAL(&s_lock);
        uint16_t consumer=begin<s_consumer_test_until?s_consumer_test:0;
        portEXIT_CRITICAL(&s_lock);
        if(consumer!=last_consumer) { consumer_input(consumer);last_consumer=consumer; }
        if(result==ESP_OK && !s_switching) {
            /* SW71 is wired to the 74HC165 chain, raw bit 69 (MSB first).
             * The schematic's separate GPIO9 connection is absent on this PCB. */
            power_policy_button((raw[69/8] & (0x80u>>(69%8)))==0);
            uint8_t custom[2];custom_keys_get(custom);lx_input_set_custom(&input,custom);
            bool key_changed=lx_input_scan(&input,raw,(uint32_t)begin);
            if(key_changed || input.report.modifiers || input.report.keys[0] || input.ptt || input.power_preview ||
               input.ptt!=last_ptt_activity || input.power_preview!=last_power_preview) power_policy_activity();
            last_ptt_activity=input.ptt;
        }
        if(result==ESP_OK && s_mode!=4 && !s_switching) {
            voice_ptt(input.ptt);
            if(input.power_preview!=last_power_preview) {
                last_power_preview=input.power_preview;
                knob_power_preview(input.power_preview);
            }
            lx_key_report_t report=input.report;
            uint32_t test_sequence=0;
            bool cancel_output=false,cancel_ime=false;
            /* Automatic text uses a shallow queue and never blocks key scan.
             * Physical reports still cancel it and take the normal fast path. */
            bool output_pending=false;
            bool text_room=text_output_room(&output_pending);
            portENTER_CRITICAL(&s_lock);
            /* The final Space may still be queued after the generator ends.
             * Keep cancellation armed until all generated edges have left the
             * transport; otherwise dropping that Space leaves raw VUC in IME. */
            if(!lx_text_busy(&s_text) && !output_pending) s_pinyin_pending=false;
            /* Limited F13/F14 diagnostic fixture enters the physical-priority
             * branch. It validates arbitration, not switch electrical timing. */
            bool physical_fixture=s_test_physical && begin<s_test_until;
            if(physical_fixture) { report=s_test_report;test_sequence=s_test_sequence; }
            bool physical=report.modifiers || report.keys[0];
            s_physical=physical;
            if(s_voice_text && !voice_audio_ready()) s_unicode_cancel=true;
            if(s_unicode_cancel) {
                s_voice_text=false;
                /* Windows may still be processing a committed report. Clear
                 * composition throughout an active Pinyin phrase, including
                 * inter-character gaps, before handing over to physical keys. */
                cancel_ime=s_pinyin_pending;
                s_pinyin_pending=false;
                cancel_output=automatic_pending;automatic_pending=false;
                unicode.active=false;lx_text_cancel(&s_text);generated=(lx_key_report_t){0};s_unicode_cancel=false;
            }
            if(physical) {
                cancel_ime|=s_pinyin_pending;
                s_pinyin_pending=false;
                cancel_output|=automatic_pending;automatic_pending=false;
                unicode.active=false; s_unicode_request=0; generated=(lx_key_report_t){0};
                lx_text_cancel(&s_text);
                if(!physical_fixture) s_test_until=0;
            } else if(s_unicode_request && !lx_text_busy(&s_text)) {
                lx_unicode_begin(&unicode,s_unicode_request); s_unicode_request=0; unicode_next=0;
            }
            if(!physical && lx_text_busy(&s_text)) {
                automatic_pending=true;
                unicode.active=false;s_unicode_request=0;
                if(begin>=unicode_next && text_room) {
                    lx_text_next(&s_text,&generated);
                    if(s_text.encoding==LX_TEXT_PINYIN) s_pinyin_pending=true;
                    unicode_next=begin+(s_text.encoding==LX_TEXT_PINYIN?s_text_interval_us:12000);
                    if(s_text.encoding==LX_TEXT_PINYIN && !s_text.pinyin_active) unicode_next+=s_text_gap_us;
                }
                report=generated;
            } else if(!physical && unicode.active) {
                automatic_pending=true;
                if(begin>=unicode_next) { lx_unicode_next(&unicode,&generated); unicode_next=begin+12000; }
                report=generated;
            } else if(begin<s_test_until && !physical) {
                report=s_test_report; test_sequence=s_test_sequence;
            }
            bool power_text_active=lx_text_busy(&s_text) || unicode.active;
            portEXIT_CRITICAL(&s_lock);
            /* A stale last HID report while the PC is disconnected is not
             * ongoing user activity and must not defeat auto power-off. */
            if(power_text_active) power_policy_activity();
            if(cancel_output) {
                if(s_mode==1) ble_keys_cancel_pending();
                else if(s_mode==2) wifi_keys_cancel_pending();
                else if(s_mode==3) radio_keys_cancel_pending();
                else usb_keys_cancel_pending();
            }
            if(cancel_ime) ime_cancel(begin);
            if(cancel_output || memcmp(&report,&last,sizeof(report))) {
                last=report;
                uint32_t seq=test_sequence?test_sequence:++sequence;
                keyboard_submit(&report,seq,begin);
            }
        }
        portENTER_CRITICAL(&s_lock);
        s_stats.samples++;
        if(result!=ESP_OK) s_stats.errors++;
        else {
            if(memcmp(raw,s_stats.latest,sizeof(raw))) s_stats.changed++;
            memcpy(s_stats.latest,raw,sizeof(raw));
        }
        if(duration>s_stats.max_read_us) s_stats.max_read_us=duration;
        if(gap) {
            if(gap>s_stats.max_gap_us) s_stats.max_gap_us=gap;
            if(gap>2000) s_stats.over_2ms++;
            unsigned bucket=gap/100;
            s_stats.gap_buckets[bucket>40?40:bucket]++;
        }
        portEXIT_CRITICAL(&s_lock);
        vTaskDelayUntil(&next,1);
    }
}
static void consumer_input(uint16_t usage)
{
    if(s_switching) return;
    power_policy_activity();
    if(s_mode==1) ble_consumer_submit(usage);
    else if(s_mode==2) wifi_consumer_submit(usage);
    else if(s_mode==3) radio_consumer_submit(usage);
    else if(s_mode==0) usb_consumer_submit(usage);
}
static void mouse_input(const lx_mouse_sample_t *sample)
{
    if(s_switching) return;
    power_policy_mouse(sample->buttons!=0,sample->x || sample->y || sample->wheel || sample->pan);
    if(s_mode==1) ble_mouse_submit(sample);
    else if(s_mode==2) wifi_mouse_submit(sample);
    else if(s_mode==3) radio_mouse_submit(sample);
    else if(s_mode==0) usb_mouse_submit(sample);
}
static void host_packet(const lx_packet_t *packet,void *arg)
{
    (void)arg;
    if(packet->type==LX_MSG_CUSTOM_KEYS) custom_keys_receive(packet);
    else if(packet->type==LX_MSG_LIGHTING) lighting_settings_receive(packet);
    else if(packet->type==LX_MSG_ACTIVATION) activation_ui_receive(packet);
    else if(packet->type==LX_MSG_CONTROL) link_control_receive(LX_CONTROL_USB,packet);
    else if(packet->type==LX_MSG_HOST_STATS && packet->length==3) {
        knob_host_stats(packet->payload);
        bringup_usb_packet(LX_MSG_ACK,packet->sequence,NULL,0);
    } else if(packet->type==LX_MSG_SETUP) {
        device_setup_receive(packet);
    } else if(packet->type==LX_MSG_CONFIG) {
        esp_err_t result=network_apply_config(packet->payload,packet->length);
        uint8_t status[2]={(uint8_t)result,(uint8_t)(result>>8)};
        bringup_usb_packet(LX_MSG_ACK,packet->sequence,status,2);
    }
}
static void info(void)
{
    uint32_t flash=0; esp_flash_get_size(NULL,&flash);
    printf("LX_M1 role=keyboard idf=%s flash=%lu psram=%u usb_role=%s gpio4=%d usb_mounted=%u mode=%u\n",
        esp_get_idf_version(),(unsigned long)flash,(unsigned)esp_psram_get_size(),
        s_host?"host":"device",gpio_get_level(PIN_USB_MODE),s_host?0:bringup_usb_mounted(),s_mode);
    printf("LX_SD mode=%u gpio6=%d active_low=1\n",s_mode,gpio_get_level(PIN_USB_SD_EN));
}
static void print_scan(void)
{
    portENTER_CRITICAL(&s_lock);
    __typeof__(s_stats) copy=s_stats;
    portEXIT_CRITICAL(&s_lock);
    printf("LX_M0 scan samples=%lu errors=%lu changes=%lu max_read_us=%lu max_gap_us=%lu over_2ms=%lu raw=",
        (unsigned long)copy.samples,(unsigned long)copy.errors,(unsigned long)copy.changed,
        (unsigned long)copy.max_read_us,(unsigned long)copy.max_gap_us,(unsigned long)copy.over_2ms);
    for(unsigned i=0;i<BOARD_SCAN_BYTES;i++) printf("%02x",copy.latest[i]);
    printf(" sw71_raw_down=%u histogram_100us=",(copy.latest[69/8] & (0x80u>>(69%8)))==0);
    for(unsigned i=0;i<41;i++) printf("%s%lu",i?",":"",(unsigned long)copy.gap_buckets[i]);
    printf("\n");
}
static void change_mode(uint8_t mode)
{
    if(mode>3) return;
    nvs_handle_t handle;
    esp_err_t result=nvs_open("lx_dev",NVS_READWRITE,&handle);
    if(result==ESP_OK) {
        result=nvs_set_u8(handle,"mode",mode);
        if(result==ESP_OK) result=nvs_commit(handle);
        nvs_close(handle);
    }
    printf("LX_M0 role_change=%s\n",esp_err_to_name(result)); fflush(stdout);
    if(result==ESP_OK) {
        s_switching=true;
        bool output_pending=false;text_output_room(&output_pending);
        portENTER_CRITICAL(&s_lock);
        bool composing=s_pinyin_pending && (lx_text_busy(&s_text) || output_pending);
        s_pinyin_pending=false;lx_text_cancel(&s_text);
        portEXIT_CRITICAL(&s_lock);
        voice_suspend();vTaskDelay(pdMS_TO_TICKS(40));
        lx_mouse_sample_t release={0};
        if(s_mode==1) { ble_keys_cancel_pending();ble_mouse_submit(&release);ble_consumer_submit(0); }
        else if(s_mode==2) { wifi_keys_cancel_pending();wifi_mouse_submit(&release);wifi_consumer_submit(0); }
        else if(s_mode==3) { radio_keys_cancel_pending();radio_mouse_submit(&release);radio_consumer_submit(0); }
        else if(!s_host) { usb_keys_cancel_pending();usb_mouse_submit(&release);usb_consumer_submit(0); }
        if(composing && s_mode<4) ime_cancel(esp_timer_get_time());
        board_audio_stop(); vTaskDelay(pdMS_TO_TICKS(100)); esp_restart();
    }
}
static void mode_task(void *arg)
{
    (void)arg;uint8_t mode;
    while(xQueueReceive(s_modes,&mode,portMAX_DELAY)==pdTRUE) {
        vTaskDelay(pdMS_TO_TICKS(200)); /* Let the configuration ACK leave USB/TCP. */
        if(mode==254) {
            if(!power_policy_commit_shutdown()) continue;
            s_switching=true;s_load_stop=true;
            printf("LX_POWER shutdown_begin\n");
            voice_audio_enable(false);
            portENTER_CRITICAL(&s_lock);lx_text_cancel(&s_text);s_unicode_request=0;s_test_until=0;portEXIT_CRITICAL(&s_lock);
            lx_key_report_t release={0};lx_mouse_sample_t mouse_release={0};
            if(s_mode==1) { ble_keys_cancel_pending();ble_mouse_submit(&mouse_release);ble_consumer_submit(0); }
            else if(s_mode==2) { wifi_keys_cancel_pending();wifi_mouse_submit(&mouse_release);wifi_consumer_submit(0); }
            else if(s_mode==3) { radio_keys_cancel_pending();radio_mouse_submit(&mouse_release);radio_consumer_submit(0); }
            else if(!s_host) { usb_keys_cancel_pending();usb_mouse_submit(&mouse_release);usb_consumer_submit(0); }
            if(s_mode<4) keyboard_submit(&release,0,esp_timer_get_time());
            for(unsigned i=0;i<500 && !voice_audio_stopped();i++) vTaskDelay(pdMS_TO_TICKS(10));
            board_lights_off();vTaskDelay(pdMS_TO_TICKS(100));
            if(s_mode==1) ble_keys_shutdown();
            esp_wifi_stop();
            printf("LX_POWER latch_release audio_stopped=%u gpio39=1 gpio45=0\n",voice_audio_stopped());
            fflush(stdout);uart_wait_tx_done(UART_NUM_0,pdMS_TO_TICKS(200));
            board_power_off();
            vTaskDelay(pdMS_TO_TICKS(1000));
            puts("LX_POWER external_supply_or_held_button_keeps_mcu_alive physical_off=not_confirmed");
            vTaskSuspend(NULL);
        }
        s_load_stop=true;while(s_load_task) vTaskDelay(pdMS_TO_TICKS(10));
        change_mode(mode==255?s_mode:mode);
    }
}
static void load_task(void *arg)
{
    (void)arg;
    for(unsigned i=0;i<30 && !s_load_stop;i++) { board_audio_test(); board_led_test(); vTaskDelay(pdMS_TO_TICKS(10)); }
    s_load_task=NULL;
    puts("LX_LOAD done");
    vTaskDelete(NULL);
}
void app_main(void)
{
    /* Hold board power before optional peripherals or USB initialization. */
    gpio_set_level(PIN_POWER,1); gpio_set_direction(PIN_POWER,GPIO_MODE_OUTPUT);
    /* Keep SD disabled until the saved input mode has been loaded. */
    ESP_ERROR_CHECK(gpio_set_level(PIN_USB_SD_EN,1));
    ESP_ERROR_CHECK(gpio_set_direction(PIN_USB_SD_EN,GPIO_MODE_OUTPUT));
    /* Only our new partition after the app can be initialized/erased here. */
    esp_err_t nvs=nvs_flash_init();
    if(nvs==ESP_ERR_NVS_NO_FREE_PAGES || nvs==ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); nvs=nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);
    nvs_handle_t handle;
    if(nvs_open("lx_dev",NVS_READONLY,&handle)==ESP_OK) {
        uint8_t encoding=LX_TEXT_GBK;
        nvs_get_u8(handle,"mode",&s_mode);
        nvs_get_u8(handle,"text_encoding",&encoding);
        if(encoding<=LX_TEXT_PINYIN) s_text_encoding=encoding;
        nvs_close(handle);
    }
    device_setup_init(false);
    custom_keys_init();
    lighting_settings_init();
    if(s_mode>3) s_mode=0;
    ESP_ERROR_CHECK(link_control_start(false,s_mode));
    /* Wi-Fi -> PC bypasses this generator. Keep the receiver HID route at
     * its original rate: sustained fast TCP VUC tests were not reliable. */
    if(s_mode==2) { s_text_interval_us=12000;s_text_gap_us=0; }
    s_host=s_mode!=0;
    ESP_ERROR_CHECK(board_init(s_host));
    ESP_ERROR_CHECK(board_audio_setup());
    s_modes=xQueueCreate(1,1);
    power_policy_init(s_mode,request_shutdown);
    /* This worker commits NVS before rebooting. Flash writes can disable cache
     * even with code/rodata in PSRAM (e.g. while the voice model is mapped).
     * Its stack must therefore remain in internal RAM. */
    if(!s_modes || xTaskCreatePinnedToCoreWithCaps(mode_task,"mode_change",4096,NULL,3,NULL,0,
        MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)!=pdPASS) abort();
    network_set_mode_handler(request_mode);
    network_set_extra_config_handler(voice_setting);
    ESP_ERROR_CHECK(user_controls_start());
    knob_set_handler(knob_command);
    ESP_ERROR_CHECK(knob_link_start());
    link_control_set_stats_handler(knob_host_stats);
    radio_set_stats_handler(knob_host_stats);
    wifi_set_stats_handler(knob_host_stats);
    bringup_usb_set_handler(host_packet,NULL);
    ESP_ERROR_CHECK(s_host?usb_host_base_start(s_mode==4?NULL:mouse_input):bringup_usb_start(false));
    if(!s_host) ESP_ERROR_CHECK(usb_keys_start());
    if(s_mode==3) ESP_ERROR_CHECK(radio_keys_start(false));
    if(s_mode==1) ESP_ERROR_CHECK(ble_keys_start());
    if(s_mode==2) ESP_ERROR_CHECK(wifi_keys_start(false));
    if(s_mode==0 || (s_mode==1 && network_profile_present())) {
        /* Voice still needs the router when input uses USB or BLE. */
        network_auto_retry(true);
        ESP_ERROR_CHECK(network_start(network_profile_present()));
    }
    voice_set_text_handler(voice_text_request,text_busy,voice_text_cancel);
    ESP_ERROR_CHECK(voice_capture_start());
    if(xTaskCreatePinnedToCore(scan_task,"key_scan",4096,NULL,12,NULL,1)!=pdPASS) abort();
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0,512,0,0,NULL,0));
    uart_vfs_dev_use_driver(UART_NUM_0);
    info(); board_print_i2c();
    char line[512];
    while(1) {
        if(fgets(line,sizeof(line),stdin)==NULL) {
            clearerr(stdin);vTaskDelay(pdMS_TO_TICKS(20));continue;
        }
        line[strcspn(line,"\r\n")]=0;
        if(!strcmp(line,"scan")) print_scan();
        else if(!strcmp(line,"power")) power_policy_stats();
        else if(!strcmp(line,"customkeys")) custom_keys_print();
        else if(!strcmp(line,"power test")) printf("LX_POWER_TEST failures=%u hardware_cut=0\n",power_policy_selftest());
        else if(!strcmp(line,"power lights test")) { power_policy_lights_test();puts("LX_POWER_TEST lights_off_lease_ms=10000 hardware_cut=0"); }
        else if(!strcmp(line,"power wake")) { power_policy_activity();puts("LX_POWER_TEST activity=1"); }
        else if(!strcmp(line,"serial test")) {
            for(unsigned i=0;i<1024;i++) printf("LX_SERIAL %04u 0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\n",i);
            puts("LX_SERIAL_END 1024");
        }
        else if(!strcmp(line,"serial direct") || !strcmp(line,"serial loop")) {
            bool loop=!strcmp(line,"serial loop");unsigned matched=0;int received=0;
            char tx[96],rx[96];
            fflush(stdout);uart_wait_tx_done(UART_NUM_0,pdMS_TO_TICKS(1000));
            if(loop) { uart_flush_input(UART_NUM_0);uart_set_loop_back(UART_NUM_0,true); }
            for(unsigned i=0;i<1024;i++) {
                int n=snprintf(tx,sizeof(tx),"LX_SERIAL %04u 0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n",i);
                uart_write_bytes(UART_NUM_0,tx,n);
                if(loop) {
                    received=uart_read_bytes(UART_NUM_0,rx,n,pdMS_TO_TICKS(200));
                    if(received!=n || memcmp(tx,rx,n)) break;
                    matched++;
                }
            }
            uart_wait_tx_done(UART_NUM_0,pdMS_TO_TICKS(1000));
            if(loop) {
                uart_set_loop_back(UART_NUM_0,false);uart_flush_input(UART_NUM_0);
                printf("LX_SERIAL_LOOP matched=%u last_received=%d\n",matched,received);
            } else puts("LX_SERIAL_END 1024");
        }
        else if(!strncmp(line,"serial baud ",12)) {
            unsigned baud=0;
            if(sscanf(line+12,"%u",&baud)==1 && (baud==57600 || baud==115200 || baud==230400)) {
                printf("LX_SERIAL_BAUD %u\n",baud);fflush(stdout);
                uart_wait_tx_done(UART_NUM_0,pdMS_TO_TICKS(100));vTaskDelay(pdMS_TO_TICKS(100));
                uart_set_baudrate(UART_NUM_0,baud); /* RAM-only diagnostic; boot remains at configured rate. */
            } else puts("LX_SERIAL_BAUD invalid");
        }
        else if(!strcmp(line,"selftest")) printf("LX_SELFTEST failures=%u\n",ble_keys_policy_selftest()+network_discovery_selftest()+lx_consumer_selftest()+lx_input_selftest()+lx_unicode_selftest()+lx_gbk_selftest()+lx_text_selftest()+lx_voice_focus_selftest()+asr_baidu_selftest()+chat_protocol_selftest()+chat_official_selftest()+lx_protocol_selftest()+lx_mouse_selftest()+lx_mouse_queue_selftest());
        else if(!strcmp(line,"opus test")) printf("LX_OPUS request=%s\n",esp_err_to_name(voice_test_opus()));
        else if(!strcmp(line,"asr test")) printf("LX_ASR_TEST request=%s\n",esp_err_to_name(voice_test_asr()));
        else if(!strncmp(line,"asr capture ",12)) {
            unsigned duration;
            printf("LX_ASR capture_request=%s\n",esp_err_to_name(sscanf(line+12,"%u",&duration)==1?voice_test_dictation(duration):ESP_ERR_INVALID_ARG));
        }
        else if(!strncmp(line,"voice codec ",12)) {
            unsigned duration=0;printf("LX_VOICE codec_request=%s\n",esp_err_to_name(sscanf(line+12,"%u",&duration)==1?voice_test_codec(duration):ESP_ERR_INVALID_ARG));
        }
        else if(!strcmp(line,"chat")) chat_stats();
        else if(!strcmp(line,"activation test")) printf("LX_ACTIVATION_TEST failures=%u hardware_output=0 cloud=0\n",activation_ui_selftest());
        else if(!strcmp(line,"chat start")) printf("LX_CHAT request=%s\n",esp_err_to_name(voice_chat_request()));
        else if(!strcmp(line,"chat capture")) printf("LX_CHAT_CAPTURE request=%s\n",esp_err_to_name(voice_chat_diagnostic(false)));
        else if(!strcmp(line,"chat capture test")) printf("LX_CHAT_CAPTURE request=%s\n",esp_err_to_name(voice_chat_diagnostic(true)));
        else if(!strcmp(line,"chat test")) printf("LX_CHAT_TEST request=%s\n",esp_err_to_name(voice_test_chat()));
        else if(!strcmp(line,"aec test")) printf("LX_AEC_TEST request=%s\n",esp_err_to_name(voice_test_aec()));
        else if(!strcmp(line,"chat setup")) printf("LX_CHAT_OFFICIAL request=%s\n",esp_err_to_name(voice_chat_setup(false)));
        else if(!strcmp(line,"chat code test")) printf("LX_ACTIVATION_AUDIO_TEST request=%s\n",esp_err_to_name(voice_test_activation()));
        else if(!strcmp(line,"chat probe")) printf("LX_CHAT_PROBE request=%s\n",esp_err_to_name(voice_chat_probe()));
        else if(!strcmp(line,"chat cloud on")) printf("LX_CHAT_CLOUD setting=%s\n",esp_err_to_name(chat_cloud_enable(true)));
        else if(!strcmp(line,"chat cloud off")) { voice_cancel();printf("LX_CHAT_CLOUD setting=%s\n",esp_err_to_name(chat_cloud_enable(false))); }
        else if(!strcmp(line,"chat activate")) printf("LX_CHAT_OFFICIAL request=%s\n",esp_err_to_name(voice_chat_setup(true)));
        else if(!strncmp(line,"chat url ",9)) printf("LX_CHAT setting=%s\n",esp_err_to_name(chat_setting(10,(const uint8_t*)line+9,strlen(line+9))));
        else if(!strncmp(line,"chat token ",11)) printf("LX_CHAT setting=%s\n",esp_err_to_name(chat_setting(11,(const uint8_t*)line+11,strlen(line+11))));
        else if(!strncmp(line,"asr key ",8)) printf("LX_ASR setting=%s\n",esp_err_to_name(asr_baidu_setting(8,(const uint8_t*)line+8,strlen(line+8))));
        else if(!strncmp(line,"asr secret ",11)) printf("LX_ASR setting=%s\n",esp_err_to_name(asr_baidu_setting(9,(const uint8_t*)line+11,strlen(line+11))));
        else if(!strcmp(line,"voice")) voice_capture_stats();
        else if(!strcmp(line,"playout test")) printf("LX_PLAYOUT_TEST failures=%u\n",voice_playout_selftest());
        else if(!strcmp(line,"voice cancel")) { voice_cancel();portENTER_CRITICAL(&s_lock);s_unicode_cancel=true;s_unicode_request=0;portEXIT_CRITICAL(&s_lock);puts("LX_VOICE cancelled"); }
        else if(!strcmp(line,"voice wake on")) { voice_wake_enable(true);puts("LX_WAKE enabled=1"); }
        else if(!strcmp(line,"voice wake off")) { voice_wake_enable(false);puts("LX_WAKE enabled=0"); }
        else if(!strcmp(line,"voice replay begin")) printf("LX_REPLAY %s\n",esp_err_to_name(voice_replay(0,NULL,0)));
        else if(!strcmp(line,"voice replay end")) printf("LX_REPLAY %s\n",esp_err_to_name(voice_replay(2,NULL,0)));
        else if(!strncmp(line,"voice pcm ",10)) {
            size_t n=strlen(line+10);int16_t pcm[80];bool valid=n && n<=320 && !(n%4);
            for(unsigned i=0;valid && i<n/4;i++) {
                char pair[3]={0};unsigned lo,hi;
                memcpy(pair,line+10+i*4,2);
                if(strspn(pair,"0123456789abcdefABCDEF")!=2 || sscanf(pair,"%x",&lo)!=1) { valid=false;break; }
                memcpy(pair,line+12+i*4,2);
                if(strspn(pair,"0123456789abcdefABCDEF")!=2 || sscanf(pair,"%x",&hi)!=1) { valid=false;break; }
                pcm[i]=(int16_t)(lo|(hi<<8));
            }
            printf("LX_REPLAY_PCM %s\n",esp_err_to_name(valid?voice_replay(1,pcm,n/4):ESP_ERR_INVALID_ARG));
        }
        else if(!strcmp(line,"voice dump")) voice_capture_dump();
        else if(!strncmp(line,"voice capture ",14)) {
            unsigned duration;
            printf("LX_VOICE capture_request=%s\n",esp_err_to_name(sscanf(line+14,"%u",&duration)==1?voice_test_capture(duration):ESP_ERR_INVALID_ARG));
        }
        else if(!strcmp(line,"textmode")) printf("LX_TEXT_MODE %s\n",s_text_encoding==LX_TEXT_GBK?"gbk":s_text_encoding==LX_TEXT_PINYIN?"pinyin":"unicode");
        else if(!strcmp(line,"textmode gbk")) printf("LX_TEXT_MODE setting=%s\n",esp_err_to_name(text_encoding_setting(LX_TEXT_GBK)));
        else if(!strcmp(line,"textmode unicode")) printf("LX_TEXT_MODE setting=%s\n",esp_err_to_name(text_encoding_setting(LX_TEXT_UNICODE)));
        else if(!strcmp(line,"textmode pinyin")) printf("LX_TEXT_MODE setting=%s\n",esp_err_to_name(text_encoding_setting(LX_TEXT_PINYIN)));
        else if(!strncmp(line,"textgap ",8)) {
            unsigned ms=0;
            if(sscanf(line+8,"%u",&ms)==1 && ms<=250) {
                portENTER_CRITICAL(&s_lock);s_text_gap_us=ms*1000;portEXIT_CRITICAL(&s_lock);
            }
            printf("LX_TEXT_GAP char_us=%u persistent=0\n",s_text_gap_us);
        }
        else if(!strncmp(line,"textpace ",9)) {
            unsigned ms=0;
            if(sscanf(line+9,"%u",&ms)==1 && ms>=1 && ms<=20) {
                portENTER_CRITICAL(&s_lock);s_text_interval_us=ms*1000;portEXIT_CRITICAL(&s_lock);
            }
            printf("LX_TEXT_PACE report_us=%u persistent=0\n",s_text_interval_us);
        }
        else if(!strncmp(line,"text ",5)) printf("LX_TEXT accepted=%u\n",text_request(line+5));
        else if(!strncmp(line,"unicode ",8)) {
            unsigned code; lx_unicode_t check;
            if(sscanf(line+8,"%x",&code)==1 && lx_unicode_begin(&check,code)) {
                portENTER_CRITICAL(&s_lock); s_unicode_request=code; portEXIT_CRITICAL(&s_lock);
                printf("LX_UNICODE %04x\n",code);
            } else puts("LX_UNICODE invalid");
        }
        else if(!strcmp(line,"cancel")) {
            portENTER_CRITICAL(&s_lock);
            s_unicode_cancel=true; s_unicode_request=0; s_test_until=0;s_mouse_test_until=1;s_consumer_test_until=0;
            portEXIT_CRITICAL(&s_lock);
            puts("LX_CANCELLED");
        }
        else if(!strcmp(line,"input")) { if(s_mode==1) ble_keys_stats(); else if(s_mode==2) wifi_keys_stats(); else if(s_mode==3) radio_keys_stats(); else usb_keys_print_stats(); }
        else if(!strcmp(line,"trace clear")) { if(s_mode==1) ble_keys_trace_clear(); else if(s_mode==2) wifi_keys_trace_clear(); else if(s_mode==3) radio_keys_trace_clear(); else usb_keys_trace_clear(); puts("LX_TRACE_CLEARED"); }
        else if(!strcmp(line,"trace")) { if(s_mode==1) ble_keys_trace_print(); else if(s_mode==2) wifi_keys_trace_print(); else if(s_mode==3) radio_keys_trace_print(); else usb_keys_trace_print(); }
        else if(!strcmp(line,"time")) printf("LX_TIME %lld\n",(long long)esp_timer_get_time());
        else if(!strcmp(line,"load")) {
            if(!s_load_task) { s_load_stop=false; xTaskCreatePinnedToCore(load_task,"peripheral_load",8192,NULL,2,&s_load_task,0); }
            puts("LX_LOAD started");
        }
        else if(!strcmp(line,"prepare")) {
            s_load_stop=true;
            while(s_load_task) vTaskDelay(pdMS_TO_TICKS(10));
            voice_suspend();vTaskDelay(pdMS_TO_TICKS(40));
            board_audio_stop(); puts("LX_PREPARED");
        }
        else if(!strncmp(line,"test ",5) || !strncmp(line,"ptest ",6)) {
            unsigned seq=0,key=0;
            /* Diagnostic injection is limited to F13/F14 or release; a lost host
             * automatically releases it within 300 ms. Physical keys win. */
            bool priority=line[0]=='p';
            if(sscanf(line+(priority?6:5),"%u %u",&seq,&key)==2 && (key==0 || key==0x68 || key==0x69)) {
                portENTER_CRITICAL(&s_lock);
                s_test_report=(lx_key_report_t){.keys={key}};
                s_test_physical=priority;
                s_test_sequence=seq;
                s_test_until=esp_timer_get_time()+300000;
                portEXIT_CRITICAL(&s_lock);
                printf("LX_TEST %u\n",seq);
            } else puts("LX_TEST invalid");
        }
        else if(!strncmp(line,"ctest ",6)) {
            unsigned usage;
            /* Reserved consumer usage: exercise Report 3 without changing volume or playback. */
            if(sscanf(line+6,"%u",&usage)==1 && (usage==0 || usage==0x3ff)) {
                portENTER_CRITICAL(&s_lock);
                s_consumer_test=usage;s_consumer_test_until=esp_timer_get_time()+300000;
                portEXIT_CRITICAL(&s_lock);printf("LX_CTEST %u\n",usage);
            } else puts("LX_CTEST invalid");
        }
        else if(!strncmp(line,"mtest ",6)) {
            unsigned seq,buttons;int x,y,wheel,pan;
            if(sscanf(line+6,"%u %u %d %d %d %d",&seq,&buttons,&x,&y,&wheel,&pan)==6 && buttons<=31 &&
                x>=-1024 && x<=1024 && y>=-1024 && y<=1024 && wheel>=-8 && wheel<=8 && pan>=-8 && pan<=8) {
                lx_mouse_sample_t sample={.buttons=buttons,.x=x,.y=y,.wheel=wheel,.pan=pan};
                portENTER_CRITICAL(&s_lock);s_mouse_test_until=esp_timer_get_time()+300000;portEXIT_CRITICAL(&s_lock);
                mouse_input(&sample);printf("LX_MTEST %u\n",seq);
            } else puts("LX_MTEST invalid");
        }
        else if(!strcmp(line,"i2c")) board_print_i2c();
        else if(!strcmp(line,"uart")) knob_link_stats();
        else if(!strcmp(line,"controls")) user_controls_stats();
        else if(!strncmp(line,"knob ",5)) {
            size_t n=strlen(line+5);uint8_t bytes[38];bool valid=n && n<=76 && !(n&1);
            for(size_t i=0;valid && i<n/2;i++) {
                unsigned value;char pair[]={line[5+i*2],line[6+i*2],0};
                if(strspn(pair,"0123456789abcdefABCDEF")!=2 || sscanf(pair,"%x",&value)!=1) valid=false;
                else bytes[i]=value;
            }
            printf("LX_KNOB_INJECT %s\n",esp_err_to_name(valid?knob_inject(bytes,n/2):ESP_ERR_INVALID_ARG));
        }
        else if(!strcmp(line,"mouse")) board_print_mouse();
        else if(!strcmp(line,"mouse recover")) printf("LX_MOUSE_ROOT request=%s\n",esp_err_to_name(board_mouse_recover(false)));
        else if(!strcmp(line,"mouse fault")) printf("LX_MOUSE_FAULT request=%s\n",esp_err_to_name(board_mouse_recover(true)));
        else if(!strcmp(line,"ble power 9") && s_mode==1) ble_keys_power(false);
        else if(!strcmp(line,"ble power 20") && s_mode==1) ble_keys_power(true);
        else if(!strcmp(line,"radio pause 1200") && s_mode==3) { radio_keys_pause(1200);puts("LX_RADIO_PAUSED 1200"); }
        else if(!strncmp(line,"radio rate ",11) && s_mode==3) {
            unsigned rate=0;printf("LX_RADIO_RATE %s\n",esp_err_to_name(sscanf(line+11,"%u",&rate)==1?radio_keys_rate(rate):ESP_ERR_INVALID_ARG));
        }
        else if(!strcmp(line,"network")) network_print();
        else if(!strcmp(line,"wifi disconnect")) printf("LX_NETWORK disconnect_test=%s\n",esp_err_to_name(network_disconnect_test()));
        else if(!strcmp(line,"wifi retry")) printf("LX_NETWORK retry=%s\n",esp_err_to_name(network_retry()));
        else if(!strncmp(line,"wifi ",5)) {
            char *value=strchr(line+5,' ');
            if(value) { *value++=0;printf("LX_NETWORK setting=%s result=%s apply=restart\n",line+5,esp_err_to_name(network_setting(line+5,value))); }
        }
        else if(!strcmp(line,"battery")) battery_led_stats();
        else if(!strcmp(line,"battery test")) printf("LX_BATTERY_TEST failures=%u\n",battery_led_selftest());
        else if(!strcmp(line,"led")) board_led_test();
        else if(!strcmp(line,"sideled")) board_side_led_stats();
        else if(!strcmp(line,"audio") && !s_load_task) board_audio_test();
        else if(!strcmp(line,"audio channels") && !s_load_task) board_audio_channel_test();
        else if(!strcmp(line,"tone") && !s_load_task) printf("LX_TONE result=%s sound=%u\n",esp_err_to_name(board_audio_tone()),user_controls_sound_on());
        else if(!strncmp(line,"tone ",5) && !s_load_task) {
            unsigned volume=0;
            printf("LX_TONE result=%s sound=%u\n",esp_err_to_name(sscanf(line+5,"%u",&volume)==1?board_audio_tone_volume(volume):ESP_ERR_INVALID_ARG),user_controls_sound_on());
        }
        else if((!strcmp(line,"role device") || !strcmp(line,"mode usb")) && !s_load_task) change_mode(0);
        else if(!strcmp(line,"mode now") && !s_load_task) change_mode(3);
        else if(!strcmp(line,"mode ble") && !s_load_task) change_mode(1);
        else if(!strcmp(line,"mode wifi") && !s_load_task) change_mode(2);
        else info();
        fflush(stdout);
    }
}
