// KVM hub on the nRF52840: MX Keys + MX Master in (BLE central), two computers out (BLE peripheral, "KVM CMP2")
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/services/hogp.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/reboot.h>
#include <ctype.h>
#include <stdio.h>
#include <hal/nrf_power.h>
#include "hid_dev.h"
#include "cfg_svc.h"

LOG_MODULE_REGISTER(hub, LOG_LEVEL_INF);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);   // green
static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

struct adv_info { bool hid; char name[32]; };
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *buf);

static bool adv_field(struct bt_data *d, void *user)
{
    struct adv_info *a = user;
    if (d->type == BT_DATA_NAME_COMPLETE || d->type == BT_DATA_NAME_SHORTENED) {
        size_t n = MIN(d->data_len, sizeof(a->name) - 1);
        memcpy(a->name, d->data, n);
        a->name[n] = 0;
    } else if (d->type == BT_DATA_UUID16_ALL || d->type == BT_DATA_UUID16_SOME) {
        for (int i = 0; i + 1 < d->data_len; i += 2)
            if (sys_get_le16(&d->data[i]) == 0x1812) a->hid = true;
    }
    return true;
}

// ---------------------------------------------------------------- input devices (central, HID over GATT)
#define NUM_INPUTS 2
static const char *const in_name[NUM_INPUTS] = {"MX Keys", "MX Master"};   // slot 0 keyboard, slot 1 mouse

struct input {
    struct bt_conn *conn;
    struct bt_hogp hogp;
    bool ready;
};
static struct input in[NUM_INPUTS];
static bool connecting;

// Saved in flash (settings keys kvm/s0, kvm/s1 = which bonded address is the keyboard / the mouse;
// kvm/h0, kvm/h1 = the two computers; kvm/swap = bit per computer: swap Alt and Command).
static bt_addr_le_t slot_addr[NUM_INPUTS];
static bt_addr_le_t host_addr[NUM_HOSTS];
static uint8_t mod_swap = 1;                 // computer 1 is a Mac by default
static char host_name[NUM_HOSTS][16] = { "Computer 1", "Computer 2" };   // labels for the config page
static int mouse_gap_saved = -1;

static int kvm_set(const char *name, size_t len, settings_read_cb rd, void *cb_arg)
{
    if (!strcmp(name, "swap")) return rd(cb_arg, &mod_swap, 1) < 0 ? -EIO : 0;
    if (!strcmp(name, "gap")) return rd(cb_arg, &mouse_gap_saved, sizeof(mouse_gap_saved)) < 0 ? -EIO : 0;
    if (name[0] == 'n' && name[1] >= '0' && name[1] <= '1' && !name[2]) { int r = rd(cb_arg, host_name[name[1] - '0'], 15); host_name[name[1] - '0'][r > 0 ? r : 0] = 0; return r < 0 ? -EIO : 0; }
    if ((name[0] != 's' && name[0] != 'h') || name[1] < '0' || name[1] > '1' || name[2]) return -ENOENT;
    bt_addr_le_t *dst = name[0] == 's' ? &slot_addr[name[1] - '0'] : &host_addr[name[1] - '0'];
    return rd(cb_arg, dst, sizeof(bt_addr_le_t)) < 0 ? -EIO : 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(kvm, "kvm", NULL, kvm_set, NULL, NULL);

static void kvm_save(const char *key, const void *v, size_t n)
{
    char path[16];
    snprintf(path, sizeof(path), "kvm/%s", key);
    settings_save_one(path, v, n);
}
static void slot_save(int slot, const bt_addr_le_t *a)
{
    char key[4] = { 's', '0' + slot, 0 };
    slot_addr[slot] = *a;
    kvm_save(key, a, sizeof(*a));
}

static int slot_of(const struct bt_conn *c) { for (int i = 0; i < NUM_INPUTS; i++) if (in[i].conn == c) return i; return -1; }

// Two ways to connect:
//  - normal: the controller auto-connects to every bonded address (filter accept list), so a device that wakes up
//    on its Easy-Switch channel is caught even with directed advertising;
//  - pairing mode (type P on the UART, 60 s): scan for a device advertising the HID service and its name, pair fresh.
static bool pairing_mode;
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *buf);
static void auto_start(void);
static void scan_start(void);
static void reconnect_fn(struct k_work *w) { if (pairing_mode) scan_start(); else auto_start(); }
static K_WORK_DELAYABLE_DEFINE(rescan_work, reconnect_fn);

static void scan_start(void)
{
    struct bt_le_scan_param p = {
        .type = BT_LE_SCAN_TYPE_ACTIVE, .options = BT_LE_SCAN_OPT_NONE,
        .interval = 0x40, .window = 0x40,      // 40 ms / 40 ms
    };
    int rc = bt_le_scan_start(&p, scan_cb);
    if (rc && rc != -EALREADY) LOG_ERR("scan start %d", rc);
}

