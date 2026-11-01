/**
 * @file ble_dial_hid.c
 * @brief NimBLE HID GATT 服务，模拟 Windows Surface Dial
 *
 * 广播名 LX_Dial；HID report map 与 PnP ID 对齐 Dial 类设备。
 */

#include "ble_dial_hid.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>

static const char *TAG = "ble_dial_hid";

#define DIAL_REPORT_ID 10

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_report_handle;
static bool s_active;
static bool s_host_ready;
static bool s_notify_enabled;
static uint8_t s_last_report[2];

static const uint8_t s_hid_report_map[] = {
    0x05, 0x01, 0x09, 0x0e, 0xa1, 0x01, 0x85, 10, 0x05, 0x0d, 0x09, 0x21, 0xa1, 0x00,
    0x05, 0x09, 0x09, 0x01, 0x95, 0x01, 0x75, 0x01, 0x15, 0x00, 0x25, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x37, 0x95, 0x01, 0x75, 0x0f, 0x55, 0x0f, 0x65, 0x14, 0x36, 0xf0, 0xf1,
    0x46, 0x10, 0x0e, 0x16, 0xf0, 0xf1, 0x26, 0x10, 0x0e, 0x81, 0x06, 0xc0, 0xc0,
};

/* bcdHID 1.11, country 0, remote wake */
static const uint8_t s_hid_info[4] = { 0x11, 0x01, 0x00, 0x01 };
static const uint8_t s_report_ref[] = { DIAL_REPORT_ID, 0x01 };
static const uint8_t s_battery_level = 100;
static const char s_manufacturer[] = "LX";
static const uint8_t s_pnp_id[] = {
    0x02,                   /* USB vendor ID source */
    0xac, 0x05,             /* vid 0x05ac */
    0x0a, 0x82,             /* pid 0x820a */
    0x10, 0x02,             /* product version 0x0210 */
};

static int ble_dial_hid_gap_event(struct ble_gap_event *event, void *arg);
static int ble_dial_hid_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ble_dial_hid_dsc_access(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(0x2A29),
                .access_cb = ble_dial_hid_chr_access,
                .arg = (void *)s_manufacturer,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A50),
                .access_cb = ble_dial_hid_chr_access,
                .arg = (void *)s_pnp_id,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(0x2A19),
                .access_cb = ble_dial_hid_chr_access,
                .arg = (void *)&s_battery_level,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4B),
                .access_cb = ble_dial_hid_chr_access,
                .arg = (void *)s_hid_report_map,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4A),
                .access_cb = ble_dial_hid_chr_access,
                .arg = (void *)s_hid_info,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4E),
                .access_cb = ble_dial_hid_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4C),
                .access_cb = ble_dial_hid_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4D),
                .access_cb = ble_dial_hid_chr_access,
                .val_handle = &s_report_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .descriptors = (struct ble_gatt_dsc_def[]){
                    {
                        .uuid = BLE_UUID16_DECLARE(0x2908),
                        .att_flags = BLE_ATT_F_READ,
                        .access_cb = ble_dial_hid_dsc_access,
                        .arg = (void *)s_report_ref,
                    },
                    { 0 },
                },
            },
            { 0 },
        },
    },
    { 0 },
};

static int ble_dial_hid_dsc_access(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_DSC) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const uint8_t *value = (const uint8_t *)arg;
    return os_mbuf_append(ctxt->om, value, 2) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int ble_dial_hid_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    uint16_t uuid = ble_uuid_u16(ctxt->chr->uuid);

    if (uuid == 0x2A4B) {
        const uint8_t *map = (const uint8_t *)arg;
        return os_mbuf_append(ctxt->om, map, sizeof(s_hid_report_map)) == 0 ? 0
                                                                            : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (uuid == 0x2A4A || uuid == 0x2A50) {
        const uint8_t *value = (const uint8_t *)arg;
        size_t len = (uuid == 0x2A4A) ? sizeof(s_hid_info) : sizeof(s_pnp_id);
        return os_mbuf_append(ctxt->om, value, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (uuid == 0x2A29) {
        const char *text = (const char *)arg;
        return os_mbuf_append(ctxt->om, text, strlen(text)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (uuid == 0x2A19) {
        const uint8_t *level = (const uint8_t *)arg;
        return os_mbuf_append(ctxt->om, level, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (uuid == 0x2A4E) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            static uint8_t mode = 1;
            return os_mbuf_append(ctxt->om, &mode, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;
    }
    if (uuid == 0x2A4C) {
        return 0;
    }
    if (uuid == 0x2A4D) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            return os_mbuf_append(ctxt->om, s_last_report, sizeof(s_last_report)) == 0 ? 0
                                                                                       : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static void ble_dial_hid_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    if (!s_host_ready || !s_active) {
        return;
    }

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = 0x03C0;
    fields.appearance_is_present = 1;
    fields.name = (uint8_t *)"LX_Dial";
    fields.name_len = 7;
    fields.name_is_complete = 1;
    fields.uuids16 = (ble_uuid16_t[]){ BLE_UUID16_INIT(0x1812) };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields failed: %d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params,
                           ble_dial_hid_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    }
}

static int ble_dial_hid_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_notify_enabled = false;
            ESP_LOGI(TAG, "BLE connected");
        } else {
            ble_dial_hid_advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled = false;
        ESP_LOGI(TAG, "BLE disconnected");
        ble_dial_hid_advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_report_handle) {
            s_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "Report notify %s", s_notify_enabled ? "on" : "off");
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "Encryption %s",
                 event->enc_change.status == 0 ? "enabled" : "failed");
        break;
    default:
        break;
    }
    return 0;
}

static void ble_dial_hid_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure addr failed: %d", rc);
        return;
    }
    s_host_ready = true;
    ble_svc_gap_device_name_set("LX_Dial");
    if (s_active) {
        ble_dial_hid_advertise();
    }
}

static void ble_dial_hid_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ble_dial_hid_gatt_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_dial_hid_init(void)
{
    static bool started;

    if (started) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_dial_hid_on_sync;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ESP_ERROR_CHECK(ble_dial_hid_gatt_init());
    nimble_port_freertos_init(ble_dial_hid_host_task);
    started = true;
    ESP_LOGI(TAG, "NimBLE dial HID ready");
    return ESP_OK;
}

void ble_dial_hid_set_active(bool active)
{
    s_active = active;
    if (!s_host_ready) {
        return;
    }
    if (active) {
        ble_dial_hid_advertise();
    } else {
        ble_gap_adv_stop();
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
}

bool ble_dial_hid_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

void ble_dial_hid_send_report(uint8_t key)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_report_handle == 0 || !s_notify_enabled) {
        return;
    }

    s_last_report[0] = key;
    s_last_report[1] = (key == BLE_DIAL_KEY_L) ? 0xff : 0x00;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(s_last_report, sizeof(s_last_report));
    if (om == NULL) {
        return;
    }
    int rc = ble_gatts_notify_custom(s_conn_handle, s_report_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify failed: %d", rc);
    }
}
