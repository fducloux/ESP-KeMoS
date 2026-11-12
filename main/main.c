// ESP32 BLE keyboard/mouse switch
//
//   MX Keys ─┐                 ┌─> Computer 1 (BLE HID)
//            ├─BLE─> ESP32 ────┤
//   MX Master┘   (central)     └─> Computer 2 (BLE HID)
//                                  (peripheral, connected to both at once)
//
// Hotkey: Ctrl+Shift+Left Arrow = computer 1, Ctrl+Shift+Right Arrow = computer 2.
// Settings and forgetting pairings: Wi-Fi page (web.c). Press BOOT (GPIO0) to switch it on.

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "hid_parse.h"
#include "kvm.h"

// ---------------------------------------------------------------- settings
#define DEVICE_NAME       "KVM CMP2"   // name the computers see
#define RESET_GPIO        GPIO_NUM_0  // BOOT button (press = switch the Wi-Fi page on)
#define LED_GPIO          GPIO_NUM_2  // on-board LED on most 38-pin boards: lit = computer 2
#define LED1_GPIO         GPIO_NUM_25 // external LED (GPIO -> 330 ohm -> LED -> GND): lit = computer 1
#define LED2_GPIO         GPIO_NUM_23 // external LED, other side of the board: lit = computer 2
#define HK_RIGHT          0x4F        // right arrow: Ctrl+Shift+Right = computer 2
#define HK_LEFT           0x50        // left arrow:  Ctrl+Shift+Left  = computer 1
// -------------------------------------------------------------------------

#define NUM_HOSTS   2
#define NUM_INPUTS  2
#define MAX_REPORTS 12
#define NONE        BLE_HS_CONN_HANDLE_NONE

static const char *TAG = "kvm";

// Time of the last connect/disconnect/ready on any link. Link-parameter requests wait until
// things have been quiet for a while: overlapping update procedures on several links crash the
// BLE controller (r_lld_evt_update_create, LoadProhibited).
static uint32_t last_link_ms;
static int upd_tries[2];             // per computer: link-update requests sent since it connected
static void link_event(void) { last_link_ms = esp_log_timestamp(); }

typedef struct {
    uint16_t   conn;
    bool       known;            // paired before (address saved in NVS)
    ble_addr_t addr;
    bool       ready;            // discovery done, reports subscribed
    hid_desc_t desc;
    struct { uint16_t handle; uint8_t id; uint16_t cccd; } rep[MAX_REPORTS];
    int        n_rep;
    bool       cached;           // rep[], led_handle and desc are valid for cache_addr: skip discovery
    ble_addr_t cache_addr;
    uint16_t   led_handle;       // keyboard output report (Caps Lock LED)
    uint8_t    keys[32];         // pressed keyboard usages
    uint16_t   buttons;
    uint16_t   consumer;
    uint32_t   last_rx_ms;       // for disconnect diagnosis
} input_t;

typedef struct {
    uint16_t   conn;
    bool       encrypted;
    bool       known;
    ble_addr_t id;
    uint8_t    leds;
} host_t;

static input_t in[NUM_INPUTS];
static host_t  host[NUM_HOSTS];

// ---- Two-board mode: computer REMOTE_HOST is reached through a second ESP32 (the "pad", see
// experiments/pad) over a UART. The pad is that computer's own Bluetooth keyboard+mouse, so this
// board only has ONE computer link. UART wiring: hub TX GPIO17 -> pad RX GPIO16, hub RX GPIO16 <- pad TX GPIO17, GND.
#define REMOTE_HOST   0
#define DIRECT_HOST   (1 - REMOTE_HOST)
#define PAD_UART      UART_NUM_1
#define PAD_TX_GPIO   17
#define PAD_RX_GPIO   16
#define PAD_BAUD      460800
static struct { volatile bool ok, known, pair_open; volatile uint32_t seen_ms; } pad;
enum { PT_KBD = 1, PT_MOUSE = 2, PT_CONS = 3, PT_PAIR = 5, PT_FORGET = 6, PT_LEDS = 0x81, PT_STATUS = 0x82 };
static int     active;
static uint8_t own_addr_type;
static bool    connecting;

// Pairing is only possible after the user asks for it on the web page, for one slot at a time.
enum { PAIR_NONE, PAIR_HOST0, PAIR_HOST1, PAIR_IN0, PAIR_IN1 };
#define PAIR_SECONDS 120
static int     pair_target;
static int64_t pair_deadline;

static bool pairing_on(int t)
{
    if (pair_target && esp_timer_get_time() > pair_deadline) pair_target = PAIR_NONE;
    return pair_target == t;
}
static QueueHandle_t disc_q;
// Pairing code currently waiting for someone to type it (shown on the web page)
static volatile int32_t g_passkey = -1;
static char    g_passkey_for[12];
static int64_t g_passkey_time;

static void passkey_show(uint32_t code, const char *who)
{
    strlcpy(g_passkey_for, who, sizeof(g_passkey_for));
    g_passkey_time = esp_timer_get_time();
    g_passkey = code;
}
static void passkey_clear(void) { g_passkey = -1; }

void ble_store_config_init(void);
static void start_scan(void);
static void start_adv(void);

// ================================================================ NVS