static void add_bond_cb(const struct bt_bond_info *info, void *ud)
{
    int *n = ud;
    bool input = false;                                // bonds of the computers are not ours to connect to
    for (int i = 0; i < NUM_INPUTS; i++) if (!bt_addr_le_cmp(&slot_addr[i], &info->addr)) input = true;
    if (input && !bt_le_filter_accept_list_add(&info->addr)) (*n)++;
}

static bt_addr_le_t stale[CONFIG_BT_MAX_PAIRED];
static int n_stale;

static void stale_cb(const struct bt_bond_info *info, void *ud)
{
    for (int i = 0; i < NUM_INPUTS; i++) {
        const bt_addr_le_t *a = in[i].conn ? bt_conn_get_dst(in[i].conn) : NULL;
        if (a && !bt_addr_le_cmp(a, &info->addr)) return;
    }
    if (n_stale < CONFIG_BT_MAX_PAIRED) stale[n_stale++] = info->addr;
}

// forget every bond that is not a connected device
static void forget_stale(void)
{
    n_stale = 0;
    bt_foreach_bond(BT_ID_DEFAULT, stale_cb, NULL);
    for (int i = 0; i < n_stale; i++) bt_unpair(BT_ID_DEFAULT, &stale[i]);
    LOG_INF("forgot %d stale bonds", n_stale);
}

static void computer_bond_cb(const struct bt_bond_info *info, void *ud)
{
    for (int i = 0; i < NUM_INPUTS; i++) if (!bt_addr_le_cmp(&slot_addr[i], &info->addr)) return;   // the MX devices stay
    if (n_stale < CONFIG_BT_MAX_PAIRED) stale[n_stale++] = info->addr;
}

// forget every bond that is not the keyboard or the mouse (computers, old pairings), on both identities
static void forget_computers(void)
{
    n_stale = 0;
    bt_foreach_bond(BT_ID_DEFAULT, computer_bond_cb, NULL);
    for (int i = 0; i < n_stale; i++) bt_unpair(BT_ID_DEFAULT, &stale[i]);
    LOG_INF("forgot %d computer bonds on identity 0", n_stale);
    bt_unpair(1, NULL);
}

static void auto_start(void)
{
    if (pairing_mode || connecting) return;
    bool free_slot = false;
    for (int i = 0; i < NUM_INPUTS; i++) if (!in[i].conn) free_slot = true;
    if (!free_slot) return;
    bt_conn_create_auto_stop();
    bt_le_filter_accept_list_clear();
    int n = 0;
    bt_foreach_bond(BT_ID_DEFAULT, add_bond_cb, &n);
    if (!n) { LOG_INF("no bonded devices: type P on the UART to pair one"); return; }
    // Looking for a missing device must not eat radio time from the links that carry the mouse:
    // 10 ms of scanning every 100 ms (the default 30 ms every 60 ms delayed the computers' links by 30-60 ms).
    struct bt_conn_le_create_param cp = BT_CONN_LE_CREATE_PARAM_INIT(BT_CONN_LE_OPT_NONE, 160, 16);
    struct bt_le_conn_param pp = BT_LE_CONN_PARAM_INIT(6, 12, 0, 400);   // 7.5-15 ms, latency 0, 4 s timeout
    int rc = bt_conn_le_create_auto(&cp, &pp);
    if (rc) LOG_ERR("auto connect %d", rc);
    else LOG_INF("waiting for %d bonded devices", n);
}

static void pairing_end_fn(struct k_work *w)
{
    if (!pairing_mode) return;
    pairing_mode = false;
    bt_le_scan_stop();
    gpio_pin_set_dt(&led_r, 0);
    LOG_INF("pairing mode over");
    auto_start();
}
static K_WORK_DELAYABLE_DEFINE(pairing_end_work, pairing_end_fn);

