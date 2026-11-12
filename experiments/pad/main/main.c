// KVM pad: the second ESP32 of the two-board setup. It is one computer's own Bluetooth keyboard+mouse
// ("ESP KVM", mouse appearance so macOS gives it a 7.5 ms link). The hub (the main firmware) reads the
// MX Keys / MX Master and sends their reports here over a UART; the pad forwards them to its computer.
//
// UART frame: 0xA5 | type | len | payload | xor(type,len,payload)
//   hub -> pad: 1 keyboard(8)  2 mouse(7)  3 consumer(2)  5 open pairing window  6 forget the computer
//   pad -> hub: 0x81 keyboard LEDs(1)  0x82 status{linked, known, pairing open} once a second
// Wiring: pad TX GPIO17 -> hub RX GPIO16, pad RX GPIO16 <- hub TX GPIO17, GND to GND.
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "kvm.h"

#define DEVICE_NAME  "ESP KVM"
#define LED_GPIO     GPIO_NUM_2
#define BOOT_GPIO    GPIO_NUM_0
#define UART_PAD     UART_NUM_1
#define PAD_TX_GPIO  17
#define PAD_RX_GPIO  16
#define PAD_BAUD     460800
#define PAIR_SECONDS 120

static const char *TAG = "pad";
enum { PT_KBD = 1, PT_MOUSE = 2, PT_CONS = 3, PT_PAIR = 5, PT_FORGET = 6, PT_LEDS = 0x81, PT_STATUS = 0x82 };

static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool linked;           // encrypted link to the one known computer
static bool known;
static ble_addr_t peer;
static int64_t pair_until_us;
static uint8_t own_addr_type;
static int ask_step;
static int64_t linked_us;

void ble_store_config_init(void);
static void start_adv(void);

static void kvm_nvs_put(void)
{
    nvs_handle_t h;
    if (nvs_open("pad", NVS_READWRITE, &h) != ESP_OK) return;
    if (known) nvs_set_blob(h, "peer", &peer, sizeof(peer)); else nvs_erase_key(h, "peer");
    nvs_commit(h);
    nvs_close(h);
}
static void load_peer(void)
{
    nvs_handle_t h;
    size_t n = sizeof(peer);
    if (nvs_open("pad", NVS_READONLY, &h) != ESP_OK) return;
    known = nvs_get_blob(h, "peer", &peer, &n) == ESP_OK && n == sizeof(peer);
    nvs_close(h);
}
static bool pairing_open(void) { return esp_timer_get_time() < pair_until_us; }

static void delete_peer_both(const ble_addr_t *a)
{
    ble_addr_t x = *a;
    ble_store_util_delete_peer(&x);
    if (a->type <= 1) { x.type = a->type ? 0 : 1; ble_store_util_delete_peer(&x); }
}

// ---------------------------------------------------------------- UART
static void tx_frame(uint8_t type, const uint8_t *p, uint8_t n)
{
    uint8_t f[20], chk = type ^ n;
    f[0] = 0xA5; f[1] = type; f[2] = n;
    for (int i = 0; i < n; i++) { f[3 + i] = p[i]; chk ^= p[i]; }
    f[3 + n] = chk;
    uart_write_bytes(UART_PAD, f, 4 + n);
}

void kvm_on_host_leds(uint16_t c, uint8_t leds) { tx_frame(PT_LEDS, &leds, 1); }

static void on_frame(uint8_t type, const uint8_t *b, int n)
{
    switch (type) {
    case PT_KBD:   if (n == 8 && linked) hid_dev_send_keyboard(conn, b); break;
    case PT_MOUSE: if (n == 7 && linked) hid_dev_send_mouse(conn, b); break;
    case PT_CONS:  if (n == 2 && linked) hid_dev_send_consumer(conn, b[0] | (b[1] << 8)); break;
    case PT_PAIR:
        pair_until_us = esp_timer_get_time() + (int64_t)PAIR_SECONDS * 1000000;
        ESP_LOGI(TAG, "pairing window open for %d s", PAIR_SECONDS);
        break;
    case PT_FORGET:
        if (known) delete_peer_both(&peer);
        known = false;
        kvm_nvs_put();
        if (conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        ESP_LOGW(TAG, "computer forgotten");
        break;
    }
}

static void rx_task(void *arg)
{
    uint8_t b, buf[20], type = 0, chk = 0;
    int st = 0, len = 0, i = 0;
    for (;;) {
        if (uart_read_bytes(UART_PAD, &b, 1, portMAX_DELAY) != 1) continue;
        switch (st) {
        case 0: if (b == 0xA5) st = 1; break;
        case 1: type = b; chk = b; st = 2; break;
        case 2: len = b; chk ^= b; i = 0; st = len > 16 ? 0 : len ? 3 : 4; break;
        case 3: buf[i++] = b; chk ^= b; if (i >= len) st = 4; break;
        case 4: if (b == chk) on_frame(type, buf, len); st = 0; break;
        }
    }
}

// heartbeat to the hub, LED, BOOT button (opens the pairing window too), one polite link-speed ask
static void status_task(void *arg)
{
    int last_itvl = 0, last_lat = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!gpio_get_level(BOOT_GPIO)) { pair_until_us = esp_timer_get_time() + (int64_t)PAIR_SECONDS * 1000000; ESP_LOGI(TAG, "pairing window open (BOOT)"); }
        uint8_t st[3] = { linked, known, pairing_open() };
        tx_frame(PT_STATUS, st, 3);
        gpio_set_level(LED_GPIO, linked || (pairing_open() && (esp_timer_get_time() / 300000) % 2));
        struct ble_gap_conn_desc d;
        if (linked && ble_gap_conn_find(conn, &d) == 0) {
            if (d.conn_itvl != last_itvl || d.conn_latency != last_lat) {
                last_itvl = d.conn_itvl; last_lat = d.conn_latency;
                ESP_LOGI(TAG, "link %.2f ms lat %d timeout %d ms", d.conn_itvl * 1.25, d.conn_latency, d.supervision_timeout * 10);
            }
            // The computer picks 7.5 ms with latency 44 for a mouse; ask once for latency 0 so the first move after a pause is not delayed
            if (ask_step == 0 && esp_timer_get_time() - linked_us > 8000000LL && d.conn_latency != 0) {
                struct ble_gap_upd_params up = { .itvl_min = 6, .itvl_max = 6, .latency = 0, .supervision_timeout = 400 };
                ESP_LOGI(TAG, "asking 7.5 ms, latency 0");
                ble_gap_update_params(conn, &up);
                ask_step = 1;
            }
        } else { last_itvl = 0; last_lat = -1; }
    }
}

