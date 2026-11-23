// The hub keeps its own log: about the last 300 lines in RAM (roughly half an hour of connection events), readable over
// Bluetooth (config service characteristic ...0004) so nothing has to be unplugged to see what happened.
// It is a Zephyr log backend, so it receives everything the console receives, minus the busy mouse statistics.
#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/logging/log_core.h>
#include "ringlog.h"

#define N_ENT 300
#define TXT   150
#define TS    19                       // length of the "[00:00:03.001,495] " time stamp in front of every line

struct ent { uint32_t seq; uint16_t rep; char text[TXT]; };
static struct ent ring[N_ENT];
static uint32_t next_seq = 1;
static struct k_spinlock lock;
static char line[TXT];
static size_t line_len;

static void push(const char *text)
{
    if (strstr(text, "mouse 2 s,") || !strncmp(text, "alive ", 6) || strstr(text, "lumpy arrivals") || strstr(text, "hub: slot 1 link ")) return;   // the pointer statistics and the console heartbeat repeat every 2 s: not worth the space
    k_spinlock_key_t k = k_spin_lock(&lock);
    struct ent *last = next_seq > 1 ? &ring[(next_seq - 2) % N_ENT] : NULL;
    if (last && strlen(text) > TS && strlen(last->text) > TS && !strcmp(last->text + TS, text + TS) && last->rep < 60000) {
        last->rep++;                                                         // the same line again (a computer that keeps reconnecting): count it
    } else {
        struct ent *e = &ring[(next_seq - 1) % N_ENT];
        e->seq = next_seq++; e->rep = 1;
        strncpy(e->text, text, TXT - 1); e->text[TXT - 1] = 0;
    }
    k_spin_unlock(&lock, k);
}

static int char_out(uint8_t *data, size_t length, void *ctx)
{
    for (size_t i = 0; i < length; i++) {
        char c = data[i];
        if (c == '\n' || c == '\r') { if (line_len) { line[line_len] = 0; push(line); line_len = 0; } }
        else if (line_len < TXT - 1) line[line_len++] = c;
    }
    return length;
}
static uint8_t out_buf[160];
LOG_OUTPUT_DEFINE(ringlog_output, char_out, out_buf, sizeof(out_buf));

static void process(const struct log_backend *const backend, union log_msg_generic *msg)
{
    log_format_func_t f = log_format_func_t_get(LOG_OUTPUT_TEXT);
    f(&ringlog_output, &msg->log, log_backend_std_get_flags() & ~LOG_OUTPUT_FLAG_COLORS);
}
static void init_cb(const struct log_backend *const backend) {}
static void panic_cb(const struct log_backend *const backend) {}
static void dropped_cb(const struct log_backend *const backend, uint32_t cnt) { log_backend_std_dropped(&ringlog_output, cnt); }
static const struct log_backend_api ringlog_api = { .process = process, .init = init_cb, .panic = panic_cb, .dropped = dropped_cb };
LOG_BACKEND_DEFINE(ringlog_backend, ringlog_api, true);

// Text of the lines with sequence number >= from, "seq|line" (and " (xN)" for repeats), whole lines only, up to n bytes.
size_t ringlog_read(uint32_t from, char *out, size_t n)
{
    size_t used = 0;
    k_spinlock_key_t k = k_spin_lock(&lock);
    uint32_t oldest = next_seq > N_ENT ? next_seq - N_ENT : 1;
    for (uint32_t s = from > oldest ? from : oldest; s < next_seq; s++) {
        const struct ent *e = &ring[(s - 1) % N_ENT];
        int w = e->rep > 1 ? snprintf(out + used, n - used, "%u|%s (x%u)\n", (unsigned)e->seq, e->text, e->rep)
                           : snprintf(out + used, n - used, "%u|%s\n", (unsigned)e->seq, e->text);
        if (w < 0 || (size_t)w >= n - used) { out[used] = 0; break; }       // does not fit: leave it for the next read
        used += w;
    }
    k_spin_unlock(&lock, k);
    return used;
}