static void pairing_begin(void)
{
    pairing_mode = true;
    bt_conn_create_auto_stop();
    gpio_pin_set_dt(&led_r, 1);                       // red LED on while the pairing window is open
    scan_start();
    k_work_reschedule(&pairing_end_work, K_SECONDS(60));
    LOG_INF("pairing mode for 60 s: put the keyboard or mouse into pairing mode");
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *buf)
{
    if (connecting || !pairing_mode) return;
    struct adv_info a = {0};
    bt_data_parse(buf, adv_field, &a);
    if (!a.hid) return;
    int slot = -1;
    for (int i = 0; i < NUM_INPUTS; i++)
        if (!in[i].conn && !strncmp(a.name, in_name[i], strlen(in_name[i]))) { slot = i; break; }
    if (slot < 0) return;
    char s[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(addr, s, sizeof(s));
    if (bt_le_bond_exists(BT_ID_DEFAULT, addr)) {      // it advertises its name again: it wants a fresh pairing
        LOG_INF("dropping the old bond of %s", s);
        bt_unpair(BT_ID_DEFAULT, addr);
    }
    LOG_INF("Connecting to %s \"%s\" (slot %d, rssi %d)", s, a.name, slot, rssi);
    bt_le_scan_stop();
    struct bt_conn *c = NULL;
    struct bt_conn_le_create_param cp = BT_CONN_LE_CREATE_PARAM_INIT(BT_CONN_LE_OPT_NONE, BT_GAP_SCAN_FAST_INTERVAL, BT_GAP_SCAN_FAST_WINDOW);
    struct bt_le_conn_param pp = BT_LE_CONN_PARAM_INIT(6, 12, 0, 400);
    int rc = bt_conn_le_create(addr, &cp, &pp, &c);
    if (rc) { LOG_ERR("connect failed %d", rc); scan_start(); return; }
    in[slot].conn = c;
    connecting = true;
}

// ---------------------------------------------------------------- routing (which computer gets the keys and the mouse)
static bool host_dbg;           // log every input report (type D on the UART)
static int active;              // the computer that gets the keyboard and mouse now
static uint8_t keys[32], last_keys[32], suppressed[32];   // HID usage bitmaps: 0xE0..0xE7 are the modifiers
static uint16_t consumer, last_consumer;
static uint8_t mouse_buttons;
static uint8_t led_state[NUM_HOSTS];                      // keyboard LEDs each computer asked for
static int led_sent = -1;                                 // last LED value written to the MX Keys
static struct bt_hogp_rep_info *kbd_led_rep;              // the MX Keys output report with the LEDs

static bool bm_get(const uint8_t *m, int u) { return m[u >> 3] & (1 << (u & 7)); }
static void bm_set(uint8_t *m, int u) { m[u >> 3] |= 1 << (u & 7); }
static bool is_modifier(int u) { return u >= 0xE0 && u <= 0xE7; }
#define HK_LEFT  0x50
#define HK_RIGHT 0x4F

static void build_kbd_report(uint8_t rpt[8])
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
    if (mod_swap & (1 << active)) {
        uint8_t mo = rpt[0];
        rpt[0] = (mo & 0x33) | ((mo & 0x04) << 1) | ((mo & 0x08) >> 1) | ((mo & 0x40) << 1) | ((mo & 0x80) >> 1);
    }
}

static void send_keys(void)
{
    if (!hid_dev_connected(active)) return;
    uint8_t rpt[8];
    build_kbd_report(rpt);
    hid_dev_send_keyboard(active, rpt);
}

static void forward_leds(void)
{
    if (!kbd_led_rep || !in[0].ready || led_sent == led_state[active]) return;   // an unchanged value would light the backlight
    uint8_t v = led_state[active];
    if (!bt_hogp_rep_write_wo_rsp(&in[0].hogp, kbd_led_rep, &v, 1, NULL)) led_sent = v;
}

void hid_dev_on_leds(int h, uint8_t leds)
{
    LOG_INF("computer %d set keyboard LEDs 0x%02x", h + 1, leds);
    led_state[h] = leds;
    if (h == active) forward_leds();
}

static void switch_to(int target)
{
    if (target == active) return;
    if (hid_dev_connected(active)) {                 // let go of everything on the old computer
        static const uint8_t kz[8] = {0};
        hid_dev_send_keyboard(active, kz);
        hid_dev_send_consumer(active, 0);
        hid_dev_mouse_in(0, 0, 0, 0, 0);
    }
    active = target;
    hid_dev_set_active(target);
    last_consumer = 0;
    gpio_pin_set_dt(&led, active == 0);               // green = computer 1, off = computer 2
    LOG_INF(">>> Switched to computer %d%s", active + 1, hid_dev_connected(active) ? "" : " (not connected yet)");
    send_keys();
    led_sent = -1;
    forward_leds();
    if (mouse_buttons) hid_dev_mouse_in(mouse_buttons, 0, 0, 0, 0);
}

// Hotkey: Ctrl+Shift+Left = computer 1, Ctrl+Shift+Right = computer 2. Returns true if the key is swallowed.
static bool hotkey(int u)
{
    if (is_modifier(u)) return false;
    bool ctrl = bm_get(keys, 0xE0) || bm_get(keys, 0xE4);
    bool shift = bm_get(keys, 0xE1) || bm_get(keys, 0xE5);
    if (!ctrl || !shift) return false;
    int target = u == HK_LEFT ? 0 : u == HK_RIGHT ? 1 : -1;
    if (target < 0) return false;
    bm_set(suppressed, u);                // swallow the arrow *before* switching, so the new computer never sees it
    switch_to(target);
    return true;
}

static void process_keys(void)
{
    for (int u = 0; u < 256; u++) {
        bool was = bm_get(last_keys, u), is = bm_get(keys, u);
        if (was && !is) suppressed[u >> 3] &= ~(1 << (u & 7));
        if (!was && is && hotkey(u)) bm_set(suppressed, u);
    }
    memcpy(last_keys, keys, 32);
    send_keys();
}

static void keys_released(void)       // the keyboard went away: nothing stays pressed
{
    memset(keys, 0, sizeof(keys));
    process_keys();
}

