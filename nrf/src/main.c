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
#include <zephyr/random/random.h>
#include <ctype.h>
#include <stdio.h>
#include <hal/nrf_power.h>
#include "hid_dev.h"
#include "cfg_svc.h"
#include "nfc.h"

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
static char host_name[NUM_HOSTS][16] = { "Computer 1", "Computer 2", "Computer 3", "Computer 4" };   // labels for the config page
static int mouse_gap_saved = -1;
static struct { uint8_t gap[NUM_HOSTS]; uint8_t even; } mouse_saved;     // per computer: report spacing (ms) and "even out" bits
static bool mouse_saved_ok;
static uint8_t sw_btn, ns_btn = 4;           // mouse buttons (1-based, 0 = none) for the switch and no-sleep hotkey clicks
static volatile int64_t last_key_ms;       // uptime of the last keyboard report (the gaze prototype does not switch computers while you type)
static uint8_t nosleep_bits;                 // bit per computer: an F15 tap every 30 s keeps it awake
static char why_log[320];                     // last bond removals with their reason, kept in flash (UART key W)
static uint8_t new_id_pending;                // 1 = at the next boot give the computers a new Bluetooth address (UART key I)

static int kvm_set(const char *name, size_t len, settings_read_cb rd, void *cb_arg)
{
    if (!strcmp(name, "swap")) return rd(cb_arg, &mod_swap, 1) < 0 ? -EIO : 0;
    if (!strcmp(name, "btn")) { uint8_t b[2] = {0}; int r = rd(cb_arg, b, 2); if (r >= 2) { sw_btn = b[0]; ns_btn = b[1]; } return r < 0 ? -EIO : 0; }
    if (!strcmp(name, "ns")) return rd(cb_arg, &nosleep_bits, 1) < 0 ? -EIO : 0;
    if (!strcmp(name, "newid")) return rd(cb_arg, &new_id_pending, 1) < 0 ? -EIO : 0;
    if (!strcmp(name, "why")) { int r = rd(cb_arg, why_log, sizeof(why_log) - 1); why_log[r > 0 ? r : 0] = 0; return r < 0 ? -EIO : 0; }
    if (!strcmp(name, "mouse")) { int r = rd(cb_arg, &mouse_saved, sizeof(mouse_saved)); mouse_saved_ok = r == sizeof(mouse_saved); return r < 0 ? -EIO : 0; }
    if (!strcmp(name, "gap")) return rd(cb_arg, &mouse_gap_saved, sizeof(mouse_gap_saved)) < 0 ? -EIO : 0;
    if (name[0] == 'n' && name[1] >= '0' && name[1] < '0' + NUM_HOSTS && !name[2]) { int r = rd(cb_arg, host_name[name[1] - '0'], 15); host_name[name[1] - '0'][r > 0 ? r : 0] = 0; return r < 0 ? -EIO : 0; }
    if ((name[0] != 's' && name[0] != 'h') || name[1] < '0' || name[2]) return -ENOENT;
    if (name[0] == 's' ? name[1] >= '0' + NUM_INPUTS : name[1] >= '0' + NUM_HOSTS) return -ENOENT;
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
static void mouse_save(void)
{
    for (int h = 0; h < NUM_HOSTS; h++) { mouse_saved.gap[h] = hid_dev_host_gap(h); if (hid_dev_host_even(h)) mouse_saved.even |= 1 << h; else mouse_saved.even &= ~(1 << h); }
    kvm_save("mouse", &mouse_saved, sizeof(mouse_saved));
}
// Every bond removal goes through here so a lost pairing can be traced after a power cycle.
static int unpair_why(uint8_t id, const bt_addr_le_t *a, const char *why)
{
    char as[BT_ADDR_LE_STR_LEN] = "all";
    if (a) bt_addr_le_to_str(a, as, sizeof(as));
    LOG_WRN("UNPAIR identity %u %s: %s", id, as, why);
    char evt[64], tmp[sizeof(why_log)];
    snprintf(evt, sizeof(evt), "%us id%u %.17s %s", (unsigned)(k_uptime_get() / 1000), id, as, why);
    snprintf(tmp, sizeof(tmp), "%s | %s", evt, why_log);
    tmp[sizeof(why_log) - 1] = 0;
    strcpy(why_log, tmp);
    kvm_save("why", why_log, strlen(why_log));
    return bt_unpair(id, a);
}

static void slot_save(int slot, const bt_addr_le_t *a)
{
    char key[4] = { 's', '0' + slot, 0 };
    static const uint8_t zero[6];
    if (memcmp(slot_addr[slot].a.val, zero, 6) && bt_addr_le_cmp(&slot_addr[slot], a)) {      // this slot was paired again: the old pairing is dead weight
        bt_addr_le_t old = slot_addr[slot];
        slot_addr[slot] = *a;
        unpair_why(BT_ID_DEFAULT, &old, "replaced by a new pairing");
    }
    slot_addr[slot] = *a;
    kvm_save(key, a, sizeof(*a));
}

static int slot_of(const struct bt_conn *c) { for (int i = 0; i < NUM_INPUTS; i++) if (in[i].conn == c) return i; return -1; }

// Two ways to connect:
//  - normal: the controller auto-connects to every bonded address (filter accept list), so a device that wakes up
//    on its Easy-Switch channel is caught even with directed advertising;
//  - pairing mode (type P on the UART, 60 s): scan for a device advertising the HID service and its name, pair fresh.
static bool pairing_mode;
static int pair_slot = -1;                      // which MX device the pairing window is for: 0 keyboard, 1 mouse, -1 either

// Setup mode (the phone): while open, a phone may use the config service without being paired. A wizard walks through the steps.
enum { ST_KBD, ST_MOUSE, ST_C1, ST_C2, ST_DONE };
static bool setup_open;
static uint8_t setup_step = ST_DONE;
static int64_t setup_until;                     // when setup closes by itself (ms of uptime), 0 = never
static uint32_t gen_code;                       // random code for the next keyboard pairing, 0 = use the fixed one
static void setup_begin(bool wizard, int minutes);
static void setup_event(int what, int idx);     // 0 = an MX device was paired (idx = slot), 1 = a new computer joined (idx)

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
    for (int i = 0; i < n_stale; i++) unpair_why(BT_ID_DEFAULT, &stale[i], "key C");
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
    for (int i = 0; i < n_stale; i++) unpair_why(BT_ID_DEFAULT, &stale[i], "key U");
    LOG_INF("forgot %d computer bonds on identity 0", n_stale);
    unpair_why(1, NULL, "key U");
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
    pair_slot = -1;
    gen_code = 0;
    bt_le_scan_stop();
    LOG_INF("pairing mode over");
    auto_start();
}
static K_WORK_DELAYABLE_DEFINE(pairing_end_work, pairing_end_fn);

