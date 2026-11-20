#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <bluetooth/services/hids.h>
#include "hid_dev.h"

LOG_MODULE_REGISTER(hid_dev, LOG_LEVEL_INF);

// Report IDs in our own descriptor (same map as main/hid_dev.c on the ESP32)
#define RID_KBD   1
#define RID_MOUSE 2
#define RID_CONS  3

// indexes in the HIDS input report table
enum { IN_KBD = 0, IN_MOUSE = 1, IN_CONS = 2 };

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

BT_HIDS_DEF(hids, 1, 8, 7, 2);       // report lengths: LED output, keyboard, mouse, consumer inputs

#define MOUSE_TICK_MS 20            // watchdog and statistics only; sending is paced by completions

// the links that are connected to us as a peripheral. host >= 0: computer 1 or 2; -1: connected but not identified yet; -2: a phone doing setup
#define NUM_PERIPH (NUM_HOSTS + 1)                // the computers and a phone doing setup
static struct { struct bt_conn *conn; int host; } hc[NUM_PERIPH];
static int active_host;
static int idle_tries[NUM_HOSTS];                // asks to relax an idle computer's link that macOS undid

static struct k_work_delayable link_work, mouse_work;
K_THREAD_STACK_DEFINE(mouse_q_stack, 3072);
static struct k_work_q mouse_q;                  // own thread: sending a notification may block, and must not hold up the Bluetooth threads or the system queue
static void link_fn(struct k_work *w);
void links_dump(const char *why);
static void mouse_tick_fn(struct k_work *w);
static K_MUTEX_DEFINE(send_lock);       // bt_hids_inp_rep_send must not run from two threads at once

static int find(const struct bt_conn *c) { for (int i = 0; i < NUM_PERIPH; i++) if (hc[i].conn == c) return i; return -1; }
static int count(void) { int n = 0; for (int i = 0; i < NUM_PERIPH; i++) if (hc[i].conn) n++; return n; }

bool hid_dev_is_host_conn(const struct bt_conn *c) { return find(c) >= 0; }
int hid_dev_host_of(const struct bt_conn *c) { int i = find(c); return i < 0 ? -1 : hc[i].host; }
struct bt_conn *hid_dev_conn_for(int h) { for (int i = 0; i < NUM_PERIPH; i++) if (hc[i].conn && hc[i].host == h) return hc[i].conn; return NULL; }
bool hid_dev_connected(int h) { return hid_dev_conn_for(h) != NULL; }
struct bt_conn *hid_dev_pending(void) { for (int i = 0; i < NUM_PERIPH; i++) if (hc[i].conn && hc[i].host == -1) return hc[i].conn; return NULL; }
// a link that is using the configuration service (a phone during setup): it is not a computer and must not be refused
void hid_dev_mark_config(struct bt_conn *c) { int i = find(c); if (i >= 0 && hc[i].host == -1) hc[i].host = -2; }
int hid_dev_disconnect_config(void) { int n = 0; for (int i = 0; i < NUM_PERIPH; i++) if (hc[i].conn && hc[i].host == -2) { bt_conn_disconnect(hc[i].conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN); n++; } return n; }
bool hid_dev_config_connected(void) { for (int i = 0; i < NUM_PERIPH; i++) if (hc[i].conn && hc[i].host == -2) return true; return false; }

static void leds_handler(struct bt_hids_rep *rep, struct bt_conn *conn, bool write)
{
    int h = hid_dev_host_of(conn);
    if (write && rep->size >= 1 && h >= 0) hid_dev_on_leds(h, rep->data[0]);
}