static void nvs_put_addr(const char *key, const ble_addr_t *a)
{
    nvs_handle_t h;
    if (nvs_open("kvm", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, a, sizeof(*a));
    nvs_commit(h);
    nvs_close(h);
}

static bool nvs_get_addr(const char *key, ble_addr_t *a)
{
    nvs_handle_t h;
    size_t len = sizeof(*a);
    if (nvs_open("kvm", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_blob(h, key, a, &len) == ESP_OK && len == sizeof(*a);
    nvs_close(h);
    return ok;
}

static void nvs_forget_all(void)
{
    nvs_handle_t h;
    if (nvs_open("kvm", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

static void load_state(void)
{
    char key[8];
    for (int i = 0; i < NUM_INPUTS; i++) {
        in[i].conn = NONE;
        snprintf(key, sizeof(key), "in%d", i);
        in[i].known = nvs_get_addr(key, &in[i].addr);
    }
    for (int i = 0; i < NUM_HOSTS; i++) {
        host[i].conn = NONE;
        snprintf(key, sizeof(key), "host%d", i);
        host[i].known = nvs_get_addr(key, &host[i].id);
    }
}


// ================================================================ settings (NVS "kvmcfg")

kvm_cfg_t kvm_cfg;

// No-sleep per computer (see below), saved in NVS "kvmcfg" key "nosleep" (bit 0 = computer 1)
static bool nosleep[NUM_HOSTS];

static void nosleep_save(void)
{
    nvs_handle_t h;
    if (nvs_open("kvmcfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "nosleep", (nosleep[0] ? 1 : 0) | (nosleep[1] ? 2 : 0));
    nvs_commit(h);
    nvs_close(h);
}

static void cfg_defaults(void)
{
    memset(&kvm_cfg, 0, sizeof(kvm_cfg));
    strcpy(kvm_cfg.pair_prefix, "MX ");       // "" = pair any HID device
    kvm_cfg.ap_minutes = 3;
    strcpy(kvm_cfg.ap_pass, "espkvm-setup");
    strcpy(kvm_cfg.web_pass, "admin1234");
    kvm_cfg.mod_swap = 1;                      // bit 0 = computer 1 (the Mac): swap Alt and Command. Bit 1 = computer 2
    kvm_cfg.sw_btn = 0;
}

void kvm_cfg_load(void)
{
    cfg_defaults();
    nvs_handle_t h;
    if (nvs_open("kvmcfg", NVS_READWRITE, &h) != ESP_OK) return;
    size_t n = sizeof(kvm_cfg.pair_prefix);
    nvs_get_str(h, "prefix", kvm_cfg.pair_prefix, &n);
    nvs_get_u16(h, "apmin", &kvm_cfg.ap_minutes);
    if (kvm_cfg.ap_minutes == 10) kvm_cfg.ap_minutes = 3;     // 10 was the old default
    n = sizeof(kvm_cfg.ap_pass);
    nvs_get_str(h, "appass", kvm_cfg.ap_pass, &n);
    n = sizeof(kvm_cfg.web_pass);
    nvs_get_str(h, "webpass", kvm_cfg.web_pass, &n);
    nvs_get_u8(h, "modswap", &kvm_cfg.mod_swap);
    uint8_t mig = 0;                                   // one-time: turn Alt/Command swap on for computer 1 (the Mac)
    nvs_get_u8(h, "migr1", &mig);
    if (!mig) {
        kvm_cfg.mod_swap |= 1;
        nvs_set_u8(h, "modswap", kvm_cfg.mod_swap);
        nvs_set_u8(h, "migr1", 1);
        nvs_commit(h);
    }
    ESP_LOGI(TAG, "Alt/Command swap: computer 1 %s, computer 2 %s", (kvm_cfg.mod_swap & 1) ? "on" : "off", (kvm_cfg.mod_swap & 2) ? "on" : "off");
    nvs_get_u8(h, "swbtn", &kvm_cfg.sw_btn);
    uint8_t ns = 0;
    nvs_get_u8(h, "nosleep", &ns);
    nosleep[0] = ns & 1;
    nosleep[1] = ns & 2;
    nvs_close(h);
}

void kvm_cfg_save(void)
{
    nvs_handle_t h;
    if (nvs_open("kvmcfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "prefix", kvm_cfg.pair_prefix);
    nvs_set_u16(h, "apmin", kvm_cfg.ap_minutes);
    nvs_set_str(h, "appass", kvm_cfg.ap_pass);
    nvs_set_str(h, "webpass", kvm_cfg.web_pass);
    nvs_set_u8(h, "modswap", kvm_cfg.mod_swap);
    nvs_set_u8(h, "swbtn", kvm_cfg.sw_btn);
    nvs_commit(h);
    nvs_close(h);
}

void kvm_cfg_reset(void)
{
    nvs_handle_t h;
    if (nvs_open("kvmcfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    cfg_defaults();
    nosleep[0] = nosleep[1] = false;
}

// ================================================================ output side

static uint8_t last_keys[32];
static uint8_t suppressed[32];   // keys swallowed by the hotkey until released
static uint16_t last_consumer;

// No-sleep: per computer, toggled with Ctrl+Shift+mouse button 4. While on, the computer gets a
// ghost F15 key every 30 s, and its LED flashes for 100 ms every 2 s (off-flash if it is the
// active computer, on-flash if not).
static bool ns_flash;                // true during the 100 ms flash window

// Shows the active computer: LED1 = computer 1, LED2 and the on-board LED = computer 2
static void led_update(void)
{
    bool l1 = (active == 0) ^ (nosleep[0] && ns_flash);
    bool l2 = (active == 1) ^ (nosleep[1] && ns_flash);
    gpio_set_level(LED1_GPIO, l1);
    gpio_set_level(LED2_GPIO, l2);
    gpio_set_level(LED_GPIO, l2);
}

static bool host_ok(int h)
{
    if (h == REMOTE_HOST) return pad.ok;
    return host[h].conn != NONE && host[h].encrypted;
}
static bool host_known(int h) { return h == REMOTE_HOST ? pad.known : host[h].known; }

static void pad_send(uint8_t type, const uint8_t *p, uint8_t n)
{
    uint8_t f[20], chk = type ^ n;
    f[0] = 0xA5; f[1] = type; f[2] = n;
    for (int i = 0; i < n; i++) { f[3 + i] = p[i]; chk ^= p[i]; }
    f[3 + n] = chk;
    uart_write_bytes(PAD_UART, f, 4 + n);
}
static void out_kbd(int h, const uint8_t r[8])   { if (h == REMOTE_HOST) pad_send(PT_KBD, r, 8);   else hid_dev_send_keyboard(host[h].conn, r); }
static void out_mouse(int h, const uint8_t r[7]) { if (h == REMOTE_HOST) pad_send(PT_MOUSE, r, 7); else hid_dev_send_mouse(host[h].conn, r); }
static void out_cons(int h, uint16_t c)
{
    uint8_t r[2] = {c & 0xFF, c >> 8};
    if (h == REMOTE_HOST) pad_send(PT_CONS, r, 2); else hid_dev_send_consumer(host[h].conn, c);
}

static void merged_keys(uint8_t out[32])
{
    memset(out, 0, 32);
    for (int i = 0; i < NUM_INPUTS; i++)
        if (in[i].ready)
            for (int b = 0; b < 32; b++) out[b] |= in[i].keys[b];
}

static void build_kbd_report(const uint8_t keys[32], uint8_t rpt[8])
{
    memset(rpt, 0, 8);
    int n = 0;
    for (int u = 0xE0; u <= 0xE7; u++)
        if (bm_get(keys, u) && !bm_get(suppressed, u)) rpt[0] |= 1 << (u - 0xE0);
    for (int u = 4; u < 0xE0; u++) {
        if (!bm_get(keys, u) || bm_get(suppressed, u)) continue;
        if (n == 6) { memset(&rpt[2], 0x01, 6); break; }   // rollover
        rpt[2 + n++] = u;
    }
    // This keyboard sends PC-layout modifiers. A Mac wants Alt and Command exchanged.
    if (kvm_cfg.mod_swap & (1 << active)) {
        uint8_t mo = rpt[0];
        rpt[0] = (mo & 0x33) | ((mo & 0x04) << 1) | ((mo & 0x08) >> 1) | ((mo & 0x40) << 1) | ((mo & 0x80) >> 1);
    }
}

static void send_keys(void)
{
    if (!host_ok(active)) return;
    uint8_t keys[32], rpt[8];
    merged_keys(keys);
    build_kbd_report(keys, rpt);
    out_kbd(active, rpt);
}

static uint16_t btn_suppress;        // buttons swallowed by the computer-switch click until released
static volatile bool learn_btn;      // next mouse button click becomes the switch button

static uint16_t raw_buttons(void)
{
    uint16_t b = 0;
    for (int i = 0; i < NUM_INPUTS; i++) if (in[i].ready) b |= in[i].buttons;
    return b;
}

static uint16_t merged_buttons(void) { return raw_buttons() & ~btn_suppress; }

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

// Mouse motion is added up and sent as one report every 10 ms (and at once when a button
// changes). Sending every incoming report separately floods the link and makes the
// pointer lag behind or jump when the computer's connection interval is longer.
static int32_t acc_x, acc_y, acc_w, acc_p;
static uint16_t sent_buttons;
static struct ble_npl_callout mouse_co;

static uint32_t dbg_rx, dbg_tx, dbg_pmin = 999, dbg_pmax;           // diagnosis: mouse reports in / out per 3 s
static int pend_n;                       // incoming mouse reports waiting in acc_*
static uint32_t pend_ms;                 // when the first of them arrived
static void mouse_flush(void)
{
    uint16_t b = merged_buttons();
    if (!host_ok(active)) {                      // nobody to send to: drop, don't replay later
        acc_x = acc_y = acc_w = acc_p = 0;
        sent_buttons = b;
        return;
    }
    if (!acc_x && !acc_y && !acc_w && !acc_p && b == sent_buttons) return;
    pend_n = 0;
    int32_t x = clamp(acc_x, -32767, 32767), y = clamp(acc_y, -32767, 32767);
    int32_t w = clamp(acc_w, -127, 127),     p = clamp(acc_p, -127, 127);
    acc_x -= x; acc_y -= y; acc_w -= w; acc_p -= p;   // anything over the limit goes in the next report
    uint8_t r[7] = {
        b & 0xFF,
        x & 0xFF, (x >> 8) & 0xFF,
        y & 0xFF, (y >> 8) & 0xFF,
        (uint8_t)(int8_t)w,
        (uint8_t)(int8_t)p,
    };
    out_mouse(active, r);
    dbg_tx++;
    sent_buttons = b;
}

static void mouse_tick(struct ble_npl_event *ev)
{
    // Fixed 15 ms grid: re-arming "15 ms after this run" gives 15 ms + handler delay, which beats
    // against the computer's 15 ms connection events (pointer speeds up and slows down).
    static uint32_t due;
    uint32_t now = esp_log_timestamp();
    static uint32_t prev;
    if (prev) { uint32_t pd = now - prev; if (pd < dbg_pmin) dbg_pmin = pd; if (pd > dbg_pmax) dbg_pmax = pd; }
    prev = now;
    if (pend_n && now - pend_ms >= 12) mouse_flush();       // a lone leftover report
    due += 15;
    int32_t wait = (int32_t)(due - now);
    if (wait < 1 || wait > 30) { due = now + 15; wait = 15; }   // first run or a long stall: resync
    ble_npl_callout_reset(&mouse_co, ble_npl_time_ms_to_ticks32(wait));
}

static void send_mouse(int32_t x, int32_t y, int32_t wheel, int32_t pan)
{
    acc_x += x; acc_y += y; acc_w += wheel; acc_p += pan;
    // The mouse sends ~110 reports/s, a 15 ms computer link takes ~66/s. Sending all of them queues
    // up (laggy pointer); merging by elapsed time gives uneven step sizes (macOS acceleration
    // pumps). So merge in fixed pairs: every 2nd report goes out carrying both, ~55/s, evenly.
    if (active == REMOTE_HOST) { mouse_flush(); return; }       // the pad's link is 7.5 ms: forward every report 1:1
    if (!pend_n) pend_ms = esp_log_timestamp();
    pend_n++;
    if (pend_n >= 2 || merged_buttons() != sent_buttons) mouse_flush();
}

static void send_consumer_if_changed(void)
{
    uint16_t c = 0;
    for (int i = 0; i < NUM_INPUTS && !c; i++) if (in[i].ready) c = in[i].consumer;
    if (c == last_consumer) return;
    last_consumer = c;
    if (host_ok(active)) out_cons(active, c);
}

static void forward_leds(void)
{
    uint8_t leds = host[active].leds;
    for (int i = 0; i < NUM_INPUTS; i++)
        if (in[i].ready && in[i].led_handle)
            ble_gattc_write_no_rsp_flat(in[i].conn, in[i].led_handle, &leds, 1);
}

void kvm_on_host_leds(uint16_t conn, uint8_t leds)
{
    for (int h = 0; h < NUM_HOSTS; h++) {
        if (host[h].conn != conn) continue;
        host[h].leds = leds;
        if (h == active) forward_leds();
    }
}

static void switch_to(int target)
{
    if (target == active) return;
    if (host_ok(active)) {                       // let go of everything on the old computer
        static const uint8_t kz[8] = {0};
        static const uint8_t mz[7] = {0};
        out_kbd(active, kz);
        out_mouse(active, mz);
        out_cons(active, 0);
    }
    active = target;
    last_consumer = 0;
    acc_x = acc_y = acc_w = acc_p = 0;
    sent_buttons = 0;
    led_update();
    ESP_LOGI(TAG, ">>> Switched to computer %d%s", active + 1,
             host_ok(active) ? "" : " (not connected yet)");
    send_keys();
    forward_leds();
    struct ble_gap_conn_desc cd;                  // for lag diagnosis: how fast is the new computer's link?
    if (active != REMOTE_HOST && host_ok(active) && ble_gap_conn_find(host[active].conn, &cd) == 0)
        ESP_LOGI(TAG, "Computer %d link %.2f ms lat %d", active + 1, cd.conn_itvl * 1.25, cd.conn_latency);
}

static bool is_modifier(int u) { return u >= 0xE0 && u <= 0xE7; }

// Hotkey: Ctrl+Shift+Left = computer 1, Ctrl+Shift+Right = computer 2.
// Returns true if the key press should be swallowed.
static bool hotkey(int u, const uint8_t *keys)
{
    if (is_modifier(u)) return false;
    bool ctrl = bm_get(keys, 0xE0) || bm_get(keys, 0xE4);
    bool shift = bm_get(keys, 0xE1) || bm_get(keys, 0xE5);
    if (!ctrl || !shift) return false;
    int target = u == HK_LEFT ? 0 : u == HK_RIGHT ? 1 : -1;
    if (target < 0) return false;
    // Mark the arrow as swallowed *before* switching, so the new computer never sees it.
    bm_set(suppressed, u);
    switch_to(target);
    return true;
}

#define NS_BUTTON_BIT 3              // mouse button 4
#define GHOST_MOD     0x02           // Left Shift tap: counts as activity and types nothing (F13-F18 get printed by terminals)
static struct ble_npl_callout ns_co;

static bool ctrl_shift_down(void)
{
    uint8_t k[32];
    merged_keys(k);
    return (bm_get(k, 0xE0) || bm_get(k, 0xE4)) && (bm_get(k, 0xE1) || bm_get(k, 0xE5));
}

static void ns_tick(struct ble_npl_event *ev)
{
    static unsigned n;
    n++;
    ns_flash = (n % 20) == 0;                       // 100 ms every 2 s
    if (n % 300 == 0 || n % 300 == 1) {             // press, then release 100 ms later
        bool press = n % 300 == 0;
        uint8_t keys[32];
        merged_keys(keys);
        for (int h = 0; h < NUM_HOSTS; h++) {
            if (!nosleep[h] || !host_ok(h)) continue;
            if (h == active) {                      // never disturb real typing
                bool any = false;
                for (int b = 0; b < 32; b++) any |= keys[b] != 0;
                if (any) continue;
            }
            uint8_t rpt[8] = {0};
            if (press) rpt[0] = GHOST_MOD;
            out_kbd(h, rpt);
        }
    }
    led_update();
    ble_npl_callout_reset(&ns_co, ble_npl_time_ms_to_ticks32(100));
}

static void process_keys(void)
{
    uint8_t now[32];
    merged_keys(now);
    for (int u = 0; u < 256; u++) {
        bool was = bm_get(last_keys, u), is = bm_get(now, u);
        if (was && !is) suppressed[u >> 3] &= ~(1 << (u & 7));
        if (!was && is && hotkey(u, now)) bm_set(suppressed, u);
    }
    memcpy(last_keys, now, 32);
    send_keys();
}

// ================================================================ input side: reports

static void on_input_report(input_t *d, uint16_t handle, const uint8_t *data, size_t len)
{
    if (d == &in[1]) dbg_rx++;
    d->last_rx_ms = esp_log_timestamp();
    int id = -1;
    for (int i = 0; i < d->n_rep; i++) if (d->rep[i].handle == handle) id = d->rep[i].id;
    if (id < 0) return;

    hid_input_t r;
    hid_decode_input(&d->desc, id, data, len, &r);
    if (r.has_keys) {
        for (int b = 0; b < 32; b++)
            d->keys[b] = (d->keys[b] & ~r.key_cover[b]) | (r.key_down[b] & r.key_cover[b]);
        process_keys();
    }
    if (r.has_mouse) {
        uint16_t newly = r.buttons & ~d->buttons;
        d->buttons = r.buttons;
        if (learn_btn && newly) {
            int bit = __builtin_ctz(newly);
            kvm_cfg.sw_btn = bit + 1;
            kvm_cfg_save();
            learn_btn = false;
            btn_suppress |= 1u << bit;
            ESP_LOGW(TAG, "Switch button learned: mouse button %d", bit + 1);
        } else if ((newly >> NS_BUTTON_BIT & 1) && kvm_cfg.sw_btn != NS_BUTTON_BIT + 1 && ctrl_shift_down()) {
            btn_suppress |= 1u << NS_BUTTON_BIT;
            nosleep[active] = !nosleep[active];
            nosleep_save();
            ESP_LOGI(TAG, "No-sleep computer %d: %s", active + 1, nosleep[active] ? "on" : "off");
        } else if (kvm_cfg.sw_btn && ((newly >> (kvm_cfg.sw_btn - 1)) & 1)) {
            uint8_t k[32];
            merged_keys(k);
            bool ctrl = bm_get(k, 0xE0) || bm_get(k, 0xE4);
            bool shift = bm_get(k, 0xE1) || bm_get(k, 0xE5);
            if (ctrl && shift) {
                btn_suppress |= 1u << (kvm_cfg.sw_btn - 1);
                switch_to(1 - active);
            }
        }
        btn_suppress &= raw_buttons();
        send_mouse(r.x, r.y, r.wheel, r.pan);
    }
    if (r.has_consumer) {
        d->consumer = r.consumer;
        send_consumer_if_changed();
    }
}

// ================================================================ input side: GATT discovery
// Runs in its own task and waits on each NimBLE callback, which keeps it readable.

static SemaphoreHandle_t gsem;
static int g_status;
static uint16_t svc_start, svc_end;
typedef struct { uint16_t def, val, uuid; uint8_t props; } chr_t;
static chr_t chrs[32];
static int n_chrs;
typedef struct { uint16_t handle, uuid; } dsc_t;
static dsc_t dscs[8];
static int n_dscs;
static uint8_t rbuf[1024];
static uint16_t rlen;

static void done(int status) { g_status = status == BLE_HS_EDONE ? 0 : status; xSemaphoreGive(gsem); }

static int wait_gatt(int rc)
{
    if (rc) return rc;
    if (xSemaphoreTake(gsem, pdMS_TO_TICKS(10000)) != pdTRUE) return BLE_HS_ETIMEOUT;
    return g_status;
}

static int svc_cb(uint16_t c, const struct ble_gatt_error *e, const struct ble_gatt_svc *s, void *a)
{
    if (e->status == 0 && s) {
        if (!svc_start) { svc_start = s->start_handle; svc_end = s->end_handle; }
        return 0;
    }
    done(e->status);
    return 0;
}

static int chr_cb(uint16_t c, const struct ble_gatt_error *e, const struct ble_gatt_chr *ch, void *a)
{
    if (e->status == 0 && ch) {
        if (n_chrs < 32)
            chrs[n_chrs++] = (chr_t){ ch->def_handle, ch->val_handle, ble_uuid_u16(&ch->uuid.u), ch->properties };
        return 0;
    }
    done(e->status);
    return 0;
}

static int dsc_cb(uint16_t c, const struct ble_gatt_error *e, uint16_t chr_val, const struct ble_gatt_dsc *ds, void *a)
{
    if (e->status == 0 && ds) {
        if (n_dscs < 8) dscs[n_dscs++] = (dsc_t){ ds->handle, ble_uuid_u16(&ds->uuid.u) };
        return 0;
    }
    done(e->status);
    return 0;
}

static int read_long_cb(uint16_t c, const struct ble_gatt_error *e, struct ble_gatt_attr *attr, void *a)
{
    if (e->status == 0 && attr) {
        uint16_t n = OS_MBUF_PKTLEN(attr->om);
        if (attr->offset + n <= sizeof(rbuf)) {
            os_mbuf_copydata(attr->om, 0, n, rbuf + attr->offset);
            rlen = attr->offset + n;
        }
        return 0;
    }
    done(e->status);
    return 0;
}

static int read_cb(uint16_t c, const struct ble_gatt_error *e, struct ble_gatt_attr *attr, void *a)
{
    if (e->status == 0 && attr) {
        rlen = OS_MBUF_PKTLEN(attr->om);
        if (rlen > sizeof(rbuf)) rlen = sizeof(rbuf);
        os_mbuf_copydata(attr->om, 0, rlen, rbuf);
    }
    done(e->status);
    return 0;
}

static int write_cb(uint16_t c, const struct ble_gatt_error *e, struct ble_gatt_attr *attr, void *a)
{
    done(e->status);
    return 0;
}

static int discover(input_t *d)
{
    uint16_t conn = d->conn, map_handle = 0;
    int rc;
    xSemaphoreTake(gsem, 0);

    // Same device as last time: the handles are still valid, only re-enable the notifications.
    struct ble_gap_conn_desc cd;
    if (d->cached && ble_gap_conn_find(conn, &cd) == 0 && ble_addr_cmp(&cd.peer_id_addr, &d->cache_addr) == 0) {
        static const uint8_t on[2] = {1, 0};
        bool ok = true;
        for (int i = 0; i < d->n_rep && ok; i++)
            ok = wait_gatt(ble_gattc_write_flat(conn, d->rep[i].cccd, on, 2, write_cb, NULL)) == 0;
        if (ok) { ESP_LOGI(TAG, "Input device: using cached setup"); return 0; }
        ESP_LOGW(TAG, "cached setup failed, full discovery");
    }
    d->cached = false;

    svc_start = svc_end = 0;
    rc = wait_gatt(ble_gattc_disc_svc_by_uuid(conn, BLE_UUID16_DECLARE(0x1812), svc_cb, NULL));
    if (rc || !svc_start) { ESP_LOGE(TAG, "no HID service (%d)", rc); return rc ? rc : -1; }

    n_chrs = 0;
    rc = wait_gatt(ble_gattc_disc_all_chrs(conn, svc_start, svc_end, chr_cb, NULL));
    if (rc) return rc;

    for (int i = 0; i < n_chrs; i++)
        if (chrs[i].uuid == 0x2A4B) map_handle = chrs[i].val;
    if (!map_handle) { ESP_LOGE(TAG, "no report map"); return -1; }

    rlen = 0;
    rc = wait_gatt(ble_gattc_read_long(conn, map_handle, 0, read_long_cb, NULL));
    if (rc) { ESP_LOGE(TAG, "report map read failed (%d)", rc); return rc; }
    hid_parse_descriptor(rbuf, rlen, &d->desc);

    d->n_rep = 0;
    d->led_handle = 0;
    for (int i = 0; i < n_chrs; i++) {
        if (chrs[i].uuid != 0x2A4D) continue;
        uint16_t end = i + 1 < n_chrs ? chrs[i + 1].def - 1 : svc_end;
        n_dscs = 0;
        if (end > chrs[i].val) {
            rc = wait_gatt(ble_gattc_disc_all_dscs(conn, chrs[i].val, end, dsc_cb, NULL));
            if (rc) return rc;
        }
        uint8_t rid = 0, type = 0;
        uint16_t cccd = 0;
        for (int k = 0; k < n_dscs; k++) {
            if (dscs[k].uuid == 0x2908) {
                rlen = 0;
                if (wait_gatt(ble_gattc_read(conn, dscs[k].handle, read_cb, NULL)) == 0 && rlen >= 2) {
                    rid = rbuf[0];
                    type = rbuf[1];
                }
            } else if (dscs[k].uuid == 0x2902) {
                cccd = dscs[k].handle;
            }
        }
        if (type == 1 && cccd && (chrs[i].props & BLE_GATT_CHR_PROP_NOTIFY) && d->n_rep < MAX_REPORTS) {
            static const uint8_t on[2] = {1, 0};
            rc = wait_gatt(ble_gattc_write_flat(conn, cccd, on, 2, write_cb, NULL));
            if (rc == 0) {
                d->rep[d->n_rep].handle = chrs[i].val;
                d->rep[d->n_rep].id = rid;
                d->rep[d->n_rep].cccd = cccd;
                d->n_rep++;
            }
        } else if (type == 2 && !d->led_handle) {
            d->led_handle = chrs[i].val;
        }
    }
    if (d->n_rep && ble_gap_conn_find(conn, &cd) == 0) {
        d->cache_addr = cd.peer_id_addr;
        d->cached = false;   // cache disabled: instant setup overlapped the mouse's own link update (controller crash)
    }
    return d->n_rep ? 0 : -1;
}

static void disc_task(void *arg)
{
    int idx;
    for (;;) {
        xQueueReceive(disc_q, &idx, portMAX_DELAY);
        input_t *d = &in[idx];
        if (d->conn == NONE) continue;
        if (discover(d) == 0) {
            memset(d->keys, 0, sizeof(d->keys));
            d->buttons = d->consumer = 0;
            d->ready = true;
            // No link update request here: the devices pick their own power-save timing anyway, and an
            // update procedure overlapping the computers' updates crashes the controller.
            link_event();
            ESP_LOGI(TAG, "Input device %d ready (%d reports)", idx + 1, d->n_rep);
            forward_leds();
        } else {
            ESP_LOGE(TAG, "Input device %d: setup failed, disconnecting", idx + 1);
            ble_gap_terminate(d->conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
}

// ================================================================ input side: GAP (central)

// Delete a peer's bond under both address types (the same device can show up as public or random)
static void delete_peer_both(const ble_addr_t *a)
{
    ble_addr_t x = *a;
    ble_store_util_delete_peer(&x);
    if (a->type <= 1) {
        x.type = a->type ? 0 : 1;
        ble_store_util_delete_peer(&x);
    }
}

static int repeat_pairing(uint16_t conn)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) == 0) {
        ESP_LOGW(TAG, "REPEAT PAIRING from %02x:%02x:%02x:%02x:%02x:%02x (type %d): deleting its bond",
                 desc.peer_id_addr.val[5], desc.peer_id_addr.val[4], desc.peer_id_addr.val[3],
                 desc.peer_id_addr.val[2], desc.peer_id_addr.val[1], desc.peer_id_addr.val[0],
                 desc.peer_id_addr.type);
        delete_peer_both(&desc.peer_id_addr);
    }
    return BLE_GAP_REPEAT_PAIRING_RETRY;
}

static int central_cb(struct ble_gap_event *ev, void *arg)
{
    input_t *d = arg;
    int idx = d - in;
    struct ble_gap_conn_desc desc;

    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        link_event();
        connecting = false;
        if (ev->connect.status == 0) {
            d->conn = ev->connect.conn_handle;
            d->ready = false;
            // A keyboard wants a passkey typed on it, so we act as "display only"
            // while pairing a new input device; computers pair with "Just Works".
            if (!d->known) ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
            ble_gap_security_initiate(d->conn);
        }
        start_scan();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
        passkey_clear();
        if (ev->enc_change.status != 0) {
            ESP_LOGW(TAG, "Input device %d: encryption failed (%d)", idx + 1, ev->enc_change.status);
            ble_gap_terminate(d->conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        if (!d->known && ble_gap_conn_find(d->conn, &desc) == 0) {
            char key[8];
            snprintf(key, sizeof(key), "in%d", idx);
            d->addr = desc.peer_id_addr;
            d->known = true;
            nvs_put_addr(key, &d->addr);
            ESP_LOGI(TAG, "Input device %d paired and saved", idx + 1);
            if (pair_target == PAIR_IN0 + idx) pair_target = PAIR_NONE;
        }
        xQueueSend(disc_q, &idx, 0);
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io io = { .action = ev->passkey.params.action };
        if (io.action == BLE_SM_IOACT_DISP) {
            io.passkey = esp_random() % 1000000;
            passkey_show(io.passkey, "keyboard");
            printf("\n*************************************************\n"
                   "  On the keyboard, type  %06" PRIu32 "  then press Enter\n"
                   "*************************************************\n\n", io.passkey);
        } else if (io.action == BLE_SM_IOACT_NUMCMP) {
            io.numcmp_accept = 1;
        } else {
            return 0;
        }
        ble_sm_inject_io(ev->passkey.conn_handle, &io);
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        return repeat_pairing(ev->repeat_pairing.conn_handle);

    case BLE_GAP_EVENT_CONN_UPDATE_REQ: {
        // The keyboard/mouse ask for slow, sleepy link settings to save battery. That adds
        // delay to the first key or move after a pause, so ask for fast settings instead.
        struct ble_gap_upd_params *sp = ev->conn_update_req.self_params;
        *sp = *ev->conn_update_req.peer_params;
        if (sp->itvl_max > 12) sp->itvl_max = 12;                 // 15 ms
        if (sp->itvl_min > sp->itvl_max) sp->itvl_min = sp->itvl_max;
        sp->latency = 0;
        sp->supervision_timeout = 400;
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (d->ready && !ev->notify_rx.indication) {
            uint8_t buf[64];
            uint16_t len = 0;
            if (ble_hs_mbuf_to_flat(ev->notify_rx.om, buf, sizeof(buf), &len) == 0)
                on_input_report(d, ev->notify_rx.attr_handle, buf, len);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        link_event();
        ESP_LOGI(TAG, "Input device %d disconnected (reason 0x%x) link %.2f ms lat %d timeout %d ms, last report %u ms ago",
                 idx + 1, ev->disconnect.reason, ev->disconnect.conn.conn_itvl * 1.25,
                 ev->disconnect.conn.conn_latency, ev->disconnect.conn.supervision_timeout * 10,
                 (unsigned)(esp_log_timestamp() - d->last_rx_ms));
        // A failed keyboard pairing must not leave computers being asked for a PIN
        ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
        passkey_clear();
        d->conn = NONE;
        d->ready = false;
        memset(d->keys, 0, sizeof(d->keys));
        d->buttons = d->consumer = 0;
        process_keys();               // release anything it was holding
        send_mouse(0, 0, 0, 0);
        send_consumer_if_changed();
        start_scan();
        return 0;
    }
    return 0;
}

static bool name_matches(const struct ble_hs_adv_fields *f)
{
    size_t n = strlen(kvm_cfg.pair_prefix);
    if (n == 0) return true;
    return f->name && f->name_len >= n && memcmp(f->name, kvm_cfg.pair_prefix, n) == 0;
}

static void connect_to(int slot, const ble_addr_t *addr)
{
    ble_gap_disc_cancel();
    struct ble_gap_conn_params p = {
        .scan_itvl = 0x10, .scan_window = 0x10,
        .itvl_min = 6, .itvl_max = 12,            // 7.5–15 ms
        .latency = 0, .supervision_timeout = 400, // 4 s
    };
    if (ble_gap_connect(own_addr_type, addr, 10000, &p, central_cb, &in[slot]) == 0)
        connecting = true;
    else
        start_scan();
}

static int scan_cb(struct ble_gap_event *ev, void *arg)
{
    if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) { start_scan(); return 0; }
    if (ev->type != BLE_GAP_EVENT_DISC || connecting) return 0;

    const struct ble_gap_disc_desc *dd = &ev->disc;
    if (dd->event_type == BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND ||
        dd->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND) return 0;

    // Known device coming back?
    for (int i = 0; i < NUM_INPUTS; i++) {
        if (in[i].known && in[i].conn == NONE && ble_addr_cmp(&in[i].addr, &dd->addr) == 0) {
            connect_to(i, &dd->addr);
            return 0;
        }
        if (in[i].known && ble_addr_cmp(&in[i].addr, &dd->addr) == 0) return 0; // already connected
    }

    // New device: only while the page's Pair button for that slot is active
    int slot = pairing_on(PAIR_IN0) ? 0 : pairing_on(PAIR_IN1) ? 1 : -1;
    if (slot < 0 || in[slot].known || in[slot].conn != NONE) return 0;

    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, dd->data, dd->length_data) != 0) return 0;
    if (!name_matches(&f)) return 0;
    // slot 0 is the keyboard, slot 1 the mouse: skip a device that says it is the other kind
    if (f.appearance_is_present && ((slot == 0 && f.appearance == 0x03C2) || (slot == 1 && f.appearance == 0x03C1))) return 0;
    bool hid = f.appearance_is_present && (f.appearance >= 0x03C0 && f.appearance <= 0x03CF);
    for (int i = 0; i < f.num_uuids16; i++) if (f.uuids16[i].value == 0x1812) hid = true;
    if (!hid && dd->event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) return 0;

    ESP_LOGI(TAG, "Pairing new input device: %.*s", f.name_len, (const char *)f.name);
    connect_to(slot, &dd->addr);
    return 0;
}

static void start_scan(void)
{
    // Let the computers finish their own link changes after boot first: setting up all four links
    // at once crashes the controller (r_lld_evt_update_create). upd_tick() retries after this.
    if (esp_log_timestamp() < 15000) return;
    if (connecting) return;
    bool need = false;
    // Scan to bring back known devices, or to find a new one while its Pair button is active
    for (int i = 0; i < NUM_INPUTS; i++)
        if (in[i].conn == NONE && (in[i].known || pairing_on(PAIR_IN0 + i))) need = true;
    if (!need) { ble_gap_disc_cancel(); return; }
    if (ble_gap_disc_active()) return;

    struct ble_gap_disc_params p = {
        .itvl = 0x100, .window = 0x20,  // 160 ms / 20 ms: ~12% radio time, leaves room for the connections
        .passive = 0, .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &p, scan_cb, NULL);
    if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "scan start rc=%d", rc);
}

// ================================================================ host side: GAP (peripheral)

static void assign_host(uint16_t conn)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) != 0) return;

    int h = -1;
    for (int i = 0; i < NUM_HOSTS; i++)
        if (i != REMOTE_HOST && host[i].known && memcmp(host[i].id.val, desc.peer_id_addr.val, 6) == 0) h = i;   // bytes only: macOS flips the address type
    if (h < 0) {                                  // a computer we do not know
        int t = pairing_on(PAIR_HOST0 + DIRECT_HOST) ? DIRECT_HOST : -1;
        if (t < 0 || host[t].known) {
            ESP_LOGW(TAG, "Unknown computer refused: press Pair for a computer on the setup page first");
            delete_peer_both(&desc.peer_id_addr);
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            return;
        }
        char key[8];
        snprintf(key, sizeof(key), "host%d", t);
        host[t].known = true;
        host[t].id = desc.peer_id_addr;
        nvs_put_addr(key, &host[t].id);
        h = t;
        pair_target = PAIR_NONE;
    }
    host[h].conn = conn;
    host[h].encrypted = true;
    ESP_LOGI(TAG, "Computer %d connected%s", h + 1, h == active ? " (active)" : "");

    // No link update request here: upd_tick() asks once things have been quiet for a while.
    link_event();
    upd_tries[h] = 0;
    if (h == active) send_keys();
}

// Some computers (macOS) connect with the same address bytes but a different address type
// (public vs random) than the one their bond was saved under. NimBLE then finds no key.
// If a bond exists under the other type, copy it (keys and subscriptions) to this type.
struct cccd_copy { ble_addr_t from, to; struct ble_store_value_cccd v[12]; int n; };

static int cccd_collect(int obj_type, union ble_store_value *val, void *cookie)
{
    struct cccd_copy *c = cookie;
    if (ble_addr_cmp(&val->cccd.peer_addr, &c->from) == 0 && c->n < 12) c->v[c->n++] = val->cccd;
    return 0;
}

static void fix_bond_type(const ble_addr_t *a)
{
    if (a->type > 1) return;
    ble_addr_t alt = *a;
    alt.type = a->type ? 0 : 1;
    struct ble_store_key_sec k = { .peer_addr = *a };
    struct ble_store_value_sec v;
    if (ble_store_read_our_sec(&k, &v) == 0) return;               // already there
    k.peer_addr = alt;
    if (ble_store_read_our_sec(&k, &v) != 0) return;               // nothing to copy
    v.peer_addr = *a;
    ble_store_write_our_sec(&v);
    k.peer_addr = alt;
    if (ble_store_read_peer_sec(&k, &v) == 0) { v.peer_addr = *a; ble_store_write_peer_sec(&v); }
    static struct cccd_copy cc;
    cc.from = alt; cc.to = *a; cc.n = 0;
    ble_store_iterate(BLE_STORE_OBJ_TYPE_CCCD, cccd_collect, &cc);
    for (int i = 0; i < cc.n; i++) { cc.v[i].peer_addr = *a; ble_store_write_cccd(&cc.v[i]); }
    ESP_LOGW(TAG, "Bond copied from address type %d to type %d (%d subscriptions)", alt.type, a->type, cc.n);
}

static int periph_cb(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        link_event();
        if (ev->connect.status == 0) {
            struct ble_gap_conn_desc cd;
            if (ble_gap_conn_find(ev->connect.conn_handle, &cd) == 0) fix_bond_type(&cd.peer_id_addr);
            ble_gap_security_initiate(ev->connect.conn_handle);
        }
        start_adv();   // keep advertising for the other computer
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        passkey_clear();
        if (ev->enc_change.status != 0)
            ESP_LOGW(TAG, "Computer link: encryption failed, status %d (0x%x)", ev->enc_change.status,
                     ev->enc_change.status);
        if (ev->enc_change.status == 0) assign_host(ev->enc_change.conn_handle);
        else ble_gap_terminate(ev->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        return repeat_pairing(ev->repeat_pairing.conn_handle);

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io io = { .action = ev->passkey.params.action };
        if (io.action == BLE_SM_IOACT_NUMCMP) {
            io.numcmp_accept = 1;
        } else if (io.action == BLE_SM_IOACT_DISP) {
            // The computer asks for a PIN: show it on the web page and in the log
            io.passkey = esp_random() % 1000000;
            passkey_show(io.passkey, "computer");
            printf("\n*** Computer asks for a PIN: %06" PRIu32 " ***\n\n", io.passkey);
        } else {
            return 0;
        }
        ble_sm_inject_io(ev->passkey.conn_handle, &io);
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        link_event();
        for (int h = 0; h < NUM_HOSTS; h++) {
            if (host[h].conn == ev->disconnect.conn.conn_handle) {
                host[h].conn = NONE;
                host[h].encrypted = false;
                ESP_LOGI(TAG, "Computer %d disconnected", h + 1);
            }
        }
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
    int connected = 0;
    if (host[DIRECT_HOST].conn != NONE) connected++;
    if (connected >= 1 || ble_gap_adv_active()) return;          // one direct computer only

    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.appearance = 0x03C0;
    f.appearance_is_present = 1;
    f.uuids16 = &hid_uuid;
    f.num_uuids16 = 1;
    f.uuids16_is_complete = 1;
    f.name = (const uint8_t *)DEVICE_NAME;
    f.name_len = strlen(DEVICE_NAME);
    f.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&f) != 0) return;

    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = connected ? 0xA0 : 0x30,     // fast (30–60 ms) until one computer is connected,
        .itvl_max = connected ? 0xF0 : 0x60,     // then 100–150 ms so it steals less radio time
    };
    int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, periph_cb, NULL);
    if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "adv start rc=%d", rc);
    else if (!rc) ESP_LOGI(TAG, "Advertising as \"%s\"", DEVICE_NAME);
}

// ================================================================ actions for the web page

static void addr_str(const ble_addr_t *a, char *out)
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}

void kvm_get_status(kvm_status_t *s)
{
    memset(s, 0, sizeof(*s));
    s->active = active;
    s->learning = learn_btn;
    pairing_on(-1);                                // expire an old pairing window
    s->pairing = pair_target;
    s->pair_left = pair_target ? (int)((pair_deadline - esp_timer_get_time()) / 1000000) : 0;
    if (g_passkey >= 0 && esp_timer_get_time() - g_passkey_time > 120LL * 1000000) passkey_clear();
    s->passkey = g_passkey;
    strlcpy(s->passkey_for, g_passkey_for, sizeof(s->passkey_for));
    for (int i = 0; i < NUM_HOSTS; i++) {
        s->host[i].known = host_known(i);
        s->host[i].connected = host_ok(i);
        s->host[i].ready = host_ok(i);
        if (host[i].known) addr_str(&host[i].id, s->host[i].addr);
    }
    for (int i = 0; i < NUM_INPUTS; i++) {
        s->input[i].known = in[i].known;
        s->input[i].connected = in[i].conn != NONE;
        s->input[i].ready = in[i].ready;
        if (in[i].known) addr_str(&in[i].addr, s->input[i].addr);
    }
}

bool kvm_pair(int target)
{
    if (target == PAIR_NONE) { pair_target = PAIR_NONE; start_scan(); return true; }
    if (target < PAIR_HOST0 || target > PAIR_IN1) return false;
    bool known = target == PAIR_HOST0 ? host_known(0) : target == PAIR_HOST1 ? host_known(1)
               : target == PAIR_IN0 ? in[0].known : in[1].known;
    ESP_LOGI(TAG, "Pair request %d: %s", target, known ? "refused, already paired (forget it first)" : "window open");
    if (known) return false;                      // forget it first
    pair_target = target;
    pair_deadline = esp_timer_get_time() + (int64_t)PAIR_SECONDS * 1000000;
    if (target == PAIR_HOST0 + REMOTE_HOST) pad_send(PT_PAIR, NULL, 0);     // the pad opens its own pairing window
    if (target == PAIR_HOST0 + DIRECT_HOST) {      // make sure the computer can find us: restart advertising
        ESP_LOGI(TAG, "Advertising was %s: restarting it", ble_gap_adv_active() ? "on" : "OFF");
        ble_gap_adv_stop();
        start_adv();
    }
    start_scan();
    return true;
}

void kvm_learn_button(bool on)
{
    if (!on) kvm_cfg.sw_btn = 0, kvm_cfg_save();
    learn_btn = on;
}

void kvm_switch(int target)
{
    if (target >= 0 && target < NUM_HOSTS) switch_to(target);
}

static void kvm_nvs_erase(const char *key)
{
    nvs_handle_t h;
    if (nvs_open("kvm", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

void kvm_forget_host(int h)
{
    if (h < 0 || h >= NUM_HOSTS) return;
    if (h == REMOTE_HOST) { pad_send(PT_FORGET, NULL, 0); pad.known = false; ESP_LOGW(TAG, "Computer %d (pad) forgotten", h + 1); return; }
    char key[8];
    snprintf(key, sizeof(key), "host%d", h);
    if (host[h].known) delete_peer_both(&host[h].id);
    kvm_nvs_erase(key);
    host[h].known = false;
    if (host[h].conn != NONE) ble_gap_terminate(host[h].conn, BLE_ERR_REM_USER_CONN_TERM);
    ESP_LOGW(TAG, "Computer %d forgotten", h + 1);
}

// Swap which computer is "1" and which is "2" (no re-pairing). The same physical computer stays active.
void kvm_swap_hosts(void)
{
    ESP_LOGW(TAG, "Swap is not available in two-board mode");
    return;
    host_t t = host[0]; host[0] = host[1]; host[1] = t;
    active = 1 - active;
    kvm_cfg.mod_swap = ((kvm_cfg.mod_swap & 1) << 1) | ((kvm_cfg.mod_swap >> 1) & 1);   // setting follows the computer
    bool nt = nosleep[0]; nosleep[0] = nosleep[1]; nosleep[1] = nt;
    kvm_cfg_save();
    nosleep_save();
    led_update();
    for (int i = 0; i < NUM_HOSTS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "host%d", i);
        if (host[i].known) nvs_put_addr(key, &host[i].id); else kvm_nvs_erase(key);
    }
    ESP_LOGW(TAG, "Computers 1 and 2 swapped");
}

void kvm_forget_input(int i)
{
    if (i < 0 || i >= NUM_INPUTS) return;
    char key[8];
    snprintf(key, sizeof(key), "in%d", i);
    if (in[i].known) delete_peer_both(&in[i].addr);
    kvm_nvs_erase(key);
    in[i].known = false;
    if (in[i].conn != NONE) ble_gap_terminate(in[i].conn, BLE_ERR_REM_USER_CONN_TERM);
    start_scan();
    ESP_LOGW(TAG, "Input device %d forgotten", i + 1);
}

void kvm_forget_all(void)
{
    pad_send(PT_FORGET, NULL, 0);
    for (int h = 0; h < NUM_HOSTS; h++)
        if (host[h].conn != NONE) ble_gap_terminate(host[h].conn, BLE_ERR_REM_USER_CONN_TERM);
    for (int i = 0; i < NUM_INPUTS; i++)
        if (in[i].conn != NONE) ble_gap_terminate(in[i].conn, BLE_ERR_REM_USER_CONN_TERM);
    ble_store_clear();
    nvs_forget_all();
    ESP_LOGW(TAG, "All pairings forgotten");
}

// ================================================================ startup

static struct ble_npl_callout upd_co;

// Every 3 s: any computer link that skips events (latency) or is slower than 15 ms gets a new request.
// The first request at connect time often collides with the computer's own update.
// macOS picks the top of any range it is offered, so ask for exactly 15 ms three times; if it
// refuses, stop trying (a link that has latency 0 is fine at up to 30 ms).
static void upd_tick(struct ble_npl_event *ev)
{
    if (pad.ok && esp_log_timestamp() - pad.seen_ms > 3500) { pad.ok = false; ESP_LOGW(TAG, "Pad: no heartbeat, marked offline"); }
    int was_pairing = pair_target;
    pairing_on(-1);
    if (was_pairing && !pair_target) start_scan();      // pairing window ran out
    else start_scan();                                   // no-op unless a known device is missing
    if (dbg_rx || dbg_tx) {
        struct ble_gap_conn_desc md;
        if (in[1].conn != NONE && ble_gap_conn_find(in[1].conn, &md) == 0)
            ESP_LOGI(TAG, "mouse in %u out %u (3 s), mouse link %.2f ms lat %d, tick %u..%u ms", (unsigned)dbg_rx, (unsigned)dbg_tx,
                     md.conn_itvl * 1.25, md.conn_latency, (unsigned)dbg_pmin, (unsigned)dbg_pmax);
        dbg_rx = dbg_tx = 0; dbg_pmin = 999; dbg_pmax = 0;
    }
    for (int h = 0; h < NUM_HOSTS; h++) {                // diagnosis: log every change of a computer's link
        static int lastv[NUM_HOSTS][2];
        struct ble_gap_conn_desc cd;
        if (!host_ok(h) || ble_gap_conn_find(host[h].conn, &cd) != 0) { lastv[h][0] = 0; continue; }
        if (cd.conn_itvl != lastv[h][0] || cd.conn_latency != lastv[h][1]) {
            lastv[h][0] = cd.conn_itvl; lastv[h][1] = cd.conn_latency;
            ESP_LOGI(TAG, "Computer %d link now %.2f ms lat %d", h + 1, cd.conn_itvl * 1.25, cd.conn_latency);
        }
    }
    // Wait until no link has connected, disconnected or finished setup for 10 s (Windows changes
    // its own link timing a few seconds after connecting; an update on the other link at the same
    // time crashes the controller), then send at most ONE request per tick.
    // Also not before 30 s after boot: the input devices connect from 15 s on, and their setup must not overlap an update.
    if (esp_log_timestamp() >= 30000 && esp_log_timestamp() - last_link_ms >= 10000) {
        bool sent = false;
        for (int h = 0; h < NUM_HOSTS && !sent; h++) {
            struct ble_gap_conn_desc d;
            if (!host_ok(h) || ble_gap_conn_find(host[h].conn, &d) != 0) { upd_tries[h] = 0; continue; }
            bool fast = d.conn_latency == 0 && d.conn_itvl <= 12;
            bool usable = d.conn_latency == 0 && d.conn_itvl <= 24;
            if (fast || (usable && upd_tries[h] >= 3)) continue;
            struct ble_gap_upd_params up = { .itvl_min = 12, .itvl_max = upd_tries[h] < 3 ? 12 : 24,
                                             .latency = 0, .supervision_timeout = 400 };
            ESP_LOGI(TAG, "Computer %d link %.2f ms lat %d -> asking %.2f ms", h + 1, d.conn_itvl * 1.25, d.conn_latency, up.itvl_min * 1.25);
            ble_gap_update_params(host[h].conn, &up);
            upd_tries[h]++;
            sent = true;
        }
    }
    ble_npl_callout_reset(&upd_co, ble_npl_time_ms_to_ticks32(3000));
}

static void on_sync(void)
{
    ble_npl_callout_init(&upd_co, nimble_port_get_dflt_eventq(), upd_tick, NULL);
    ble_npl_callout_reset(&upd_co, ble_npl_time_ms_to_ticks32(3000));
    ble_npl_callout_init(&ns_co, nimble_port_get_dflt_eventq(), ns_tick, NULL);
    ble_npl_callout_reset(&ns_co, ble_npl_time_ms_to_ticks32(100));
    ble_npl_callout_init(&mouse_co, nimble_port_get_dflt_eventq(), mouse_tick, NULL);
    ble_npl_callout_reset(&mouse_co, ble_npl_time_ms_to_ticks32(15));
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    start_adv();
    start_scan();
    ESP_LOGI(TAG, "Ready. Active computer: %d", active + 1);
}

static void on_reset(int reason) { ESP_LOGW(TAG, "BLE host reset, reason %d", reason); }

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void pad_handle(uint8_t type, const uint8_t *b, int n)
{
    if (type == PT_STATUS && n >= 3) {
        bool ok = b[0], known = b[1];
        if (ok != pad.ok) ESP_LOGI(TAG, "Pad: computer link %s", ok ? "up" : "down");
        pad.ok = ok; pad.known = known; pad.pair_open = b[2];
        pad.seen_ms = esp_log_timestamp();
        if (known && pair_target == PAIR_HOST0 + REMOTE_HOST) pair_target = PAIR_NONE;
    } else if (type == PT_LEDS && n >= 1) {
        host[REMOTE_HOST].leds = b[0];
        if (active == REMOTE_HOST) forward_leds();
    }
}

static void pad_rx_task(void *arg)
{
    uint8_t b, buf[20], type = 0, chk = 0;
    int st = 0, len = 0, i = 0;
    for (;;) {
        if (uart_read_bytes(PAD_UART, &b, 1, portMAX_DELAY) != 1) continue;
        switch (st) {
        case 0: if (b == 0xA5) st = 1; break;
        case 1: type = b; chk = b; st = 2; break;
        case 2: len = b; chk ^= b; i = 0; st = len > 16 ? 0 : len ? 3 : 4; break;
        case 3: buf[i++] = b; chk ^= b; if (i >= len) st = 4; break;
        case 4: if (b == chk) pad_handle(type, buf, len); st = 0; break;
        }
    }
}

static void pad_init(void)
{
    uart_config_t c = { .baud_rate = PAD_BAUD, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE };
    ESP_ERROR_CHECK(uart_driver_install(PAD_UART, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(PAD_UART, &c));
    ESP_ERROR_CHECK(uart_set_pin(PAD_UART, PAD_TX_GPIO, PAD_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreate(pad_rx_task, "pad_rx", 3072, NULL, 6, NULL);
}

void app_main(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << RESET_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    const gpio_num_t leds[] = { LED_GPIO, LED1_GPIO, LED2_GPIO };
    for (int i = 0; i < 3; i++) {
        gpio_reset_pin(leds[i]);
        gpio_set_direction(leds[i], GPIO_MODE_OUTPUT);
    }
    led_update();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("BLE_INIT", ESP_LOG_WARN);
    kvm_cfg_load();
    load_state();
    pad_init();
    disc_q = xQueueCreate(4, sizeof(int));
    gsem = xSemaphoreCreateBinary();
    xTaskCreate(disc_task, "hid_disc", 6144, NULL, 5, NULL);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;      // Just Works for the computers; the keyboard still asks for its passkey
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    hid_dev_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_svc_gap_device_appearance_set(0x03C0);
    ble_store_config_init();

    nimble_port_freertos_init(host_task);
    web_start();
}