static uint8_t report_cb(struct bt_hogp *h, struct bt_hogp_rep_info *rep, uint8_t err, const uint8_t *data)
{
    if (!data) return BT_GATT_ITER_STOP;
    size_t n = bt_hogp_rep_size(rep);
    uint8_t id = bt_hogp_rep_id(rep);
    if (host_dbg) {
        char hex[48] = {0};
        for (size_t i = 0; i < n && i < 15; i++) snprintk(hex + i * 3, sizeof(hex) - i * 3, "%02x ", data[i]);
        LOG_INF("report id %u len %u: %s", id, (unsigned)n, hex);
    }
    if (h == &in[0].hogp && id == 1 && n == 7) {         // MX Keys: modifiers + 6 keys
        memset(keys, 0, sizeof(keys));
        for (int i = 0; i < 8; i++) if (data[0] & (1 << i)) bm_set(keys, 0xE0 + i);
        for (int i = 1; i < 7; i++) if (data[i] >= 4) bm_set(keys, data[i]);
        process_keys();
    } else if (h == &in[0].hogp && id == 3 && n >= 2) {  // MX Keys: media keys, one 16-bit usage
        consumer = sys_get_le16(data);
        if (consumer != last_consumer) { last_consumer = consumer; hid_dev_send_consumer(active, consumer); }
    } else if (h == &in[1].hogp && id == 2 && n == 7) {  // MX Master: 16 buttons, X/Y 12 bit, wheel, pan
        int16_t x = (int16_t)(data[2] | ((data[3] & 0x0f) << 8)); if (x & 0x800) x |= 0xf000;
        int16_t y = (int16_t)((data[3] >> 4) | (data[4] << 4));   if (y & 0x800) y |= 0xf000;
        mouse_buttons = data[0];
        hid_dev_mouse_in(data[0], x, y, (int8_t)data[5], (int8_t)data[6]);
    }
    return BT_GATT_ITER_CONTINUE;
}

static void hogp_ready(struct bt_hogp *h)
{
    int slot = -1;
    for (int i = 0; i < NUM_INPUTS; i++) if (&in[i].hogp == h) slot = i;
    struct bt_hogp_rep_info *rep = NULL;
    int n = 0;
    while ((rep = bt_hogp_rep_next(h, rep))) {
        LOG_INF("slot %d: report id %u type %d size %u", slot, bt_hogp_rep_id(rep), (int)bt_hogp_rep_type(rep), (unsigned)bt_hogp_rep_size(rep));
        if (slot == 0 && bt_hogp_rep_type(rep) == BT_HIDS_REPORT_TYPE_OUTPUT && bt_hogp_rep_id(rep) == 1) kbd_led_rep = rep;
        if (bt_hogp_rep_type(rep) == BT_HIDS_REPORT_TYPE_INPUT) {
            int rc = bt_hogp_rep_subscribe(h, rep, report_cb);
            if (rc) LOG_WRN("subscribe %d", rc); else n++;
        }
    }
    LOG_INF("Input device %d ready (%d input reports subscribed)", slot, n);
    if (slot == 0) {                             // the keyboard does not need a fast link: 30 ms leaves radio time to the mouse and the computer
        static const struct bt_le_conn_param slow = BT_LE_CONN_PARAM_INIT(24, 24, 0, 400);
        int urc = bt_conn_le_param_update(bt_hogp_conn(h), &slow);
        LOG_INF("slot 0: asking 30 ms latency 0 (rc %d)", urc);
    }
    if (slot == 1) {                             // the mouse picks 11.25 ms latency 44 by itself: ask for 7.5 ms, latency 0
        static const struct bt_le_conn_param fast = BT_LE_CONN_PARAM_INIT(6, 6, 0, 400);
        int urc = bt_conn_le_param_update(bt_hogp_conn(h), &fast);
        LOG_INF("slot %d: asking 7.5 ms latency 0 (rc %d)", slot, urc);
    }
    if (slot >= 0) in[slot].ready = true;
    if (slot == 0) { led_sent = -1; forward_leds(); }
}
static void hogp_fail(struct bt_hogp *h, int err) { LOG_ERR("hogp prepare failed %d", err); }

static void dm_found(struct bt_gatt_dm *dm, void *ctx)
{
    struct input *d = ctx;
    int rc = bt_hogp_handles_assign(dm, &d->hogp);
    if (rc) LOG_ERR("handles_assign %d", rc);
    bt_gatt_dm_data_release(dm);
}
static void dm_missing(struct bt_conn *c, void *ctx) { LOG_ERR("no HID service"); bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN); }
static void dm_err(struct bt_conn *c, int err, void *ctx) { LOG_ERR("discovery error %d", err); }
static const struct bt_gatt_dm_cb dm_cb = { .completed = dm_found, .service_not_found = dm_missing, .error_found = dm_err };

static bool is_host(struct bt_conn *c)
{
    struct bt_conn_info ci;
    return !bt_conn_get_info(c, &ci) && ci.role == BT_CONN_ROLE_PERIPHERAL;
}

// ---------------------------------------------------------------- computers (peripheral)
// A computer is known by its (bonded) address. An unknown one is refused unless the pairing window of a computer is open.
static int host_pair = -1;                     // computer whose pairing window is open, or -1
static void host_pair_end_fn(struct k_work *w)
{
    if (host_pair < 0) return;
    host_pair = -1;
    gpio_pin_set_dt(&led_r, 0);
    LOG_INF("computer pairing window over");
}
static K_WORK_DELAYABLE_DEFINE(host_pair_end_work, host_pair_end_fn);