void hid_dev_init(void)
{
    k_work_queue_start(&mouse_q, mouse_q_stack, K_THREAD_STACK_SIZEOF(mouse_q_stack), K_PRIO_PREEMPT(6), NULL);
    k_thread_name_set(&mouse_q.thread, "mouse_q");
    k_work_init_delayable(&mouse_work, mouse_tick_fn);
    k_work_init_delayable(&link_work, link_fn);
    k_work_reschedule_for_queue(&mouse_q, &mouse_work, K_MSEC(MOUSE_TICK_MS));
    struct bt_hids_init_param p = { 0 };
    p.rep_map.data = report_map;
    p.rep_map.size = sizeof(report_map);
    p.info.bcd_hid = 0x0111;
    p.info.b_country_code = 0x21;
    p.info.flags = BT_HIDS_NORMALLY_CONNECTABLE;

    static const uint8_t in_id[3]   = { RID_KBD, RID_MOUSE, RID_CONS };
    static const uint8_t in_size[3] = { 8, 7, 2 };
    for (int i = 0; i < 3; i++) {
        p.inp_rep_group_init.reports[i].id = in_id[i];
        p.inp_rep_group_init.reports[i].size = in_size[i];
    }
    p.inp_rep_group_init.cnt = 3;

    p.outp_rep_group_init.reports[0].id = RID_KBD;
    p.outp_rep_group_init.reports[0].size = 1;
    p.outp_rep_group_init.reports[0].handler = leds_handler;
    p.outp_rep_group_init.cnt = 1;

    int rc = bt_hids_init(&hids, &p);
    if (rc) LOG_ERR("bt_hids_init %d", rc);
}

void hid_dev_conn_open(struct bt_conn *c)
{
    int i = find(NULL);
    if (i < 0) { bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN); return; }
    int rc = bt_hids_connected(&hids, c);
    if (rc) LOG_ERR("bt_hids_connected %d", rc);
    hc[i].conn = bt_conn_ref(c);
    hc[i].host = -1;
}

void hid_dev_conn_close(struct bt_conn *c)
{
    int i = find(c);
    if (i < 0) return;
    bt_hids_disconnected(&hids, c);
    if (hc[i].host == active_host) hid_dev_set_active(active_host);       // drop what was queued for it
    bt_conn_unref(hc[i].conn);
    hc[i].conn = NULL;
    hc[i].host = -1;
}

void hid_dev_assign(struct bt_conn *c, int h)
{
    int i = find(c);
    if (i < 0) return;
    for (int j = 0; j < NUM_PERIPH; j++)                      // the same computer reconnected: its older link is stale
        if (j != i && hc[j].conn && hc[j].host == h) bt_conn_disconnect(hc[j].conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    hc[i].host = h;
    k_work_reschedule(&link_work, K_SECONDS(3));
}

void hid_dev_swap_hosts(int a, int b)
{
    for (int i = 0; i < NUM_PERIPH; i++) {
        if (!hc[i].conn) continue;
        if (hc[i].host == a) hc[i].host = b; else if (hc[i].host == b) hc[i].host = a;
    }
    int t = idle_tries[a]; idle_tries[a] = idle_tries[b]; idle_tries[b] = t;
    if (active_host == a) active_host = b; else if (active_host == b) active_host = a;
}

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xC2, 0x03),   // 0x03C2 mouse: macOS gives it a 7.5 ms link
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL), BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};
static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static uint8_t adv_id = BT_ID_DEFAULT;

void hid_dev_id_setup(void)
{
    bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
    size_t n = CONFIG_BT_ID_MAX;
    bt_id_get(addrs, &n);
    int id = n >= 2 ? 1 : bt_id_create(NULL, NULL);
    if (id < 0) { LOG_ERR("bt_id_create %d", id); return; }
    adv_id = id;
    n = CONFIG_BT_ID_MAX;
    bt_id_get(addrs, &n);
    char as[BT_ADDR_LE_STR_LEN]; bt_addr_le_to_str(&addrs[id], as, sizeof(as));
    LOG_INF("computers see identity %d: %s", id, as);
}

static struct bt_le_ext_adv *adv_set;

