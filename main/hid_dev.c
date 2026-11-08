// BLE HID peripheral (HID over GATT) that the two computers pair with.
// One combined keyboard + mouse + media-key device.
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "kvm.h"

static const char *TAG = "hid_dev";

// Report IDs in our own descriptor
#define RID_KBD  1
#define RID_MOUSE 2
#define RID_CONS 3

static const uint8_t report_map[] = {
    // Keyboard, report 1: modifiers, reserved, 6 keys; output: 5 LEDs
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, RID_KBD,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xFF, 0x00,
    0x05, 0x07, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x81, 0x00,
    0xC0,
    // Mouse, report 2: 8 buttons, X/Y int16, wheel int8, AC pan int8
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, RID_MOUSE, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x08, 0x75, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x06,
    0xC0, 0xC0,
    // Consumer control, report 3: one 16-bit usage
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, RID_CONS,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

static const uint8_t hid_info[4] = {0x11, 0x01, 0x00, 0x02}; // HID 1.11, normally connectable
static uint8_t protocol_mode = 1;                             // report protocol
static const uint8_t battery_level[1] = {100};
// PnP ID: vendor source USB-IF, Espressif VID 0x303A, PID 0x4004, version 1.0
static const uint8_t pnp_id[7] = {0x02, 0x3A, 0x30, 0x04, 0x40, 0x00, 0x01};

static const uint8_t ref_kbd_in[2]   = {RID_KBD, 1};
static const uint8_t ref_kbd_out[2]  = {RID_KBD, 2};
static const uint8_t ref_mouse_in[2] = {RID_MOUSE, 1};
static const uint8_t ref_cons_in[2]  = {RID_CONS, 1};

static uint16_t h_kbd_in, h_kbd_out, h_mouse_in, h_cons_in, h_batt;

static int append(struct ble_gatt_access_ctxt *ctxt, const void *p, size_t n)
{
    return os_mbuf_append(ctxt->om, p, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int static_read_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    // arg points to a {ptr,len} pair
    const struct { const void *p; size_t n; } *blob = arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR || ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC)
        return append(ctxt, blob->p, blob->n);
    return BLE_ATT_ERR_UNLIKELY;
}

#define BLOB(name, data) static const struct { const void *p; size_t n; } name = {data, sizeof(data)}
BLOB(b_map, report_map);
BLOB(b_info, hid_info);
BLOB(b_batt, battery_level);
BLOB(b_pnp, pnp_id);
BLOB(b_ref_kbd_in, ref_kbd_in);
BLOB(b_ref_kbd_out, ref_kbd_out);
BLOB(b_ref_mouse_in, ref_mouse_in);
BLOB(b_ref_cons_in, ref_cons_in);
static const char manuf[] = "DIY";
static const struct { const void *p; size_t n; } b_manuf = {manuf, sizeof(manuf) - 1};

static int report_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    static const uint8_t zeros[8] = {0};
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        size_t n = attr == h_mouse_in ? 7 : attr == h_cons_in ? 2 : attr == h_kbd_out ? 1 : 8;
        return append(ctxt, zeros, n);
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && attr == h_kbd_out) {
        uint8_t leds = 0;
        uint16_t len;
        ble_hs_mbuf_to_flat(ctxt->om, &leds, 1, &len);
        kvm_on_host_leds(conn, leds);
        return 0;
    }
    return 0;
}

static int proto_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return append(ctxt, &protocol_mode, 1);
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len;
        ble_hs_mbuf_to_flat(ctxt->om, &protocol_mode, 1, &len);
    }
    return 0;
}

static int ctrl_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0; // suspend / exit suspend: ignored
}

#define RD_ENC (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC)

static const struct ble_gatt_svc_def svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812), // HID
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = static_read_cb, .arg = (void *)&b_info,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = static_read_cb, .arg = (void *)&b_map,
              .flags = RD_ENC },
            { .uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = ctrl_cb,
              .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = proto_cb,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = report_cb, .val_handle = &h_kbd_in,
              .flags = RD_ENC | BLE_GATT_CHR_F_NOTIFY,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
                    .access_cb = static_read_cb, .arg = (void *)&b_ref_kbd_in }, {0} } },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = report_cb, .val_handle = &h_kbd_out,
              .flags = RD_ENC | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
                    .access_cb = static_read_cb, .arg = (void *)&b_ref_kbd_out }, {0} } },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = report_cb, .val_handle = &h_mouse_in,
              .flags = RD_ENC | BLE_GATT_CHR_F_NOTIFY,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
                    .access_cb = static_read_cb, .arg = (void *)&b_ref_mouse_in }, {0} } },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = report_cb, .val_handle = &h_cons_in,
              .flags = RD_ENC | BLE_GATT_CHR_F_NOTIFY,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
                    .access_cb = static_read_cb, .arg = (void *)&b_ref_cons_in }, {0} } },
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F), // Battery
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = static_read_cb, .arg = (void *)&b_batt,
              .val_handle = &h_batt, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A), // Device Information
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A50), .access_cb = static_read_cb, .arg = (void *)&b_pnp,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = static_read_cb, .arg = (void *)&b_manuf,
              .flags = BLE_GATT_CHR_F_READ },
            {0},
        },
    },
    {0},
};

void hid_dev_init(void)
{
    ESP_ERROR_CHECK(ble_gatts_count_cfg(svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(svcs));
}

static void notify(uint16_t conn, uint16_t handle, const void *data, size_t len)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return;
    int rc = ble_gatts_notify_custom(conn, handle, om);
    if (rc) ESP_LOGD(TAG, "notify rc=%d", rc);
}

void hid_dev_send_keyboard(uint16_t conn, const uint8_t rpt[8]) { notify(conn, h_kbd_in, rpt, 8); }
void hid_dev_send_mouse(uint16_t conn, const uint8_t rpt[7])    { notify(conn, h_mouse_in, rpt, 7); }
void hid_dev_send_consumer(uint16_t conn, uint16_t usage)
{
    uint8_t r[2] = {usage & 0xFF, usage >> 8};
    notify(conn, h_cons_in, r, 2);
}

// Exposed so the parser test can check our own descriptor.
const uint8_t *hid_dev_report_map(size_t *len) { *len = sizeof(report_map); return report_map; }