static void pairing_begin(int slot)
{
    if (slot != 1) gen_code = 100000 + sys_rand32_get() % 900000;     // the keyboard (or either device) asks for a code: a fresh random one, shown on the page
    pairing_mode = true;
    pair_slot = slot;
    bt_conn_create_auto_stop();
    scan_start();
    int secs = setup_open ? 300 : 60;
    k_work_reschedule(&pairing_end_work, K_SECONDS(secs));
    LOG_INF("pairing mode for %d s: put the %s into pairing mode", secs, slot == 0 ? "keyboard" : slot == 1 ? "mouse" : "keyboard or mouse");
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *buf)
{
    if (connecting || !pairing_mode) return;
    struct adv_info a = {0};
    bt_data_parse(buf, adv_field, &a);
    if (!a.hid) return;
    int slot = -1;
    for (int i = 0; i < NUM_INPUTS; i++)
        if (!in[i].conn && (pair_slot < 0 || pair_slot == i) && !strncmp(a.name, in_name[i], strlen(in_name[i]))) { slot = i; break; }
    if (slot < 0) return;
    char s[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(addr, s, sizeof(s));
    if (bt_le_bond_exists(BT_ID_DEFAULT, addr)) {      // it advertises its name again: it wants a fresh pairing
        LOG_INF("dropping the old bond of %s", s);
        unpair_why(BT_ID_DEFAULT, addr, "pairing scan: device advertises again");
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
static uint8_t mouse_buttons;               // buttons the computer sees
static uint8_t mouse_raw;                   // buttons as the mouse reports them
static uint8_t btn_suppress;                // buttons hidden from the computer until released (a hotkey click)
static uint8_t learn_btn;                   // 1 = the next click becomes the switch button, 2 = the no-sleep button
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
    LOG_INF(">>> Switched to computer %d%s", active + 1, hid_dev_connected(active) ? "" : " (not connected yet)");
    send_keys();
    led_sent = -1;
    forward_leds();
    if (mouse_buttons) hid_dev_mouse_in(mouse_buttons, 0, 0, 0, 0);
}

static bool host_known(int h);

// The computer one place to the left (dir -1) or right (dir +1) of the active one, or -1 to stay. Empty positions are
// skipped. Past the end it wraps round, but only with three or more computers (with two, Left and Right just pick a side).
static int step_target(int dir)
{
    int n = 0;
    for (int h = 0; h < NUM_HOSTS; h++) if (host_known(h)) n++;
    for (int h = active + dir; h >= 0 && h < NUM_HOSTS; h += dir) if (host_known(h)) return h;
    if (n < 3) return -1;
    for (int h = dir > 0 ? 0 : NUM_HOSTS - 1; h != active; h -= dir) if (host_known(h)) return h;
    return -1;
}

// Hotkey: Ctrl+Shift+Left / Right = the computer to the left / right, Ctrl+Shift+1..4 = that position.
// Returns true if the key is swallowed.
static bool hotkey(int u)
{
    if (is_modifier(u)) return false;
    bool ctrl = bm_get(keys, 0xE0) || bm_get(keys, 0xE4);
    bool shift = bm_get(keys, 0xE1) || bm_get(keys, 0xE5);
    static int seq_pos;
    if (!ctrl || !shift) { seq_pos = 0; return false; }
    static const uint8_t seq[4] = { 0x25, 0x15, 0x08, 0x06 };     // Ctrl+Shift held, then 8 R E C: open the phone link
    if (u == seq[seq_pos] || u == seq[0]) {
        seq_pos = (u == seq[seq_pos]) ? seq_pos + 1 : 1;
        bm_set(suppressed, u);
        if (seq_pos == 4) { seq_pos = 0; setup_begin(false, 5); }
        return true;
    }
    seq_pos = 0;
    int target = -1;
    if (u == HK_LEFT) target = step_target(-1);
    else if (u == HK_RIGHT) target = step_target(1);
    else if (u >= 0x1E && u < 0x1E + NUM_HOSTS) target = host_known(u - 0x1E) ? u - 0x1E : -1;     // Ctrl+Shift+1..4: that position
    else return false;
    bm_set(suppressed, u);                // swallow the key *before* switching, so the new computer never sees it
    if (target >= 0) switch_to(target);
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

static bool ctrl_shift_down(void)
{
    return (bm_get(keys, 0xE0) || bm_get(keys, 0xE4)) && (bm_get(keys, 0xE1) || bm_get(keys, 0xE5));
}

static void nosleep_set(int h, bool on)
{
    if (on) nosleep_bits |= 1 << h; else nosleep_bits &= ~(1 << h);
    kvm_save("ns", &nosleep_bits, 1);
    LOG_INF("no-sleep computer %d: %s", h + 1, on ? "on" : "off");
}

static void buttons_save(void) { uint8_t b[2] = { sw_btn, ns_btn }; kvm_save("btn", b, 2); }

static void learn_end_fn(struct k_work *w) { if (learn_btn) { learn_btn = 0; LOG_INF("learn timed out"); } }
static K_WORK_DELAYABLE_DEFINE(learn_end_work, learn_end_fn);

static void learn_begin(int what)
{
    learn_btn = what;
    k_work_reschedule(&learn_end_work, K_SECONDS(15));
    LOG_INF("press the mouse button to use as the %s button (15 s)", what == 1 ? "switch" : "no-sleep");
}

// A mouse button went down: either a hotkey click (Ctrl+Shift held) or the button being learned.
static void mouse_click(uint8_t newly)
{
    if (learn_btn) {
        int bit = __builtin_ctz(newly);
        if (learn_btn == 1) sw_btn = bit + 1; else ns_btn = bit + 1;
        LOG_INF("learned: mouse button %d is the %s button", bit + 1, learn_btn == 1 ? "switch" : "no-sleep");
        learn_btn = 0; btn_suppress |= 1 << bit; buttons_save();
        k_work_cancel_delayable(&learn_end_work);
        return;
    }
    if (!ctrl_shift_down()) return;
    if (sw_btn && ((newly >> (sw_btn - 1)) & 1)) { btn_suppress |= 1 << (sw_btn - 1); { int t = step_target(1); if (t < 0) t = step_target(-1); if (t >= 0) switch_to(t); } }
    else if (ns_btn && ((newly >> (ns_btn - 1)) & 1)) { btn_suppress |= 1 << (ns_btn - 1); nosleep_set(active, !((nosleep_bits >> active) & 1)); }
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
        last_key_ms = k_uptime_get();
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
        uint8_t raw = data[0], newly = raw & ~mouse_raw;
        mouse_raw = raw;
        if (newly) mouse_click(newly);
        btn_suppress &= raw;                              // a hidden click stays hidden until the button is released
        mouse_buttons = raw & ~btn_suppress;
        hid_dev_mouse_in(mouse_buttons, x, y, (int8_t)data[5], (int8_t)data[6]);
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
    LOG_INF("computer pairing window over");
}
static K_WORK_DELAYABLE_DEFINE(host_pair_end_work, host_pair_end_fn);

static void host_pair_begin(int h)
{
    host_pair = h;
    int secs = setup_open ? 300 : 120;
    k_work_reschedule(&host_pair_end_work, K_SECONDS(secs));
    LOG_INF("computer %d pairing window for %d s: pair \"%s\" on that computer now", h + 1, secs, CONFIG_BT_DEVICE_NAME);
}

static bool same_addr(const bt_addr_le_t *a, const bt_addr_le_t *b) { return !memcmp(a->a.val, b->a.val, 6); }   // bytes only: macOS flips public/random
static bool host_known(int h) { static const uint8_t z[6]; return memcmp(host_addr[h].a.val, z, 6) != 0; }

static void forget_bond_both(const bt_addr_le_t *a, const char *why) { unpair_why(0, a, why); unpair_why(1, a, why); }

static void refuse_host(struct bt_conn *c, const char *why)
{
    LOG_WRN("computer refused (%s)", why);
    bt_addr_le_t a = *bt_conn_get_dst(c);
    bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    forget_bond_both(&a, why);
}

// A computer that still has its default label ("Computer N") is asked for its own Bluetooth name (the Generic Access
// Device Name). Computers that you renamed keep your name.
static struct bt_gatt_read_params name_rd;
static bool name_busy;
static uint8_t name_tries[NUM_HOSTS];                            // give up on a computer that will not tell its name
static void name_read_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(name_read_work, name_read_fn);

static uint8_t name_cb(struct bt_conn *c, uint8_t err, struct bt_gatt_read_params *p, const void *data, uint16_t len)
{
    int h = hid_dev_host_of(c);
    if (!err && data && h >= 0 && !strncmp(host_name[h], "Computer ", 9)) {
        char nm[16]; int j = 0;
        const uint8_t *d = data;
        for (int i = 0; i < len && j < 15; i++)                       // letters, digits, space and dash only
            if (isalnum(d[i]) || d[i] == ' ' || d[i] == '-') nm[j++] = d[i];
        nm[j] = 0;
        if (j > 0) {
            strcpy(host_name[h], nm);
            char key[3] = { 'n', '0' + h, 0 };
            kvm_save(key, host_name[h], strlen(host_name[h]));
            LOG_INF("computer %d is called \"%s\" (its Bluetooth name)", h + 1, nm);
        }
    }
    name_busy = false;
    k_work_reschedule(&name_read_work, K_SECONDS(1));                 // the next computer that still needs a name
    return BT_GATT_ITER_STOP;
}

static void name_read_fn(struct k_work *w)
{
    if (name_busy) return;
    for (int h = 0; h < NUM_HOSTS; h++) {
        struct bt_conn *c = hid_dev_conn_for(h);
        if (!c || strncmp(host_name[h], "Computer ", 9) || name_tries[h] >= 2) continue;
        name_tries[h]++;
        name_rd = (struct bt_gatt_read_params){ .func = name_cb, .by_uuid = { .uuid = BT_UUID_GAP_DEVICE_NAME, .start_handle = 0x0001, .end_handle = 0xffff } };
        name_busy = true;
        int rc = bt_gatt_read(c, &name_rd);
        if (rc) { name_busy = false; LOG_INF("computer %d name read failed (%d)", h + 1, rc); }
        return;
    }
}

static void host_assigned(int h)
{
    LOG_INF("computer %d connected%s", h + 1, h == active ? " (active)" : "");
    name_tries[h] = 0;
    k_work_reschedule(&name_read_work, K_SECONDS(3));
    if (h == active) { hid_dev_set_active(h); send_keys(); forward_leds(); }
}

// Works out which computer this link is. `fresh` = pairing just finished (a new bond), so an unknown one may join.
static void identify_host(struct bt_conn *c, bool fresh)
{
    if (hid_dev_host_of(c) >= 0 || hid_dev_host_of(c) == -2) return;     // already known, or the phone
    const bt_addr_le_t *a = bt_conn_get_dst(c);
    for (int h = 0; h < NUM_HOSTS; h++)
        if (host_known(h) && same_addr(&host_addr[h], a)) { hid_dev_assign(c, h); host_assigned(h); return; }
    // Not fresh: only an open pairing window may take an unknown computer, and only once its bond is stored
    // (before that the address can still be a temporary one that changes after pairing).
    if (!fresh && (host_pair < 0 || !(bt_le_bond_exists(0, a) || bt_le_bond_exists(1, a)))) return;
    if (host_pair < 0) { refuse_host(c, "unknown computer; type 1 or 2 on the UART first"); return; }
    int h = host_pair;
    if (host_known(h) && !same_addr(&host_addr[h], a)) forget_bond_both(&host_addr[h], "computer replaced");     // replaced by a new computer
    host_addr[h] = *a;
    char key[3] = { 'h', '0' + h, 0 };
    kvm_save(key, a, sizeof(*a));
    host_pair = -1;
    k_work_cancel_delayable(&host_pair_end_work);
    hid_dev_assign(c, h);
    host_assigned(h);
    if (setup_open) setup_event(1, h);
}

// a computer that connected and never told us who it is gets dropped
static void host_check_fn(struct k_work *w)
{
    struct bt_conn *c = hid_dev_pending();
    if (!c) return;
    if (host_pair >= 0) { k_work_reschedule((struct k_work_delayable *)w, K_SECONDS(10)); return; }
    const bt_addr_le_t *a = bt_conn_get_dst(c);
    for (int h = 0; h < NUM_HOSTS; h++)
        if (host_known(h) && same_addr(&host_addr[h], a)) {          // a computer we know that did not encrypt in time: let it try again, keep its pairing
            LOG_WRN("known computer %d did not encrypt in 30 s: disconnected, pairing kept", h + 1);
            bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            return;
        }
    refuse_host(c, "never identified itself");
}
static K_WORK_DELAYABLE_DEFINE(host_check_work, host_check_fn);

// ---------------------------------------------------------------- setup wizard (driven from the phone page)
static void setup_enter_step(int step)
{
    setup_step = step;
    gen_code = 0;
    switch (step) {
    case ST_KBD:   gen_code = 100000 + sys_rand32_get() % 900000; pairing_begin(0); break;   // a fresh random code every time
    case ST_MOUSE: pairing_begin(1); break;
    case ST_C1:    host_pair_begin(0); break;
    case ST_C2:    host_pair_begin(1); break;
    default:       break;
    }
    LOG_INF("setup step %d%s", step, step == ST_KBD ? " (code on the page)" : "");
}

static void setup_close_windows(void)
{
    if (pairing_mode) pairing_end_fn(NULL);
    if (host_pair >= 0) host_pair_end_fn(NULL);
}

static void setup_next_fn(struct k_work *w)
{
    if (!setup_open || setup_step >= ST_DONE) return;
    setup_close_windows();
    setup_enter_step(setup_step + 1);
}
static K_WORK_DELAYABLE_DEFINE(setup_next_work, setup_next_fn);

static void setup_event(int what, int idx)
{
    bool done = (what == 0 && ((setup_step == ST_KBD && idx == 0) || (setup_step == ST_MOUSE && idx == 1))) ||
                (what == 1 && ((setup_step == ST_C1 && idx == 0) || (setup_step == ST_C2 && idx == 1)));
    if (done) k_work_reschedule(&setup_next_work, K_MSEC(1500));      // a moment later: let the new link finish its own setup
}

static void setup_begin(bool wizard, int minutes)
{
    setup_open = true;
    hid_dev_cfg_adv(true);
    setup_until = 0;                              // never closes by itself: only the page's Close button (or a reset) ends it. `minutes` is ignored on purpose
    LOG_INF("setup open for %d min%s", minutes, wizard ? " (wizard)" : "");
    if (wizard) setup_enter_step(ST_KBD); else setup_step = ST_DONE;
}

static void setup_end(void)
{
    setup_open = false;
    hid_dev_cfg_adv(false);
    setup_step = ST_DONE;
    gen_code = 0;
    k_work_cancel_delayable(&setup_next_work);
    setup_close_windows();
    int n = hid_dev_disconnect_config();
    LOG_INF("setup closed (%d phone link%s dropped)", n, n == 1 ? "" : "s");
}

// The config service: a paired computer, or any device while setup is open (that link is then kept as the config link).
bool cfg_access_ok(struct bt_conn *c)
{
    if (hid_dev_host_of(c) >= 0) return true;
    if (setup_open) { hid_dev_mark_config(c); return true; }
    return false;
}

static const struct bt_conn_auth_cb auth_input;      // defined below

static void on_connected(struct bt_conn *c, uint8_t err)
{
    if (is_host(c)) {
        if (err) { LOG_ERR("computer connect failed 0x%02x", err); hid_dev_adv_start_later(); return; }
        char as[BT_ADDR_LE_STR_LEN]; bt_addr_le_to_str(bt_conn_get_dst(c), as, sizeof(as));
        LOG_INF("computer link up: %s", as);
        hid_dev_conn_open(c);
        if (hid_dev_is_cfg_conn(c)) {                    // the phone on its setup advertisement: not a computer, never bonded
            hid_dev_mark_config(c);
            bt_conn_set_bondable(c, false);
            LOG_INF("phone link up (setup)");
        }
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
    if (slot == 1) { mouse_buttons = mouse_raw = btn_suppress = 0; hid_dev_mouse_in(0, 0, 0, 0, 0); }
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
        const bt_addr_le_t *pa = bt_conn_get_dst(c);
        if (err == BT_SECURITY_ERR_PIN_OR_KEY_MISSING) {         // the device really no longer knows our key: forget it, pair fresh next time
            LOG_WRN("security failed (key missing), dropping the bond");
            unpair_why(BT_ID_DEFAULT, pa, "peer lost its key");
        } else {                                                // anything else (a link that broke half way, a glitch) is not a reason to lose a pairing
            LOG_WRN("security failed (err %d): bond kept, will retry", err);
        }
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
static bool passkey_wait;          // a code has to be typed on the keyboard (green blinks slowly)
static void blue_blink_fn(struct k_work *w)
{
    if (!blue_active) return;
    gpio_pin_toggle_dt(&led_b);
    k_work_reschedule(&blue_blink_work, K_MSEC(120));
}
static void blue_start(void) { blue_active = true; k_work_reschedule(&blue_blink_work, K_NO_WAIT); }
static void blue_stop(void) { passkey_wait = false; blue_active = false; k_work_cancel_delayable(&blue_blink_work); gpio_pin_set_dt(&led_b, 0); }

static void blue_test_stop_fn(struct k_work *w) { blue_stop(); }
static K_WORK_DELAYABLE_DEFINE(blue_test_stop, blue_test_stop_fn);

static void passkey_display(struct bt_conn *c, unsigned int passkey)
{
    passkey_wait = true;                              // green LED blinks slowly: type the passkey on the keyboard now
    printk("\n*** TYPE THIS PASSKEY ON THE KEYBOARD OR COMPUTER, THEN PRESS ENTER: %06u ***\n\n", passkey);
}
static void pair_cancel(struct bt_conn *c) { blue_stop(); LOG_WRN("pairing cancelled"); }
static unsigned int app_passkey(struct bt_conn *c) { return gen_code ? gen_code : 123456; }
static struct bt_conn_auth_cb auth_cbs = { .cancel = pair_cancel };   // global: no IO, computers pair without a code
static const struct bt_conn_auth_cb auth_input = { .passkey_display = passkey_display, .app_passkey = app_passkey, .cancel = pair_cancel };   // MX Keys: type 123456 on the keyboard
static void pair_done(struct bt_conn *c, bool bonded)
{
    blue_stop();
    LOG_INF("pairing complete, bonded %d", bonded);
    int sl = slot_of(c);
    if (bonded && sl >= 0) slot_save(sl, bt_conn_get_dst(c));
    if (bonded && hid_dev_is_host_conn(c)) { identify_host(c, true); return; }
    if (pairing_mode) { k_work_cancel_delayable(&pairing_end_work); pairing_mode = false; pair_slot = -1; }
    if (bonded && sl >= 0 && setup_open) setup_event(0, sl);
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

// a phone read the NFC tag: open the setup window so the page can connect
void nfc_tapped(void)
{
    LOG_INF("nfc tag read by a phone");
    setup_begin(false, 5);
}

int cfg_status(char *buf, size_t n)
{
    char why_json[101];                                          // the newest bond removals and their reasons, plain characters only; short on purpose: a status over ~740 bytes is cut off by the GATT read
    { size_t j = 0; for (const char *q = why_log; *q && j + 1 < sizeof(why_json); q++) why_json[j++] = (*q >= 0x20 && *q < 0x7f && *q != '"' && *q != '\\') ? *q : '_'; why_json[j] = 0; }
    int o = snprintf(buf, n,
        "{\"v\":3,\"setup\":%d,\"step\":%d,\"code\":%u,\"phone\":%d,\"active\":%d,\"gap\":%d,\"kbd\":%d,\"mouse\":%d,\"pair\":%d,\"pairmx\":%d,\"sw\":%d,\"nsb\":%d,\"learn\":%d,\"nfc\":[%d,%d],\"kidle\":%d,\"why\":\"%s\",\"h\":[",
        setup_open, setup_step, (pairing_mode && pair_slot != 1) ? gen_code : 0, hid_dev_config_connected(),
        active, hid_dev_get_mouse_gap(), in[0].ready, in[1].ready, host_pair + 1, pairing_mode, sw_btn, ns_btn, learn_btn, nfc_field_count(), nfc_read_count(),
        last_key_ms ? (int)MIN(60000, k_uptime_get() - last_key_ms) : 60000, why_json);
    for (int h = 0; h < NUM_HOSTS && o > 0 && o < (int)n; h++) {
        char nm[20];
        json_name(nm, sizeof(nm), host_name[h]);
        o += snprintf(buf + o, n - o, "%s{\"n\":\"%s\",\"known\":%d,\"on\":%d,\"swap\":%d,\"nos\":%d,\"g\":%d,\"e\":%d}", h ? "," : "", nm,
                      host_known(h), hid_dev_connected(h), (mod_swap >> h) & 1, (nosleep_bits >> h) & 1, hid_dev_host_gap(h), hid_dev_host_even(h));
    }
    if (o > 0 && o < (int)n - 3) o += snprintf(buf + o, n - o, "]}");
    return o;
}

// Computers a and b trade places: addresses, names, Alt/Command swap and no-sleep go with the computer, and so do live links.
static void move_host(int a, int b)
{
    if (a < 0 || a >= NUM_HOSTS || b < 0 || b >= NUM_HOSTS || a == b) return;
    bt_addr_le_t ta = host_addr[a]; host_addr[a] = host_addr[b]; host_addr[b] = ta;
    char tn[16]; memcpy(tn, host_name[a], 16); memcpy(host_name[a], host_name[b], 16); memcpy(host_name[b], tn, 16);
    uint8_t ms = ((mod_swap >> a) & 1) | (((mod_swap >> b) & 1) << 1);              // bit 0 = old a, bit 1 = old b
    mod_swap = (mod_swap & ~((1 << a) | (1 << b))) | (((ms >> 1) & 1) << a) | ((ms & 1) << b);
    uint8_t ns = ((nosleep_bits >> a) & 1) | (((nosleep_bits >> b) & 1) << 1);
    nosleep_bits = (nosleep_bits & ~((1 << a) | (1 << b))) | (((ns >> 1) & 1) << a) | ((ns & 1) << b);
    if (active == a) active = b; else if (active == b) active = a;
    hid_dev_swap_hosts(a, b);
    for (int h = a, i = 0; i < 2; i++, h = b) {
        char k1[3] = { 'h', '0' + h, 0 }, k2[3] = { 'n', '0' + h, 0 };
        kvm_save(k1, &host_addr[h], sizeof(host_addr[h]));
        kvm_save(k2, host_name[h], strlen(host_name[h]));
    }
    kvm_save("swap", &mod_swap, 1);
    kvm_save("ns", &nosleep_bits, 1);
    mouse_save();
    LOG_INF("positions %d and %d traded places", a + 1, b + 1);
}

static void forget_host(int h)
{
    bt_addr_le_t old = host_addr[h];
    struct bt_conn *c = hid_dev_conn_for(h);
    memset(&host_addr[h], 0, sizeof(host_addr[h]));
    char key[3] = { 'h', '0' + h, 0 };
    kvm_save(key, &host_addr[h], sizeof(host_addr[h]));
    if (c) bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    forget_bond_both(&old, "config page forget");
    snprintf(host_name[h], sizeof(host_name[h]), "Computer %d", h + 1);       // the slot starts fresh: default label, Alt/Command swap and no-sleep off
    char nk[3] = { 'n', '0' + h, 0 };
    kvm_save(nk, host_name[h], strlen(host_name[h]));
    mod_swap &= ~(1 << h); nosleep_bits &= ~(1 << h);
    kvm_save("swap", &mod_swap, 1); kvm_save("ns", &nosleep_bits, 1);
    for (int k = h; k < NUM_HOSTS - 1; k++)                                   // close the gap: later computers move up one place
        if (!host_known(k) && host_known(k + 1)) move_host(k, k + 1);
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
    else if (!strcmp(word, "pairmx")) pairing_begin(-1);
    else if (!strcmp(word, "switch") && valid) switch_to(h);
    else if (!strcmp(word, "swap") && valid) {
        if (b) mod_swap |= 1 << h; else mod_swap &= ~(1 << h);
        kvm_save("swap", &mod_swap, 1);
        send_keys();
    } else if (!strcmp(word, "setup")) {
        if (a == 0) setup_end();
        else if (a == 1) { setup_close_windows(); setup_begin(true, 10); }
        else if (a == 2 && setup_open && setup_step < ST_DONE) k_work_reschedule(&setup_next_work, K_NO_WAIT);
    } else if (!strcmp(word, "learn") && (a == 1 || a == 2)) learn_begin(a);
    else if (!strcmp(word, "nosleep") && valid) nosleep_set(h, b);
    else if (!strcmp(word, "swbtn") && a >= 0 && a <= 8) { sw_btn = a; buttons_save(); }
    else if (!strcmp(word, "nsbtn") && a >= 0 && a <= 8) { ns_btn = a; buttons_save(); }
    else if (!strcmp(word, "stroke")) {            // "stroke <per> <n> <ms> <reps>" (diagnosis)
        int per = 0, n = 0, ms = 0, reps = 0;
        if (sscanf(cmd, "%*s %d %d %d %d", &per, &n, &ms, &reps) == 4) hid_dev_stroke(per, n, ms, reps);
    } else if (!strcmp(word, "mgap") && valid) {            // "mgap 2 15": computer 2 gets 15 ms between mouse reports
        hid_dev_set_host_mouse(h, b, hid_dev_host_even(h)); mouse_save();
    } else if (!strcmp(word, "meven") && valid) {           // "meven 2 1": computer 2 evens out big steps
        hid_dev_set_host_mouse(h, hid_dev_host_gap(h), b != 0); mouse_save();
    } else if (!strcmp(word, "gap")) {                      // all computers
        hid_dev_set_mouse_gap(a); mouse_save();
    } else if (!strcmp(word, "forget") && valid) forget_host(h);
    else if (!strcmp(word, "name") && valid) {
        const char *p = strchr(cmd, ' ');                 // "name 1 Mac": the rest of the line after the number
        if (p) p = strchr(p + 1, ' ');
        char tmp[16];
        json_name(tmp, sizeof(tmp), p ? p + 1 : "");
        if (!tmp[0]) { snprintf(tmp, sizeof(tmp), "Computer %d", a); name_tries[h] = 0; k_work_reschedule(&name_read_work, K_MSEC(300)); }   // empty: use its Bluetooth name
        strcpy(host_name[h], tmp);
        char key[3] = { 'n', '0' + h, 0 };
        kvm_save(key, host_name[h], strlen(host_name[h]));
    } else if (!strcmp(word, "pos") && valid) move_host(h, b - 1);        // "pos 3 1": computer 3 takes position 1, whoever was there takes its place
    else if (!strcmp(word, "bootloader")) { k_sleep(K_MSEC(100)); nrf_power_gpregret_set(NRF_POWER, 0, 0x57); sys_reboot(SYS_REBOOT_COLD);
    } else if (!strcmp(word, "reboot")) { k_sleep(K_MSEC(100)); sys_reboot(SYS_REBOOT_COLD); }
}

// The mouse link and the active computer's link both run at 7.5 ms. Depending on how their events line up on the one
// radio, the mouse's reports arrive evenly or in lumps. When they come in lumps, re-set the mouse link to the other of
// two close intervals: the controller then picks a new alignment, and this repeats until the stream is even.
static void mouse_reroll(void)
{
    static uint8_t ivl = 6;
    if (!in[1].conn || !in[1].ready) return;
    ivl = ivl == 6 ? 7 : 6;                                  // 7.5 ms <-> 8.75 ms
    struct bt_le_conn_param p = BT_LE_CONN_PARAM_INIT(ivl, ivl, 0, 400);
    LOG_INF("mouse link: lumpy arrivals, re-setting to %u.%02u ms (rc %d)", ivl * 5 / 4, (ivl * 125) % 100, bt_conn_le_param_update(in[1].conn, &p));
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

// No-sleep: every 30 s an F15 tap goes to each computer that has it on (it counts as activity and types nothing). It used to be a Left Shift
// tap, but five Shift presses in a row open the Windows Sticky Keys prompt with a beep, and at night nothing else interrupts the series.
// Never while the active computer is being typed on. The LEDs show the selected computer and its no-sleep state (see below).
static void ns_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(ns_work, ns_fn);
static void ns_fn(struct k_work *w)
{
    static unsigned n;
    n++;
    if ((n % 300) == 0 || (n % 300) == 1) {                    // press, then release 100 ms later
        bool press = (n % 300) == 0, anykey = false;
        for (int b = 0; b < 32; b++) anykey |= keys[b] != 0;
        for (int h = 0; h < NUM_HOSTS; h++) {
            if (!((nosleep_bits >> h) & 1) || !hid_dev_connected(h)) continue;
            if (h == active && anykey) continue;
            uint8_t rpt[8] = {0};
            if (press) rpt[2] = 0x6A;                    // F15: the key "keep awake" tools use; Windows and macOS ignore it
            hid_dev_send_keyboard(h, rpt);
        }
    }
    // One LED per computer: red = computer 1, blue = computer 2. The selected computer's LED is on; with no-sleep on it
    // dips off for 100 ms every 2 s. An unselected computer's LED is off; with no-sleep on it flashes on for 100 ms every 2 s.
    // The two blink one second apart. Green is for pairing: it blinks while any pairing window is open.
    // With more than two computers: 3 = red and blue together, 4 = red and blue together, blinking.
    bool lr = active == 0 || active == 2 || (active == 3 && (n % 10) < 5);
    bool lb = active == 1 || active == 2 || (active == 3 && (n % 10) < 5);
    for (int h = 0; h < NUM_HOSTS; h++)
        if (((nosleep_bits >> h) & 1) && (n % 20) == 5 * h) { if (h != 1) lr = !lr; if (h != 0) lb = !lb; }
    gpio_pin_set_dt(&led_r, lr);
    // Green is for pairing: fast blink while a pairing window is open and the hub is scanning or waiting for the device,
    // slow blink while the device is connected and waiting for you (the code to type), off when pairing is complete.
    bool g = false;
    if (passkey_wait) g = (n % 10) < 5;
    else if (pairing_mode || host_pair >= 0) g = (n % 2) == 0;
    else g = setup_open;                                   // steady green: the phone link is open
    gpio_pin_set_dt(&led, g);
    if (setup_open && setup_until && k_uptime_get() > setup_until) setup_end();
    if (!blue_active) gpio_pin_set_dt(&led_b, lb);
    k_work_reschedule(&ns_work, K_MSEC(100));
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
    hid_dev_set_reroll(mouse_reroll);
    hid_dev_id_setup();
    {   // every boot: the computers see the fixed address. It changes once (on the first boot with this firmware), then stays.
        static const uint8_t zero;
        if (new_id_pending) { new_id_pending = 0; kvm_save("newid", &zero, 1); }
        if (hid_dev_new_identity() > 0) unpair_why(1, NULL, "fixed Bluetooth address");     // only if it changed: the computers' old pairings belong to the old address
    }
    if (mouse_gap_saved >= 0) hid_dev_set_mouse_gap(mouse_gap_saved);       // older single setting: all computers
    if (mouse_saved_ok) for (int h = 0; h < NUM_HOSTS; h++) hid_dev_set_host_mouse(h, mouse_saved.gap[h], (mouse_saved.even >> h) & 1);
    bt_conn_auth_cb_register(&auth_cbs);
    bt_conn_auth_info_cb_register(&auth_info);
    for (int i = 0; i < NUM_INPUTS; i++) {
        static const struct bt_hogp_init_params hp = { .ready_cb = hogp_ready, .prep_error_cb = hogp_fail };
        bt_hogp_init(&in[i].hogp, &hp);
    }
    bt_foreach_bond(BT_ID_DEFAULT, bond_cb, NULL);
    auto_start();
    nfc_tag_init();
    hid_dev_adv_start();
    hid_dev_adv_start_later();                      // also starts the always-on beacon
    k_work_reschedule(&ns_work, K_MSEC(100));
    {   // a hub with nothing paired at all starts the setup wizard for the phone by itself
        static const uint8_t z[6];
        bool fresh = !memcmp(slot_addr[0].a.val, z, 6) && !memcmp(slot_addr[1].a.val, z, 6) && !host_known(0) && !host_known(1) && !host_known(2) && !host_known(3);
        if (fresh) setup_begin(true, 30);
        else setup_begin(false, 5);              // every power-up or reset: the phone link is open until it is closed on the page
    }
    const struct device *con = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    for (int n = 0;; n++) {
        unsigned char c;
        while (uart_poll_in(con, &c) == 0)           // type B on the UART: reboot into the UF2 bootloader
            if (c == 'P') pairing_begin(-1);
            else if (c == '1' || c == '2') host_pair_begin(c - '1');
            else if (c == 'Y') { hid_dev_beacon(!hid_dev_beacon_on()); LOG_INF("beacon %s", hid_dev_beacon_on() ? "on" : "off"); }
            else if (c == 'N') links_dump("asked");
            else if (c == 'J') hid_dev_jiggle(6000);
            else if (c == 'R') { LOG_INF("rebooting"); k_sleep(K_MSEC(100)); sys_reboot(SYS_REBOOT_COLD); }
            else if (c == 'I') { static const uint8_t one = 1; kvm_save("newid", &one, 1); LOG_INF("new Bluetooth address for the computers at the next boot: rebooting"); k_sleep(K_MSEC(300)); sys_reboot(SYS_REBOOT_COLD); }
            else if (c >= 'a' && c <= 'e') {          // test settings for the dot test: spacing and resampling
                static const struct { int gap; bool rs; } t[] = { {8, false}, {0, false}, {4, false}, {7, false}, {7, true} };
                hid_dev_set_mouse_gap(t[c - 'a'].gap); hid_dev_set_resample(t[c - 'a'].rs);
                LOG_INF("test setting %c: spacing %d ms, resampling %s", c, t[c - 'a'].gap, t[c - 'a'].rs ? "on" : "off");
            }
            else if (c >= 'f' && c <= 'k') {          // test: set the mouse link interval (units of 1.25 ms): f 7.5, g 8.75, h 10, i 11.25, j 12.5, k 15 ms
                static const uint8_t u[] = { 6, 7, 8, 9, 10, 12 };
                if (in[1].conn) {
                    struct bt_le_conn_param p = BT_LE_CONN_PARAM_INIT(u[c - 'f'], u[c - 'f'], 0, 400);
                    LOG_INF("mouse link: asking %u.%02u ms (rc %d)", u[c - 'f'] * 5 / 4, (u[c - 'f'] * 125) % 100, bt_conn_le_param_update(in[1].conn, &p));
                }
            }
            else if (c == 'l') learn_begin(1);
            else if (c == 'o') learn_begin(2);
            else if (c == 'Z') LOG_INF("mouse resampling %s", hid_dev_toggle_resample() ? "ON" : "off");
            else if (c == 'M') LOG_INF("mouse report spacing now %d ms", hid_dev_cycle_mouse_gap());
            else if (c == '3' || c == '4') switch_to(c - '3');
            else if (c == 'S') { mod_swap ^= 1 << active; kvm_save("swap", &mod_swap, 1); LOG_INF("computer %d Alt/Command swap %s", active + 1, (mod_swap >> active) & 1 ? "on" : "off"); send_keys(); }
            else if (c == 'K') { hid_dev_print_totals(); LOG_INF("active %d | connected: %d %d | known: %d %d | swap %02x | keyboard %d mouse %d", active + 1, hid_dev_connected(0), hid_dev_connected(1), host_known(0), host_known(1), mod_swap, in[0].ready, in[1].ready); }
            else if (c == 'C') forget_stale();
            else if (c == 'L') { LOG_INF("bonds id 0:"); bt_foreach_bond(0, bond_cb, NULL); LOG_INF("bonds id 1:"); bt_foreach_bond(1, bond_cb, NULL); }
            else if (c == 'U') forget_computers();
            else if (c == 'D') { host_dbg = !host_dbg; LOG_INF("report log %d", host_dbg); }
            else if (c == 'T') { blue_start(); k_work_schedule(&blue_test_stop, K_SECONDS(6)); LOG_INF("blue LED test, 6 s"); }
            else if (c == 'X') {
                unpair_why(BT_ID_DEFAULT, NULL, "key X"); unpair_why(1, NULL, "key X");
                memset(slot_addr, 0, sizeof(slot_addr)); memset(host_addr, 0, sizeof(host_addr));
                for (int i = 0; i < 2; i++) { char k[3] = { 's', '0' + i, 0 }, k2[3] = { 'h', '0' + i, 0 }; kvm_save(k, &slot_addr[i], sizeof(slot_addr[i])); kvm_save(k2, &host_addr[i], sizeof(host_addr[i])); }
                LOG_INF("all bonds and saved devices forgotten");
            }
            else if (c == 'W') LOG_INF("bond removals (newest first): %s", why_log[0] ? why_log : "none recorded");
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