static void host_pair_begin(int h)
{
    host_pair = h;
    gpio_pin_set_dt(&led_r, 1);
    k_work_reschedule(&host_pair_end_work, K_SECONDS(120));
    LOG_INF("computer %d pairing window for 120 s: pair \"%s\" on that computer now", h + 1, CONFIG_BT_DEVICE_NAME);
}

static bool same_addr(const bt_addr_le_t *a, const bt_addr_le_t *b) { return !memcmp(a->a.val, b->a.val, 6); }   // bytes only: macOS flips public/random
static bool host_known(int h) { static const uint8_t z[6]; return memcmp(host_addr[h].a.val, z, 6) != 0; }

static void forget_bond_both(const bt_addr_le_t *a) { bt_unpair(0, a); bt_unpair(1, a); }

static void refuse_host(struct bt_conn *c, const char *why)
{
    LOG_WRN("computer refused (%s)", why);
    bt_addr_le_t a = *bt_conn_get_dst(c);
    bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    forget_bond_both(&a);
}

static void host_assigned(int h)
{
    LOG_INF("computer %d connected%s", h + 1, h == active ? " (active)" : "");
    if (h == active) { hid_dev_set_active(h); send_keys(); forward_leds(); }
}

// Works out which computer this link is. `fresh` = pairing just finished (a new bond), so an unknown one may join.
static void identify_host(struct bt_conn *c, bool fresh)
{
    if (hid_dev_host_of(c) >= 0) return;
    const bt_addr_le_t *a = bt_conn_get_dst(c);
    for (int h = 0; h < NUM_HOSTS; h++)
        if (host_known(h) && same_addr(&host_addr[h], a)) { hid_dev_assign(c, h); host_assigned(h); return; }
    // Not fresh: only an open pairing window may take an unknown computer, and only once its bond is stored
    // (before that the address can still be a temporary one that changes after pairing).
    if (!fresh && (host_pair < 0 || !(bt_le_bond_exists(0, a) || bt_le_bond_exists(1, a)))) return;
    if (host_pair < 0) { refuse_host(c, "unknown computer; type 1 or 2 on the UART first"); return; }
    int h = host_pair;
    if (host_known(h) && !same_addr(&host_addr[h], a)) forget_bond_both(&host_addr[h]);     // replaced by a new computer
    host_addr[h] = *a;
    char key[3] = { 'h', '0' + h, 0 };
    kvm_save(key, a, sizeof(*a));
    host_pair = -1;
    k_work_cancel_delayable(&host_pair_end_work);
    gpio_pin_set_dt(&led_r, 0);
    hid_dev_assign(c, h);
    host_assigned(h);
}

// a computer that connected and never told us who it is gets dropped
static void host_check_fn(struct k_work *w)
{
    struct bt_conn *c = hid_dev_pending();
    if (!c) return;
    if (host_pair >= 0) { k_work_reschedule((struct k_work_delayable *)w, K_SECONDS(10)); return; }
    refuse_host(c, "never identified itself");
}
static K_WORK_DELAYABLE_DEFINE(host_check_work, host_check_fn);

static const struct bt_conn_auth_cb auth_input;      // defined below