// Advertise while there is room for another computer.
void hid_dev_adv_start(void)
{
    if (count() >= NUM_PERIPH) return;
    static int64_t last_fail;
    int64_t now = k_uptime_get();
    if (last_fail && now - last_fail < 500) return;    // never loop: after a failure wait 500 ms
    int rc;
    if (!adv_set) {      // own advertising set: its address may differ from the one the scan and connect use
        struct bt_le_adv_param ap = BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY,
                                                         BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);
        ap.id = adv_id;
        rc = bt_le_ext_adv_create(&ap, NULL, &adv_set);
        if (!rc) rc = bt_le_ext_adv_set_data(adv_set, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
        if (rc) { LOG_ERR("advertising set %d", rc); adv_set = NULL; return; }
    }
    rc = bt_le_ext_adv_start(adv_set, BT_LE_EXT_ADV_START_DEFAULT);
    if (rc && rc != -EALREADY) { LOG_ERR("advertising start %d", rc); last_fail = now; }
    else if (!rc) LOG_INF("advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
}

// Always-on, slow, scannable but not connectable: lets a browser list the hub when both computers are
// connected and the connectable set is off. (The browser then talks over the link its computer already has.)
static struct bt_le_ext_adv *beacon;
static void beacon_start(void)
{
    int rc;
    if (!beacon) {
        struct bt_le_adv_param ap = BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_SCANNABLE | BT_LE_ADV_OPT_USE_IDENTITY, 0x0640, 0x0780, NULL);   // 1 - 1.2 s
        ap.id = adv_id;
        rc = bt_le_ext_adv_create(&ap, NULL, &beacon);
        if (!rc) rc = bt_le_ext_adv_set_data(beacon, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
        if (rc) { LOG_ERR("beacon set %d", rc); beacon = NULL; return; }
    }
    rc = bt_le_ext_adv_start(beacon, BT_LE_EXT_ADV_START_DEFAULT);
    if (rc && rc != -EALREADY) LOG_ERR("beacon start %d", rc);
}

static bool beacon_wanted = true;
void hid_dev_beacon(bool on)
{
    beacon_wanted = on;
    if (on) beacon_start(); else if (beacon) bt_le_ext_adv_stop(beacon);
}
bool hid_dev_beacon_on(void) { return beacon_wanted; }

static void adv_fn(struct k_work *w) { hid_dev_adv_start(); if (beacon_wanted) beacon_start(); }
static K_WORK_DELAYABLE_DEFINE(adv_work, adv_fn);
// restart after a connection: the controller needs a moment to finish stopping the old set
void hid_dev_adv_start_later(void) { k_work_reschedule(&adv_work, K_MSEC(300)); }

static int send(int h, uint8_t idx, const uint8_t *r, uint8_t len)
{
    struct bt_conn *c = hid_dev_conn_for(h);
    if (!c) return -ENOTCONN;
    k_mutex_lock(&send_lock, K_FOREVER);
    int rc = bt_hids_inp_rep_send(&hids, c, idx, r, len, NULL);
    k_mutex_unlock(&send_lock);
    return rc;
}

int hid_dev_send_keyboard(int h, const uint8_t rpt[8]) { return send(h, IN_KBD, rpt, 8); }
int hid_dev_send_consumer(int h, uint16_t usage)
{
    uint8_t r[2] = { usage & 0xFF, usage >> 8 };
    return send(h, IN_CONS, r, 2);
}

// ---- mouse accumulator: the MX Master sends ~100 reports/s, the computer's link takes fewer.
// Deltas are summed and sent as soon as the previous notification has gone out; a failed send keeps the sum.
static struct {
    int dx, dy, wheel, pan;
    uint8_t buttons, sent_buttons;
} mac;

static int clamp16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static int clamp8(int v) { return v > 127 ? 127 : v < -127 ? -127 : v; }

static unsigned st_in, st_ok, st_fail, st_maxd, st_maxgap, st_stuck, st_maxdone, st_slowdone;
static int64_t st_last_send;
static int64_t tot_in_x, tot_in_y, tot_out_x, tot_out_y, abs_in_x, abs_in_y;     // running sums since boot (diagnosis: is movement conserved?)
static int64_t st_t;
static uint32_t last_in_us;
static void (*reroll_cb)(void);                // asks main.c to re-set the mouse link when its arrivals are lumpy
static unsigned win_gaps, win_slow, rerolls;
static int64_t win_start, last_reroll;
static unsigned in_gap_hist[7];            // gaps between mouse reports at the hub: <=4, <=9, <=14, <=19, <=29, <=49, >49 ms (only while moving)
static volatile bool mouse_resample;       // spread each burst of movement over two steps so the steps stay even (UART key Z)
static volatile int mouse_gap_ms = 8;     // minimum time between mouse reports (0 = as soon as the previous one is out); 8 ms felt best on the Mac
static int64_t last_send_ms;
static atomic_t inflight;                 // a mouse notification is queued and not yet sent over the air
static int64_t inflight_since;

static void mouse_done(struct bt_conn *c, void *user_data)
{
    { unsigned d = k_uptime_get() - inflight_since; if (d > st_maxdone) st_maxdone = d; if (d > 20) st_slowdone++; }
    atomic_clear(&inflight);
    k_work_reschedule_for_queue(&mouse_q, &mouse_work, K_NO_WAIT);     // send what was summed up meanwhile, at once
}

static void mouse_flush(void)
{
    if (atomic_get(&inflight)) return;
    struct bt_conn *c = hid_dev_conn_for(active_host);
    if (!c) return;
    k_mutex_lock(&send_lock, K_FOREVER);
    if (atomic_get(&inflight)) { k_mutex_unlock(&send_lock); return; }
    int gap = mouse_gap_ms;
    if (gap) {                                          // keep the reports evenly spaced
        int64_t wait = last_send_ms + gap - k_uptime_get();
        if (wait > 0) { k_work_reschedule_for_queue(&mouse_q, &mouse_work, K_MSEC(wait)); k_mutex_unlock(&send_lock); return; }
    }
    unsigned key = irq_lock();
    int dx = clamp16(mac.dx), dy = clamp16(mac.dy), wh = clamp8(mac.wheel), pn = clamp8(mac.pan);
    uint8_t b = mac.buttons;
    irq_unlock(key);
    if (mouse_resample) {                               // send half of what is pending (rounded up); the rest follows on the next step
        dx = dx > 0 ? (dx + 1) / 2 : dx < 0 ? (dx - 1) / 2 : 0;
        dy = dy > 0 ? (dy + 1) / 2 : dy < 0 ? (dy - 1) / 2 : 0;
    }
    if (!dx && !dy && !wh && !pn && b == mac.sent_buttons) { k_mutex_unlock(&send_lock); return; }
    { unsigned m = MAX(abs(dx), abs(dy)); if (m > st_maxd) st_maxd = m;
      int64_t t = k_uptime_get(); if (st_last_send && t - st_last_send < 300 && (unsigned)(t - st_last_send) > st_maxgap) st_maxgap = t - st_last_send; st_last_send = t; }
    uint8_t r[7] = { b, dx & 0xff, dx >> 8, dy & 0xff, dy >> 8, (uint8_t)wh, (uint8_t)pn };
    atomic_set(&inflight, 1);
    inflight_since = k_uptime_get();
    last_send_ms = inflight_since;
    if (bt_hids_inp_rep_send(&hids, c, IN_MOUSE, r, 7, mouse_done)) { atomic_clear(&inflight); st_fail++; k_mutex_unlock(&send_lock); return; }
    st_ok++;
    key = irq_lock();
    mac.dx -= dx; mac.dy -= dy; mac.wheel -= wh; mac.pan -= pn;
    tot_out_x += dx; tot_out_y += dy;
    irq_unlock(key);
    mac.sent_buttons = b;
    k_mutex_unlock(&send_lock);
}

static void mouse_tick_fn(struct k_work *w)
{
    int64_t now = k_uptime_get();
    if (atomic_get(&inflight) && now - inflight_since > 100) { atomic_clear(&inflight); st_stuck++; }   // lost completion
    mouse_flush();
    if (now - win_start >= 1000) {              // lumpy arrivals from the mouse: new link alignment
        // Short bursts of movement count too (dot clicking is stroke, pause, stroke): 25 gaps are enough, and the
        // window must be bad twice in a row so one slow hand movement does not trigger it.
        static int bad_windows;
        bool bad = win_gaps >= 25 && win_slow * 100 > win_gaps * 15;
        if (win_gaps >= 25) bad_windows = bad ? bad_windows + 1 : 0;
        if (reroll_cb && bad_windows >= 2 && now - last_reroll > 5000) { last_reroll = now; rerolls++; bad_windows = 0; reroll_cb(); }
        win_gaps = win_slow = 0; win_start = now;
    }
    if (now - st_t >= 2000) {
        if (st_in) {
            struct bt_conn_info ci = {0};
            struct bt_conn *c = hid_dev_conn_for(active_host);
            if (c) bt_conn_get_info(c, &ci);
            LOG_INF("mouse 2 s, computer %d (link %u.%02u ms lat %u): in %u, sent %u, failed %u | max step %u, max gap %u ms, stuck %u, completion max %u ms (%u over 20) | totals in %d,%d out %d,%d pending %d,%d | input gaps ms <=4:%u <=9:%u <=14:%u <=19:%u <=29:%u <=49:%u >49:%u rerolls %u", active_host + 1,
                    ci.le.interval * 5 / 4, (ci.le.interval * 125) % 100, ci.le.latency, st_in, st_ok, st_fail, st_maxd, st_maxgap, st_stuck, st_maxdone, st_slowdone, (int)tot_in_x, (int)tot_in_y, (int)tot_out_x, (int)tot_out_y, mac.dx, mac.dy,
                    in_gap_hist[0], in_gap_hist[1], in_gap_hist[2], in_gap_hist[3], in_gap_hist[4], in_gap_hist[5], in_gap_hist[6], rerolls);
        }
        if (st_slowdone >= 2) { static int64_t last_dump; if (now - last_dump > 10000) { last_dump = now; links_dump("slow completions"); } }
        memset(in_gap_hist, 0, sizeof(in_gap_hist));
        st_in = st_ok = st_fail = st_maxd = st_maxgap = st_stuck = st_maxdone = st_slowdone = 0; st_t = now;
    }
    k_work_reschedule_for_queue(&mouse_q, &mouse_work, K_MSEC(MOUSE_TICK_MS));
}

void hid_dev_set_resample(bool on) { mouse_resample = on; }
bool hid_dev_toggle_resample(void) { mouse_resample = !mouse_resample; return mouse_resample; }
void hid_dev_set_mouse_gap(int ms) { mouse_gap_ms = ms < 0 ? 0 : ms > 40 ? 40 : ms; }
int hid_dev_get_mouse_gap(void) { return mouse_gap_ms; }

int hid_dev_cycle_mouse_gap(void)
{
    static const int gaps[] = { 0, 8, 15, 22 };
    static int i = 1;                                   // 8 ms is the default
    i = (i + 1) % ARRAY_SIZE(gaps);
    mouse_gap_ms = gaps[i];
    return mouse_gap_ms;
}

// diagnosis: +1/-1 count at 100 Hz for a few seconds (no net movement), so delivery timing can be measured without a hand
static int jiggle_left;
static void jiggle_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(jiggle_work, jiggle_fn);
static void jiggle_fn(struct k_work *w)
{
    if (jiggle_left-- <= 0) return;
    hid_dev_mouse_in(0, jiggle_left & 1 ? 1 : -1, 0, 0, 0);
    k_work_reschedule_for_queue(&mouse_q, &jiggle_work, K_MSEC(10));
}
void hid_dev_jiggle(int ms) { jiggle_left = ms / 10; k_work_reschedule_for_queue(&mouse_q, &jiggle_work, K_NO_WAIT); }

// diagnosis: reps x (n reports of -per counts up, then n reports of +per counts down), one report every ms_gap ms
static struct { int per, n, ms, reps, i, phase; } stroke;
static void stroke_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(stroke_work, stroke_fn);
static void stroke_fn(struct k_work *w)
{
    if (stroke.reps <= 0) return;
    int dir = (stroke.phase & 1) ? 1 : -1;
    hid_dev_mouse_in(0, 0, dir * stroke.per, 0, 0);
    if (++stroke.i >= stroke.n) {
        stroke.i = 0;
        stroke.phase++;
        if (stroke.phase >= 2) { stroke.phase = 0; stroke.reps--; }
        k_work_reschedule_for_queue(&mouse_q, &stroke_work, K_MSEC(150));   // pause between strokes
        return;
    }
    k_work_reschedule_for_queue(&mouse_q, &stroke_work, K_MSEC(stroke.ms));
}
void hid_dev_stroke(int per, int n, int ms, int reps)
{
    stroke.per = per; stroke.n = n; stroke.ms = ms; stroke.reps = reps; stroke.i = 0; stroke.phase = 0;
    k_work_reschedule_for_queue(&mouse_q, &stroke_work, K_NO_WAIT);
}

void hid_dev_print_totals(void)
{
    LOG_INF("TOTALS in %d,%d out %d,%d pending %d,%d | path in %d,%d", (int)tot_in_x, (int)tot_in_y, (int)tot_out_x, (int)tot_out_y, mac.dx, mac.dy, (int)abs_in_x, (int)abs_in_y);
}

void hid_dev_set_reroll(void (*cb)(void)) { reroll_cb = cb; }

void hid_dev_mouse_in(uint8_t buttons, int dx, int dy, int wheel, int pan)
{
    st_in++;
    { uint32_t now = k_cyc_to_us_floor32(k_cycle_get_32()), gap = (now - last_in_us) / 1000; last_in_us = now;
      if (gap < 120) { win_gaps++; if (gap >= 20) win_slow++; }
      if (gap < 120) in_gap_hist[gap <= 4 ? 0 : gap <= 9 ? 1 : gap <= 14 ? 2 : gap <= 19 ? 3 : gap <= 29 ? 4 : gap <= 49 ? 5 : 6]++; }
    unsigned key = irq_lock();
    mac.dx += dx; mac.dy += dy; mac.wheel += wheel; mac.pan += pan;
    tot_in_x += dx; tot_in_y += dy; abs_in_x += abs(dx); abs_in_y += abs(dy);
    mac.buttons = buttons;
    irq_unlock(key);
    k_work_reschedule_for_queue(&mouse_q, &mouse_work, K_NO_WAIT);   // the mouse thread sends; this thread never blocks on it
}

// a new computer gets the mouse: nothing meant for the old one may leak over
void hid_dev_set_active(int h)
{
    unsigned key = irq_lock();
    active_host = h;
    mac.dx = mac.dy = mac.wheel = mac.pan = 0;
    mac.sent_buttons = 0;
    for (int i = 0; i < NUM_HOSTS; i++) idle_tries[i] = 0;
    atomic_clear(&inflight);
    irq_unlock(key);
    k_work_reschedule(&link_work, K_MSEC(200));
}

// The computers are the central and pick the link timing; macOS and Windows often choose a slow
// one (15 ms with latency 22, or 45 ms). Only the ACTIVE computer needs a fast link: it gets exactly
// 7.5 ms, latency 0. The idle one gets a relaxed 30-50 ms, which leaves the radio time to the others
// (two fast computer links plus the mouse made the controller drop a link: reason 0x28).
// One request at a time: overlapping link updates clash.
static void link_fn(struct k_work *w)
{
    bool any = false, asked = false;
    for (int pass = 0; pass < 2 && !asked; pass++) {            // pass 0: the active computer first
        for (int i = 0; i < NUM_HOSTS && !asked; i++) {
            struct bt_conn_info ci;
            if (!hc[i].conn || hc[i].host < 0 || bt_conn_get_info(hc[i].conn, &ci)) continue;
            bool is_active = hc[i].host == active_host;
            if (is_active != (pass == 0)) continue;
            any = true;
            bool bad = is_active ? (ci.le.interval > 6 || ci.le.latency > 0)
                                 : (ci.le.interval < 24 || ci.le.interval > 40 || ci.le.latency > 0);
            if (!bad) { if (is_active) idle_tries[hc[i].host] = 0; continue; }
            if (!is_active && idle_tries[hc[i].host] >= 2) continue;     // macOS puts it back each time: leave it
            if (!is_active) idle_tries[hc[i].host]++;
            static const struct bt_le_conn_param fast = BT_LE_CONN_PARAM_INIT(6, 6, 0, 400);      // macOS takes the top of a range
            static const struct bt_le_conn_param idle = BT_LE_CONN_PARAM_INIT(24, 40, 0, 400);
            int rc = bt_conn_le_param_update(hc[i].conn, is_active ? &fast : &idle);
            LOG_INF("computer %d link is %u.%02u ms latency %u, asking %s (rc %d)", hc[i].host + 1,
                    ci.le.interval * 5 / 4, (ci.le.interval * 125) % 100, ci.le.latency,
                    is_active ? "7.5 ms" : "30-50 ms", rc);
            asked = true;
        }
    }
    if (any) k_work_reschedule(&link_work, asked ? K_SECONDS(2) : K_SECONDS(3));
}
