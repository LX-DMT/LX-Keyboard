#include <stdio.h>
#include <string.h>
#include "esp_mac.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "bringup_usb.h"

static const char *s_role;
static char s_serial[40];
static lx_stream_t s_stream;
static lx_packet_callback_t s_handler;
static void *s_handler_arg;
void bringup_usb_set_handler(lx_packet_callback_t handler,void *arg) { s_handler=handler;s_handler_arg=arg; }
bool bringup_usb_packet(uint8_t type,uint32_t sequence,const void *payload,uint16_t length)
{
    uint8_t bytes[LX_PACKET_MAX];size_t n=lx_packet_encode(bytes,sizeof(bytes),type,sequence,0,payload,length);
    if(!n) return false;
    if(tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,bytes,n)!=n) return false;
    return tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0,0)==ESP_OK;
}
static void packet_received(const lx_packet_t *packet,void *arg)
{
    (void)arg;
    if(packet->type==LX_MSG_HELLO) bringup_usb_packet(LX_MSG_HELLO,packet->sequence,s_role,strlen(s_role));
    else if(s_handler) s_handler(packet,s_handler_arg);
}
static tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON, .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE, .idVendor = 0x303a,
    .bcdDevice = 0x0100, .iManufacturer = 1, .iProduct = 2,
    .iSerialNumber = 3, .bNumConfigurations = 1,
};
static const char *s_strings[] = {(const char[]){0x09, 0x04}, "LX Development", NULL,
                                 s_serial, "LX input", "LX diagnostics"};
static const uint8_t s_hid[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(1)),
    /* Eight buttons, relative signed X/Y/wheel/pan; report payload stays 5 bytes. */
    0x05,0x01,0x09,0x02,0xa1,0x01,0x85,0x02,0x09,0x01,0xa1,0x00,
    0x05,0x09,0x19,0x01,0x29,0x08,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,
    0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x03,0x81,0x06,
    0x05,0x0c,0x0a,0x38,0x02,0x95,0x01,0x81,0x06,0xc0,0xc0,
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(3)),
};
#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)
static const uint8_t s_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, CONFIG_LEN, 0, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, sizeof(s_hid), 0x81, 16, 1),
    TUD_CDC_DESCRIPTOR(1, 5, 0x82, 8, 0x03, 0x83, 64),
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid;
}
const uint8_t *bringup_hid_descriptor(uint16_t *length) { *length=sizeof(s_hid);return s_hid; }
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                             hid_report_type_t type, uint8_t *buffer, uint16_t length)
{
    (void)instance; (void)report_id; (void)type; (void)buffer; (void)length;
    return 0;
}
static void cdc_rx(int itf, cdcacm_event_t *event)
{
    (void)event;
    uint8_t bytes[64];
    size_t count;
    if (tinyusb_cdcacm_read(itf, bytes, sizeof(bytes), &count) != ESP_OK || count == 0) return;
    if(count && bytes[0]=='?') {
        char reply[100];
        int length=snprintf(reply,sizeof(reply),"LX_DEV role=%s usb=device\n",s_role);
        tinyusb_cdcacm_write_queue(itf,(const uint8_t*)reply,length);tinyusb_cdcacm_write_flush(itf,0);
    } else lx_stream_feed(&s_stream,bytes,count,packet_received,NULL);
}
esp_err_t bringup_usb_start(bool receiver)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    s_role = receiver ? "receiver" : "keyboard";
    s_device.idProduct = receiver ? 0x4002 : 0x4001;
    s_strings[2] = receiver ? "LX Receiver Dev" : "LX Keyboard Dev";
    snprintf(s_serial, sizeof(s_serial), "LX%sDEV%02X%02X%02X%02X%02X%02X",
             receiver ? "RX" : "KB", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    tinyusb_config_t usb = {
        .device_descriptor = &s_device, .string_descriptor = s_strings,
        .string_descriptor_count = sizeof(s_strings)/sizeof(s_strings[0]),
        .configuration_descriptor = s_configuration,
    };
    esp_err_t result = tinyusb_driver_install(&usb);
    if (result != ESP_OK) return result;
    tinyusb_config_cdcacm_t cdc = {
        .usb_dev = TINYUSB_USBDEV_0, .cdc_port = TINYUSB_CDC_ACM_0, .callback_rx = cdc_rx,
    };
    return tusb_cdc_acm_init(&cdc);
}
bool bringup_usb_mounted(void) { return tud_mounted(); }