static void on_connected(struct bt_conn *c, uint8_t err)
{
    if (is_host(c)) {
        if (err) { LOG_ERR("computer connect failed 0x%02x", err); hid_dev_adv_start_later(); return; }
        char as[BT_ADDR_LE_STR_LEN]; bt_addr_le_to_str(bt_conn_get_dst(c), as, sizeof(as));
        LOG_INF("computer link up: %s", as);
        hid_dev_conn_open(c);
        k_work_reschedule(&host_check_work, K_SECONDS(30));
        hid_dev_adv_start_later();                        // room for the other computer
        return;
    }
    int slot = slot_of(c);
    if (slot < 0 && !err) {                            // auto-connected bonded device: take the first free slot
        for (int i = 0; i < NUM_INPUTS; i++)
            if (!in[i].conn && !bt_addr_le_cmp(&slot_addr[i], bt_conn_get_dst(c))) { slot = i; break; }
        for (int i = 0; slot < 0 && i < NUM_INPUTS; i++)
            if (!in[i].conn) slot = i;
        if (slot >= 0) in[slot].conn = bt_conn_ref(c);
        if (slot < 0) { bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN); return; }
    }
    if (slot < 0) { k_work_reschedule(&rescan_work, K_MSEC(1500)); return; }   // an auto connect attempt failed
    connecting = false;
    int orc = err ? 0 : bt_conn_auth_cb_overlay(c, &auth_input);
    if (orc) LOG_ERR("auth overlay %d", orc);
    if (err) { LOG_ERR("slot %d connect failed 0x%02x", slot, err); bt_conn_unref(c); in[slot].conn = NULL; k_work_reschedule(&rescan_work, K_MSEC(1500)); return; }
    char as[BT_ADDR_LE_STR_LEN]; bt_addr_le_to_str(bt_conn_get_dst(c), as, sizeof(as));
    LOG_INF("slot %d connected to %s, securing", slot, as);
    int rc = bt_conn_set_security(c, BT_SECURITY_L2);
    if (rc) LOG_ERR("set_security %d", rc);
    k_work_reschedule(&rescan_work, K_MSEC(1500));      // look for the other bonded device too
}
static void blue_stop(void);
static void on_disconnected(struct bt_conn *c, uint8_t reason)
{
    if (hid_dev_is_host_conn(c)) {
        LOG_WRN("computer %d disconnected, reason 0x%02x", hid_dev_host_of(c) + 1, reason);
        hid_dev_conn_close(c);            // advertising restarts in on_recycled, when the connection object is free
        return;
    }
    int slot = slot_of(c);
    if (slot < 0) return;
    blue_stop();
    LOG_WRN("slot %d disconnected, reason 0x%02x", slot, reason);
    bt_hogp_release(&in[slot].hogp);
    bt_conn_unref(c);
    in[slot].conn = NULL; in[slot].ready = false;
    connecting = false;
    if (slot == 0) { kbd_led_rep = NULL; keys_released(); if (consumer) { consumer = last_consumer = 0; hid_dev_send_consumer(active, 0); } }
    if (slot == 1) { mouse_buttons = 0; hid_dev_mouse_in(0, 0, 0, 0, 0); }
    k_work_reschedule(&rescan_work, K_MSEC(1500));
}
static void on_security(struct bt_conn *c, bt_security_t level, enum bt_security_err err)
{
    if (hid_dev_is_host_conn(c)) {
        LOG_INF("computer security level %d err %d", level, err);
        if (!err && level >= BT_SECURITY_L2) identify_host(c, false);
        return;
    }
    int slot = slot_of(c);
    if (slot < 0) return;
    LOG_INF("slot %d security level %d err %d", slot, level, err);
    if (err) {
        const bt_addr_le_t *pa = bt_conn_get_dst(c);       // the keyboard no longer knows our key: forget it, pair fresh next time
        LOG_WRN("security failed, dropping the bond");
        bt_unpair(BT_ID_DEFAULT, pa);
        bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return;
    }
    int rc = bt_gatt_dm_start(c, BT_UUID_HIDS, &dm_cb, &in[slot]);
    if (rc) LOG_ERR("dm_start %d", rc);
}
// blue LED blinks fast while a passkey is waiting to be typed
static void blue_blink_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(blue_blink_work, blue_blink_fn);
static bool blue_active;
static void blue_blink_fn(struct k_work *w)
{
    if (!blue_active) return;
    gpio_pin_toggle_dt(&led_b);
    k_work_reschedule(&blue_blink_work, K_MSEC(120));
}
static void blue_start(void) { blue_active = true; k_work_reschedule(&blue_blink_work, K_NO_WAIT); }
static void blue_stop(void) { blue_active = false; k_work_cancel_delayable(&blue_blink_work); gpio_pin_set_dt(&led_b, 0); }

static void blue_test_stop_fn(struct k_work *w) { blue_stop(); }
static K_WORK_DELAYABLE_DEFINE(blue_test_stop, blue_test_stop_fn);

static void passkey_display(struct bt_conn *c, unsigned int passkey)
{
    blue_start();                                     // blue LED blinks: type the passkey on the keyboard now
    printk("\n*** TYPE THIS PASSKEY ON THE KEYBOARD OR COMPUTER, THEN PRESS ENTER: %06u ***\n\n", passkey);
}
static void pair_cancel(struct bt_conn *c) { blue_stop(); LOG_WRN("pairing cancelled"); }
static unsigned int app_passkey(struct bt_conn *c) { return 123456; }
static struct bt_conn_auth_cb auth_cbs = { .cancel = pair_cancel };   // global: no IO, computers pair without a code
static const struct bt_conn_auth_cb auth_input = { .passkey_display = passkey_display, .app_passkey = app_passkey, .cancel = pair_cancel };   // MX Keys: type 123456 on the keyboard
static void pair_done(struct bt_conn *c, bool bonded)
{
    blue_stop();
    LOG_INF("pairing complete, bonded %d", bonded);
    int sl = slot_of(c);
    if (bonded && sl >= 0) slot_save(sl, bt_conn_get_dst(c));
    if (bonded && hid_dev_is_host_conn(c)) { identify_host(c, true); return; }
    if (pairing_mode) { k_work_cancel_delayable(&pairing_end_work); pairing_mode = false; gpio_pin_set_dt(&led_r, 0); }
}
static void pair_failed(struct bt_conn *c, enum bt_security_err reason) { blue_stop(); LOG_WRN("pairing failed, reason %d", reason); }
static struct bt_conn_auth_info_cb auth_info = { .pairing_complete = pair_done, .pairing_failed = pair_failed };