// ---------------------------------------------------------------- BLE (peripheral only)
static int gap_cb(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            conn = ev->connect.conn_handle;
            linked = false;
            ble_gap_security_initiate(conn);
        } else start_adv();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc d;
        if (ev->enc_change.status != 0 || ble_gap_conn_find(ev->enc_change.conn_handle, &d) != 0) {
            ESP_LOGW(TAG, "encryption failed %d", ev->enc_change.status);
            ble_gap_terminate(ev->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        if (!known) {
            if (!pairing_open()) {                       // a stranger: refuse and forget it
                ESP_LOGW(TAG, "unknown computer refused (open the pairing window on the hub page first)");
                delete_peer_both(&d.peer_id_addr);
                ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
                return 0;
            }
            peer = d.peer_id_addr; known = true; kvm_nvs_put();
            pair_until_us = 0;
            ESP_LOGI(TAG, "computer paired and saved");
        } else if (memcmp(peer.val, d.peer_id_addr.val, 6) != 0) {   // bytes only: macOS flips the address type
            ESP_LOGW(TAG, "a different computer tried to connect: refused");
            delete_peer_both(&d.peer_id_addr);
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        linked = true; ask_step = 0; linked_us = esp_timer_get_time();
        ESP_LOGI(TAG, "computer connected");
        return 0;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) delete_peer_both(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io io = { .action = ev->passkey.params.action };
        if (io.action == BLE_SM_IOACT_NUMCMP) io.numcmp_accept = 1; else return 0;
        ble_sm_inject_io(ev->passkey.conn_handle, &io);
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "computer disconnected (reason 0x%x)", ev->disconnect.reason);
        conn = BLE_HS_CONN_HANDLE_NONE; linked = false;
        start_adv();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_adv();
        return 0;
    }
    return 0;
}

static void start_adv(void)
{
    if (conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) return;
    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.appearance = 0x03C2; f.appearance_is_present = 1;     // mouse: macOS gives it a 7.5 ms link (generic 0x03C0 gets 15 ms)
    f.uuids16 = &hid_uuid; f.num_uuids16 = 1; f.uuids16_is_complete = 1;
    f.name = (const uint8_t *)DEVICE_NAME; f.name_len = strlen(DEVICE_NAME); f.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&f) != 0) return;
    struct ble_gap_adv_params p = { .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                    .itvl_min = 0x30, .itvl_max = 0x60 };
    int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_cb, NULL);
    if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "adv start rc=%d", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    start_adv();
    ESP_LOGI(TAG, "Ready. Computer %s", known ? "known" : "not paired yet");
}
static void on_reset(int reason) { ESP_LOGW(TAG, "BLE host reset, reason %d", reason); }
static void host_task(void *p) { nimble_port_run(); nimble_port_freertos_deinit(); }

void app_main(void)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << BOOT_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); err = nvs_flash_init(); }
    ESP_ERROR_CHECK(err);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    load_peer();

    uart_config_t c = { .baud_rate = PAD_BAUD, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE };
    ESP_ERROR_CHECK(uart_driver_install(UART_PAD, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PAD, &c));
    ESP_ERROR_CHECK(uart_set_pin(UART_PAD, PAD_TX_GPIO, PAD_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreate(rx_task, "pad_rx", 3072, NULL, 6, NULL);
    xTaskCreate(status_task, "pad_status", 3072, NULL, 4, NULL);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    hid_dev_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_svc_gap_device_appearance_set(0x03C2);
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}
