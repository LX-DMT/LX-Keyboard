#include <stdio.h>
#include <string.h>
#include "board.h"
#include "hid_mouse.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "soc/usb_dwc_struct.h"
#include "soc/usb_wrap_struct.h"
#include "soc/rtc_cntl_struct.h"

typedef struct {
    hid_host_device_handle_t handle;
    lx_mouse_map_t map;
    hid_host_dev_params_t params;
    uint16_t vid,pid;
    bool active,attached,closing,disconnected,recover;
    uint8_t buttons,retries,raw[65],raw_length;
    uint32_t reports;
} mouse_interface_t;
static mouse_interface_t s_interfaces[4];
static QueueHandle_t s_events;
static TaskHandle_t s_attach_task;
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_connected,s_reports,s_rejected,s_errors,s_ignored,s_attaches,s_detaches,s_recoveries;
static esp_err_t s_last_error;
static bool s_root_request;
static uint32_t s_root_cycles,s_fault_injected;
static unsigned s_root_attempts;
static lx_mouse_sample_t s_latest;
static void (*s_mouse_input)(const lx_mouse_sample_t *);

/* Called with s_lock held. Keep buttons from other mouse interfaces. */
static lx_mouse_sample_t deactivate(mouse_interface_t *ctx)
{
    if(ctx->active) { ctx->active=false;s_connected--; }
    ctx->buttons=0;
    lx_mouse_sample_t release={0};
    for(unsigned i=0;i<4;i++) release.buttons|=s_interfaces[i].buttons;
    s_latest=release;
    return release;
}
static void interface_event(hid_host_device_handle_t handle,hid_host_interface_event_t event,void *arg)
{
    mouse_interface_t *ctx=arg;
    if(event==HID_HOST_INTERFACE_EVENT_DISCONNECTED || event==HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        portENTER_CRITICAL(&s_lock);
        bool was_active=ctx->active;
        lx_mouse_sample_t release=deactivate(ctx);
        if(event==HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
            ctx->disconnected=true;
            if(ctx->attached) { s_detaches++;ctx->attached=false; }
        } else { ctx->recover=true;s_errors++;s_last_error=ESP_FAIL; }
        portEXIT_CRITICAL(&s_lock);
        if(was_active && s_mouse_input) s_mouse_input(&release);
        printf("LX_MOUSE event=%s address=%u interface=%u\n",
            event==HID_HOST_INTERFACE_EVENT_DISCONNECTED?"disconnect":"transfer_error",ctx->params.addr,ctx->params.iface_num);
        /* Per-interface flags cannot be lost when the attachment queue fills. */
        xTaskNotifyGive(s_attach_task);
    } else if(event==HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        lx_mouse_sample_t sample;bool ok=false;
        portENTER_CRITICAL(&s_lock);
        if(ctx->active && ctx->handle==handle) {
            size_t length=0;
            esp_err_t result=hid_host_device_get_raw_input_report_data(handle,ctx->raw,sizeof(ctx->raw),&length);
            ctx->raw_length=length;
            bool mouse_report=!ctx->map.report_ids;
            if(length && ctx->map.report_ids)
                for(unsigned i=0;i<ctx->map.count;i++) mouse_report|=ctx->map.fields[i].report_id==ctx->raw[0];
            ok=result==ESP_OK && lx_mouse_decode(&ctx->map,ctx->raw,length,&sample);
            if(ok) {
                ctx->buttons=(ctx->buttons & ~sample.button_mask) | sample.buttons;
                ctx->reports++;ctx->retries=0;s_root_attempts=0;
            } else if(result==ESP_OK && length && !mouse_report) s_ignored++;
            else s_rejected++;
        }
        if(ok) {
            sample.buttons=0;
            for(unsigned i=0;i<4;i++) sample.buttons|=s_interfaces[i].buttons;
            s_latest=sample;s_reports++;
        }
        portEXIT_CRITICAL(&s_lock);
        if(ok && s_mouse_input) s_mouse_input(&sample);
    }
}
static void driver_event(hid_host_device_handle_t handle,hid_host_driver_event_t event,void *arg)
{
    (void)arg;(void)event;
    if(xQueueSend(s_events,&handle,0)!=pdTRUE) {
        /* An unopened interface has no user callback and closes in one call. */
        esp_err_t result=hid_host_device_close(handle);
        portENTER_CRITICAL(&s_lock);s_errors++;s_last_error=result==ESP_OK?ESP_ERR_NO_MEM:result;portEXIT_CRITICAL(&s_lock);
    }
    xTaskNotifyGive(s_attach_task);
}
static void close_interface(mouse_interface_t *ctx)
{
    portENTER_CRITICAL(&s_lock);
    bool final=ctx->disconnected,was_active=ctx->active;
    ctx->closing=true;
    lx_mouse_sample_t release=deactivate(ctx);
    portEXIT_CRITICAL(&s_lock);
    if(was_active && s_mouse_input) s_mouse_input(&release);
    esp_err_t result=hid_host_device_close(ctx->handle);
    portENTER_CRITICAL(&s_lock);
    /* usb_host_hid 1.2.0 first calls DISCONNECTED when an opened interface
     * is closed by us. Retain the context until its second close acknowledges
     * that callback, including for unsupported composite interfaces. */
    if(final && (result==ESP_OK || result==ESP_ERR_INVALID_ARG)) memset(ctx,0,sizeof(*ctx));
    else if(result!=ESP_OK) { s_errors++;s_last_error=result; }
    portEXIT_CRITICAL(&s_lock);
    if(result!=ESP_OK) printf("LX_MOUSE close_error=%s\n",esp_err_to_name(result));
}
static void attach_interface(hid_host_device_handle_t handle)
{
    mouse_interface_t *ctx=NULL;
    for(unsigned i=0;i<4;i++) if(!s_interfaces[i].handle) { ctx=&s_interfaces[i];break; }
    if(!ctx) {
        hid_host_device_close(handle);
        portENTER_CRITICAL(&s_lock);s_rejected++;s_last_error=ESP_ERR_NO_MEM;portEXIT_CRITICAL(&s_lock);
        return;
    }
    ctx->handle=handle;
    hid_host_device_config_t config={.callback=interface_event,.callback_arg=ctx};
    esp_err_t result=hid_host_device_open(handle,&config);
    if(result!=ESP_OK) {
        printf("LX_MOUSE open_error=%s\n",esp_err_to_name(result));
        hid_host_device_close(handle);
        portENTER_CRITICAL(&s_lock);memset(ctx,0,sizeof(*ctx));s_errors++;s_last_error=result;portEXIT_CRITICAL(&s_lock);
        return;
    }
    hid_host_dev_info_t identity={0};
    hid_host_get_device_info(handle,&identity);
    ctx->vid=identity.VID;ctx->pid=identity.PID;
    size_t length=0;uint8_t saved_descriptor[1024];
    const uint8_t *descriptor=hid_host_get_report_descriptor(handle,&length);
    /* The driver frees its descriptor before DISCONNECTED. Both tasks run on
     * core 0: copy under the lock, then parse/log our copy without retaining
     * a driver pointer across UART writes or other scheduling points. */
    portENTER_CRITICAL(&s_lock);
    bool copied=!ctx->disconnected && descriptor && length<=sizeof(saved_descriptor);
    if(copied) memcpy(saved_descriptor,descriptor,length);
    portEXIT_CRITICAL(&s_lock);
    descriptor=copied?saved_descriptor:NULL;
    bool supported=hid_host_device_get_params(handle,&ctx->params)==ESP_OK && descriptor && lx_mouse_parse(&ctx->map,descriptor,length);
    printf("LX_MOUSE device=%04x:%04x address=%u interface=%u subclass=%u protocol=%u descriptor=%u mouse=%u\n",
        ctx->vid,ctx->pid,ctx->params.addr,ctx->params.iface_num,ctx->params.sub_class,ctx->params.proto,(unsigned)length,supported);
    /* Bounded, one-time descriptor evidence; never log on every input report. */
    for(size_t at=0;descriptor && at<length && at<1024;at+=32) {
        printf("LX_MOUSE_DESC interface=%u offset=%u hex=",ctx->params.iface_num,(unsigned)at);
        for(size_t j=at;j<length && j<at+32;j++) printf("%02x",descriptor[j]);
        putchar('\n');
    }
    if(supported && ctx->params.sub_class==HID_SUBCLASS_BOOT_INTERFACE) {
        result=hid_class_request_set_protocol(handle,HID_REPORT_PROTOCOL_REPORT);
        supported=result==ESP_OK;
    }
    portENTER_CRITICAL(&s_lock);
    bool disconnected=ctx->disconnected;
    if(supported && !disconnected) { ctx->active=true;s_connected++; }
    portEXIT_CRITICAL(&s_lock);
    if(supported && !disconnected) result=hid_host_device_start(handle);
    else if(result==ESP_OK) result=ESP_ERR_NOT_SUPPORTED;
    if(result==ESP_OK) {
        portENTER_CRITICAL(&s_lock);ctx->attached=true;s_attaches++;portEXIT_CRITICAL(&s_lock);
        printf("LX_MOUSE attached address=%u interface=%u fields=%u\n",ctx->params.addr,ctx->params.iface_num,ctx->map.count);
    } else {
        printf("LX_MOUSE skipped interface=%u reason=%s\n",ctx->params.iface_num,esp_err_to_name(result));
        portENTER_CRITICAL(&s_lock);
        if(result==ESP_ERR_NOT_SUPPORTED) s_ignored++;
        else { s_errors++;s_last_error=result; }
        portEXIT_CRITICAL(&s_lock);
        close_interface(ctx);
    }
}
static void recover_root_port(void)
{
    /* Recycling only the IN endpoint cannot repair a lost USB address or a
     * disabled root port after a plug/contact error. Use the IDF root-port
     * API, allowing the HID driver to release all interfaces before restart.
     * This does not switch board 5V and does not restart BLE/keyboard/audio. */
    portENTER_CRITICAL(&s_lock);
    s_root_request=false;s_root_cycles++;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t off=usb_host_lib_set_root_port_power(false);
    printf("LX_MOUSE root_off=%s\n",esp_err_to_name(off));
    if(off!=ESP_OK) { portENTER_CRITICAL(&s_lock);s_errors++;s_last_error=off;portEXIT_CRITICAL(&s_lock);return; }
    bool drained=false;
    for(unsigned wait=0;wait<200;wait++) {
        bool occupied=false;
        for(unsigned i=0;i<4;i++) {
            if(s_interfaces[i].disconnected) close_interface(&s_interfaces[i]);
            occupied|=s_interfaces[i].handle!=NULL;
        }
        usb_host_lib_info_t info={0};usb_host_lib_info(&info);
        if(wait>=30 && !occupied && !info.num_devices) { drained=true;break; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* Closed-but-unopened handles queued before power-off no longer exist. */
    if(drained) xQueueReset(s_events);
    esp_err_t on=ESP_ERR_INVALID_STATE;
    for(unsigned wait=0;wait<20 && on!=ESP_OK;wait++) {
        on=usb_host_lib_set_root_port_power(true);
        if(on!=ESP_OK) vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("LX_MOUSE root_on=%s drained=%u\n",esp_err_to_name(on),drained);
    if(on!=ESP_OK || !drained) {
        portENTER_CRITICAL(&s_lock);s_errors++;s_last_error=on!=ESP_OK?on:ESP_ERR_TIMEOUT;portEXIT_CRITICAL(&s_lock);
    }
}
esp_err_t board_mouse_recover(bool fault)
{
    if(!s_attach_task) return ESP_ERR_INVALID_STATE;
    if(fault) {
        /* Diagnostic: exercise the exhausted transfer-error branch without
         * pretending this software event is a physical cable fault. */
        mouse_interface_t *ctx=NULL;
        portENTER_CRITICAL(&s_lock);
        for(unsigned i=0;i<4;i++) if(s_interfaces[i].active) { ctx=&s_interfaces[i];ctx->retries=3;break; }
        if(ctx) s_fault_injected++;
        portEXIT_CRITICAL(&s_lock);
        if(!ctx) return ESP_ERR_INVALID_STATE;
        interface_event(ctx->handle,HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR,ctx);
    } else {
        portENTER_CRITICAL(&s_lock);s_root_request=true;s_root_attempts=0;portEXIT_CRITICAL(&s_lock);
        xTaskNotifyGive(s_attach_task);
    }
    return ESP_OK;
}
static void attach_task(void *arg)
{
    (void)arg;
    while(1) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        for(unsigned i=0;i<4;i++) {
            mouse_interface_t *ctx=&s_interfaces[i];
            portENTER_CRITICAL(&s_lock);
            bool disconnected=ctx->disconnected,recover=ctx->recover;
            ctx->recover=false;
            portEXIT_CRITICAL(&s_lock);
            if(disconnected) close_interface(ctx);
            else if(recover && !ctx->closing) {
                /* The official driver stops submitting IN transfers on error.
                 * Clear the endpoint and retry at most three times until valid
                 * mouse input arrives. Buttons were already released above. */
                esp_err_t result=ESP_FAIL;
                if(ctx->retries++<3) {
                    result=hid_host_device_stop(ctx->handle);
                    if(result==ESP_OK) {
                        portENTER_CRITICAL(&s_lock);
                        if(!ctx->disconnected) { ctx->active=true;s_connected++; }
                        portEXIT_CRITICAL(&s_lock);
                        result=hid_host_device_start(ctx->handle);
                    }
                }
                printf("LX_MOUSE recovery=%s attempt=%u\n",esp_err_to_name(result),ctx->retries);
                if(result==ESP_OK) { portENTER_CRITICAL(&s_lock);s_recoveries++;portEXIT_CRITICAL(&s_lock); }
                else {
                    portENTER_CRITICAL(&s_lock);
                    bool retry=s_root_attempts<3;
                    if(retry) { s_root_attempts++;s_root_request=true; }
                    portEXIT_CRITICAL(&s_lock);
                    if(!retry) close_interface(ctx);
                }
            }
        }
        portENTER_CRITICAL(&s_lock);bool root_request=s_root_request;portEXIT_CRITICAL(&s_lock);
        if(root_request) recover_root_port();
        hid_host_device_handle_t handle;
        while(xQueueReceive(s_events,&handle,0)==pdTRUE) {
            /* Finish synchronous DISCONNECTED callbacks before reusing slots. */
            for(unsigned i=0;i<4;i++) if(s_interfaces[i].disconnected) close_interface(&s_interfaces[i]);
            attach_interface(handle);
        }
    }
}
static void library_task(void *arg)
{
    (void)arg;
    while(1) {
        uint32_t flags;usb_host_lib_handle_events(portMAX_DELAY,&flags);
        if(flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}
static void hid_events_task(void *arg)
{
    (void)arg;
    /* Use the driver's supported caller-owned event loop, with the same
     * priority/core as its default task but a PSRAM stack. Transfer buffers
     * and HCD DMA descriptors still use the driver's required internal RAM. */
    while(hid_host_handle_events(portMAX_DELAY)==ESP_OK) {}
    vTaskDeleteWithCaps(NULL);
}
void board_print_mouse(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t connected=s_connected,reports=s_reports,rejected=s_rejected,errors=s_errors;
    uint32_t ignored=s_ignored,attaches=s_attaches,detaches=s_detaches,recoveries=s_recoveries;
    esp_err_t error=s_last_error;
    uint32_t root_cycles=s_root_cycles,fault_injected=s_fault_injected;
    lx_mouse_sample_t latest=s_latest;
    portEXIT_CRITICAL(&s_lock);
    printf("LX_MOUSE connected=%lu reports=%lu rejected=%lu errors=%lu buttons=%02x x=%ld y=%ld wheel=%ld pan=%ld forwarding=%u\n",
        (unsigned long)connected,(unsigned long)reports,(unsigned long)rejected,(unsigned long)errors,latest.buttons,
        (long)latest.x,(long)latest.y,(long)latest.wheel,(long)latest.pan,s_mouse_input!=NULL);
    usb_host_lib_info_t info={0};
    if(s_events) usb_host_lib_info(&info);
    printf("LX_MOUSE_HOST devices=%d attaches=%lu detaches=%lu ignored=%lu recoveries=%lu last_error=%s internal_free=%u internal_largest=%u\n",
        info.num_devices,(unsigned long)attaches,(unsigned long)detaches,(unsigned long)ignored,(unsigned long)recoveries,
        esp_err_to_name(error),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    printf("LX_MOUSE_RECOVERY root_cycles=%lu fault_injected=%lu\n",(unsigned long)root_cycles,(unsigned long)fault_injected);
    printf("LX_MOUSE_DMA free=%u largest=%u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL));
    if(s_events) {
        /* Read-only S3 link evidence: no PHY override or register writes. */
        usb_dwc_hprt_reg_t port={.val=USB_DWC.hprt_reg.val};
        usb_wrap_otg_conf_reg_t phy={.val=USB_WRAP.otg_conf.val};
        printf("LX_MOUSE_PORT connected=%u enabled=%u powered=%u suspended=%u line=%u speed=%u hprt=%08lx phy=%08lx pad=%u external=%u otg_phy=%u\n",
            port.prtconnsts,port.prtena,port.prtpwr,port.prtsusp,port.prtlnsts,port.prtspd,
            (unsigned long)port.val,(unsigned long)phy.val,phy.pad_enable,phy.phy_sel,RTCCNTL.usb_conf.sw_usb_phy_sel);
    }
    for(unsigned i=0;i<4;i++) {
        portENTER_CRITICAL(&s_lock);mouse_interface_t copy=s_interfaces[i];portEXIT_CRITICAL(&s_lock);
        if(!copy.handle) continue;
        printf("LX_MOUSE_IF device=%04x:%04x address=%u interface=%u active=%u closing=%u reports=%lu buttons=%02x raw=",
            copy.vid,copy.pid,copy.params.addr,copy.params.iface_num,copy.active,copy.closing,(unsigned long)copy.reports,copy.buttons);
        for(unsigned j=0;j<copy.raw_length;j++) printf("%02x",copy.raw[j]);
        putchar('\n');
    }
}
esp_err_t usb_host_base_start(void (*mouse_input)(const lx_mouse_sample_t *))
{
    s_mouse_input=mouse_input;
    s_events=xQueueCreate(8,sizeof(hid_host_device_handle_t));
    if(!s_events) return ESP_ERR_NO_MEM;
    usb_host_config_t host={.intr_flags=ESP_INTR_FLAG_LEVEL1};
    esp_err_t result=usb_host_install(&host);
    if(result!=ESP_OK) return result;
    if(xTaskCreatePinnedToCoreWithCaps(library_task,"usb_host_lib",4096,NULL,7,NULL,0,
        MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS) return ESP_ERR_NO_MEM;
    /* Descriptor processing does not need a DMA/internal stack. Leave internal
     * RAM for hot-plug transfer allocations after BLE, Wi-Fi and audio start. */
    if(xTaskCreatePinnedToCoreWithCaps(attach_task,"mouse_attach",4096,NULL,5,&s_attach_task,0,
        MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS) return ESP_ERR_NO_MEM;
    hid_host_driver_config_t driver={.create_background_task=false,.callback=driver_event};
    result=hid_host_install(&driver);
    if(result!=ESP_OK) return result;
    return xTaskCreatePinnedToCoreWithCaps(hid_events_task,"hid_host",4096,NULL,8,NULL,0,
        MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