// ---------------------------------------------------------------- configuration page (Web Bluetooth, see cfg_svc.c)
static void json_name(char *out, size_t n, const char *s)       // names are plain: letters, digits, space, dash
{
    size_t j = 0;
    for (; *s && j + 1 < n; s++) out[j++] = (isalnum((unsigned char)*s) || *s == ' ' || *s == '-') ? *s : '_';
    out[j] = 0;
}

int cfg_status(char *buf, size_t n)
{
    char n0[20], n1[20];
    json_name(n0, sizeof(n0), host_name[0]);
    json_name(n1, sizeof(n1), host_name[1]);
    return snprintf(buf, n,
        "{\"v\":1,\"active\":%d,\"gap\":%d,\"kbd\":%d,\"mouse\":%d,\"pair\":%d,\"pairmx\":%d,"
        "\"h\":[{\"n\":\"%s\",\"known\":%d,\"on\":%d,\"swap\":%d},{\"n\":\"%s\",\"known\":%d,\"on\":%d,\"swap\":%d}]}",
        active, hid_dev_get_mouse_gap(), in[0].ready, in[1].ready, host_pair + 1, pairing_mode,
        n0, host_known(0), hid_dev_connected(0), (mod_swap >> 0) & 1,
        n1, host_known(1), hid_dev_connected(1), (mod_swap >> 1) & 1);
}

static void forget_host(int h)
{
    bt_addr_le_t old = host_addr[h];
    struct bt_conn *c = hid_dev_conn_for(h);
    memset(&host_addr[h], 0, sizeof(host_addr[h]));
    char key[3] = { 'h', '0' + h, 0 };
    kvm_save(key, &host_addr[h], sizeof(host_addr[h]));
    if (c) bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    forget_bond_both(&old);
    LOG_INF("computer %d forgotten", h + 1);
}

void cfg_command(const char *cmd)
{
    int a = 0, b = 0;
    char word[16] = {0};
    LOG_INF("config command: %s", cmd);
    if (sscanf(cmd, "%15s %d %d", word, &a, &b) < 1) return;
    int h = a - 1;
    bool valid = h >= 0 && h < NUM_HOSTS;
    if (!strcmp(word, "pair") && valid) host_pair_begin(h);
    else if (!strcmp(word, "pairmx")) pairing_begin();
    else if (!strcmp(word, "switch") && valid) switch_to(h);
    else if (!strcmp(word, "swap") && valid) {
        if (b) mod_swap |= 1 << h; else mod_swap &= ~(1 << h);
        kvm_save("swap", &mod_swap, 1);
        send_keys();
    } else if (!strcmp(word, "gap")) {
        hid_dev_set_mouse_gap(a);
        int g = hid_dev_get_mouse_gap();
        kvm_save("gap", &g, sizeof(g));
    } else if (!strcmp(word, "forget") && valid) forget_host(h);
    else if (!strcmp(word, "name") && valid) {
        const char *p = strchr(cmd, ' ');                 // "name 1 Mac": the rest of the line after the number
        if (p) p = strchr(p + 1, ' ');
        char tmp[16];
        json_name(tmp, sizeof(tmp), p ? p + 1 : "");
        if (!tmp[0]) snprintf(tmp, sizeof(tmp), "Computer %d", a);
        strcpy(host_name[h], tmp);
        char key[3] = { 'n', '0' + h, 0 };
        kvm_save(key, host_name[h], strlen(host_name[h]));
    } else if (!strcmp(word, "reboot")) { k_sleep(K_MSEC(100)); sys_reboot(SYS_REBOOT_COLD); }
}

// diagnosis: every link's timing, to catch what degrades the radio schedule
void links_dump(const char *why)
{
    LOG_INF("links (%s):", why);
    for (int i = 0; i < NUM_INPUTS; i++) {
        struct bt_conn_info ci;
        if (!in[i].conn || bt_conn_get_info(in[i].conn, &ci)) continue;
        LOG_INF("  %s: %u.%02u ms lat %u to %u ms", i ? "mouse" : "keyboard", ci.le.interval * 5 / 4, (ci.le.interval * 125) % 100,
                ci.le.latency, ci.le.timeout * 10);
    }
    for (int h = 0; h < NUM_HOSTS; h++) {
        struct bt_conn_info ci;
        struct bt_conn *c = hid_dev_conn_for(h);
        if (!c || bt_conn_get_info(c, &ci)) continue;
        LOG_INF("  computer %d%s: %u.%02u ms lat %u to %u ms", h + 1, h == active ? " (active)" : "", ci.le.interval * 5 / 4, (ci.le.interval * 125) % 100,
                ci.le.latency, ci.le.timeout * 10);
    }
}

static void on_le_param(struct bt_conn *c, uint16_t interval, uint16_t latency, uint16_t timeout)
{
    if (slot_of(c) >= 0) LOG_INF("slot %d link %u.%02u ms latency %u", slot_of(c), interval * 5 / 4, (interval * 125) % 100, latency);
    if (hid_dev_is_host_conn(c)) LOG_INF("computer %d link %u.%02u ms latency %u", hid_dev_host_of(c) + 1, interval * 5 / 4, (interval * 125) % 100, latency);
}

// the input devices ask for slow, sleepy links of their own; keep the 7.5 ms we set
static bool on_param_req(struct bt_conn *c, struct bt_le_conn_param *p)
{
    return slot_of(c) < 0;
}

static void on_recycled(void) { hid_dev_adv_start_later(); }

BT_CONN_CB_DEFINE(conn_cbs) = { .recycled = on_recycled, .le_param_req = on_param_req, .le_param_updated = on_le_param, .connected = on_connected, .disconnected = on_disconnected, .security_changed = on_security };

static void bond_cb(const struct bt_bond_info *info, void *ud)
{
    char as[BT_ADDR_LE_STR_LEN]; bt_addr_le_to_str(&info->addr, as, sizeof(as));
    LOG_INF("stored bond: %s", as);
}

int main(void)
{
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);   // marker 1: green stays on while we wait
    gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_b, GPIO_OUTPUT_INACTIVE);
    k_sleep(K_SECONDS(3));                     // let the USB console come up
    gpio_pin_set_dt(&led_b, 1);                // marker 2: sleep done
    LOG_INF("KVM hub prototype, boot");
    int rc = bt_enable(NULL);
    gpio_pin_set_dt(&led_r, 1);                // marker 3: bt_enable returned
    if (rc) { LOG_ERR("bt_enable %d", rc); return 0; }
    gpio_pin_set_dt(&led, 0);gpio_pin_set_dt(&led_b, 0);gpio_pin_set_dt(&led_r, 0);                       // marker 2: off briefly, then slow blink = BT is up
    settings_load();
    // bonds are kept now
    hid_dev_init();
    hid_dev_id_setup();
    if (mouse_gap_saved >= 0) hid_dev_set_mouse_gap(mouse_gap_saved);
    bt_conn_auth_cb_register(&auth_cbs);
    bt_conn_auth_info_cb_register(&auth_info);
    for (int i = 0; i < NUM_INPUTS; i++) {
        static const struct bt_hogp_init_params hp = { .ready_cb = hogp_ready, .prep_error_cb = hogp_fail };
        bt_hogp_init(&in[i].hogp, &hp);
    }
    bt_foreach_bond(BT_ID_DEFAULT, bond_cb, NULL);
    auto_start();
    hid_dev_adv_start();
    hid_dev_adv_start_later();                      // also starts the always-on beacon
    const struct device *con = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    for (int n = 0;; n++) {
        gpio_pin_toggle_dt(&led);
        unsigned char c;
        while (uart_poll_in(con, &c) == 0)           // type B on the UART: reboot into the UF2 bootloader
            if (c == 'P') pairing_begin();
            else if (c == '1' || c == '2') host_pair_begin(c - '1');
            else if (c == 'Y') { hid_dev_beacon(!hid_dev_beacon_on()); LOG_INF("beacon %s", hid_dev_beacon_on() ? "on" : "off"); }
            else if (c == 'N') links_dump("asked");
            else if (c == 'J') hid_dev_jiggle(6000);
            else if (c == 'R') { LOG_INF("rebooting"); k_sleep(K_MSEC(100)); sys_reboot(SYS_REBOOT_COLD); }
            else if (c == 'M') LOG_INF("mouse report spacing now %d ms", hid_dev_cycle_mouse_gap());
            else if (c == '3' || c == '4') switch_to(c - '3');
            else if (c == 'S') { mod_swap ^= 1 << active; kvm_save("swap", &mod_swap, 1); LOG_INF("computer %d Alt/Command swap %s", active + 1, (mod_swap >> active) & 1 ? "on" : "off"); send_keys(); }
            else if (c == 'K') LOG_INF("active %d | connected: %d %d | known: %d %d | swap %02x | keyboard %d mouse %d", active + 1, hid_dev_connected(0), hid_dev_connected(1), host_known(0), host_known(1), mod_swap, in[0].ready, in[1].ready);
            else if (c == 'C') forget_stale();
            else if (c == 'L') { LOG_INF("bonds id 0:"); bt_foreach_bond(0, bond_cb, NULL); LOG_INF("bonds id 1:"); bt_foreach_bond(1, bond_cb, NULL); }
            else if (c == 'U') forget_computers();
            else if (c == 'D') { host_dbg = !host_dbg; LOG_INF("report log %d", host_dbg); }
            else if (c == 'T') { blue_start(); k_work_schedule(&blue_test_stop, K_SECONDS(6)); LOG_INF("blue LED test, 6 s"); }
            else if (c == 'X') { bt_unpair(BT_ID_DEFAULT, NULL); bt_unpair(1, NULL); LOG_INF("all bonds forgotten"); }
            else if (c == 'B') {
                printk("rebooting into UF2 bootloader\n");
                k_sleep(K_MSEC(50));
                nrf_power_gpregret_set(NRF_POWER, 0, 0x57);
                sys_reboot(SYS_REBOOT_COLD);
            }
        if (n % 4 == 0) printk("alive %d s\n", n / 2);
        k_sleep(K_MSEC(500));
    }
}

