#include "hid_parse.h"
#include <string.h>

typedef struct {
    uint16_t usage_page;
    int32_t  logical_min;
    uint8_t  report_size, report_count, report_id;
} globals_t;

static uint32_t item_u(const uint8_t *p, int sz)
{
    uint32_t v = 0;
    for (int i = 0; i < sz; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static int32_t item_s(const uint8_t *p, int sz)
{
    uint32_t v = item_u(p, sz);
    if (sz == 1) return (int8_t)v;
    if (sz == 2) return (int16_t)v;
    return (int32_t)v;
}

int hid_parse_descriptor(const uint8_t *d, size_t len, hid_desc_t *out)
{
    globals_t g = {0}, stack[4];
    int sp = 0;
    uint16_t usages[HID_MAX_USAGES];
    uint16_t usage_pages[HID_MAX_USAGES];
    int n_usages = 0;
    uint16_t umin = 0, umax = 0;
    bool have_range = false;
    uint16_t bit_off[256] = {0};

    memset(out, 0, sizeof(*out));

    size_t i = 0;
    while (i < len) {
        uint8_t p = d[i++];
        if (p == 0xFE) {                     // long item: skip
            if (i + 1 >= len) break;
            i += 2 + d[i];
            continue;
        }
        int sz = (p & 3) == 3 ? 4 : (p & 3);
        if (i + sz > len) break;
        const uint8_t *v = &d[i];
        i += sz;
        uint8_t type = (p >> 2) & 3, tag = p >> 4;

        if (type == 1) {                     // Global
            switch (tag) {
            case 0: g.usage_page = item_u(v, sz); break;
            case 1: g.logical_min = item_s(v, sz); break;
            case 7: g.report_size = item_u(v, sz); break;
            case 8: g.report_id = item_u(v, sz); break;
            case 9: g.report_count = item_u(v, sz); break;
            case 10: if (sp < 4) stack[sp++] = g; break;
            case 11: if (sp > 0) g = stack[--sp]; break;
            }
        } else if (type == 2) {              // Local
            uint32_t u = item_u(v, sz);
            switch (tag) {
            case 0:
                if (n_usages < HID_MAX_USAGES) {
                    usages[n_usages] = u & 0xFFFF;
                    usage_pages[n_usages] = sz == 4 ? u >> 16 : 0;
                    n_usages++;
                }
                break;
            case 1: umin = u & 0xFFFF; have_range = true; break;
            case 2: umax = u & 0xFFFF; have_range = true; break;
            }
        } else if (type == 0) {              // Main
            if (tag == 8) {                  // Input
                uint8_t flags = item_u(v, sz);
                uint16_t bits = g.report_size * g.report_count;
                if (!(flags & 1) && out->n < HID_MAX_FIELDS && g.report_size <= 32) {
                    hid_field_t *f = &out->f[out->n++];
                    f->report_id = g.report_id;
                    f->usage_page = (n_usages && usage_pages[0]) ? usage_pages[0] : g.usage_page;
                    f->bit_off = bit_off[g.report_id];
                    f->bit_size = g.report_size;
                    f->count = g.report_count;
                    f->logical_min = g.logical_min;
                    f->flags = flags;
                    if (have_range) {
                        f->usage_min = umin;
                        f->usage_max = umax;
                    } else {
                        f->n_usages = n_usages;
                        memcpy(f->usages, usages, n_usages * sizeof(uint16_t));
                    }
                }
                bit_off[g.report_id] += bits;
            }
            // Any main item clears local state.
            n_usages = 0;
            have_range = false;
            umin = umax = 0;
        }
    }
    return out->n;
}

static uint32_t get_bits(const uint8_t *data, size_t len, uint32_t off, uint8_t size)
{
    uint32_t v = 0;
    for (uint8_t b = 0; b < size; b++) {
        uint32_t bit = off + b;
        if ((bit >> 3) >= len) break;
        if ((data[bit >> 3] >> (bit & 7)) & 1) v |= 1u << b;
    }
    return v;
}

static int32_t field_value(const hid_field_t *f, const uint8_t *data, size_t len, int idx)
{
    uint32_t raw = get_bits(data, len, f->bit_off + idx * f->bit_size, f->bit_size);
    if (f->logical_min < 0 && f->bit_size < 32 && (raw & (1u << (f->bit_size - 1))))
        raw |= ~0u << f->bit_size;           // sign-extend
    return (int32_t)raw;
}

// Usage for element idx of a variable field.
static uint16_t var_usage(const hid_field_t *f, int idx)
{
    if (f->n_usages) return f->usages[idx < f->n_usages ? idx : f->n_usages - 1];
    return f->usage_min + idx;
}

// Usage selected by value v of an array field (0 = none).
static uint16_t arr_usage(const hid_field_t *f, int32_t v)
{
    int32_t index = v - f->logical_min;
    if (index < 0) return 0;
    if (f->n_usages) return index < f->n_usages ? f->usages[index] : 0;
    if (f->usage_min + index > f->usage_max) return 0;
    return f->usage_min + index;
}

void hid_decode_input(const hid_desc_t *desc, uint8_t report_id,
                      const uint8_t *data, size_t len, hid_input_t *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < desc->n; i++) {
        const hid_field_t *f = &desc->f[i];
        if (f->report_id != report_id) continue;
        bool variable = f->flags & 2;

        if (f->usage_page == 0x07) {                         // Keyboard
            out->has_keys = true;
            if (variable) {
                for (int k = 0; k < f->count; k++) {
                    uint16_t u = var_usage(f, k);
                    bm_set(out->key_cover, u);
                    if (field_value(f, data, len, k)) bm_set(out->key_down, u);
                }
            } else {
                uint16_t lo = f->n_usages ? 0 : f->usage_min;
                uint16_t hi = f->n_usages ? 255 : f->usage_max;
                for (uint32_t u = lo; u <= hi && u < 256; u++) bm_set(out->key_cover, u);
                for (int k = 0; k < f->count; k++) {
                    uint16_t u = arr_usage(f, field_value(f, data, len, k));
                    if (u > 3) bm_set(out->key_down, u);    // 1..3 are error codes
                }
            }
        } else if (f->usage_page == 0x09 && variable) {      // Buttons
            out->has_mouse = true;
            for (int k = 0; k < f->count; k++) {
                uint16_t u = var_usage(f, k);
                if (u >= 1 && u <= 16 && field_value(f, data, len, k))
                    out->buttons |= 1u << (u - 1);
            }
        } else if (f->usage_page == 0x01 && variable) {      // X / Y / wheel
            for (int k = 0; k < f->count; k++) {
                uint16_t u = var_usage(f, k);
                int32_t val = field_value(f, data, len, k);
                if (u == 0x30) { out->x = val; out->has_mouse = true; }
                else if (u == 0x31) { out->y = val; out->has_mouse = true; }
                else if (u == 0x38) { out->wheel = val; out->has_mouse = true; }
            }
        } else if (f->usage_page == 0x0C) {                  // Consumer
            if (variable) {
                for (int k = 0; k < f->count; k++) {
                    uint16_t u = var_usage(f, k);
                    int32_t val = field_value(f, data, len, k);
                    if (u == 0x238 && (f->flags & 4)) {      // AC Pan (horizontal scroll)
                        out->pan = val;
                        out->has_mouse = true;
                    } else {
                        out->has_consumer = true;
                        if (val && !out->consumer) out->consumer = u;
                    }
                }
            } else {
                out->has_consumer = true;
                for (int k = 0; k < f->count; k++) {
                    uint16_t u = arr_usage(f, field_value(f, data, len, k));
                    if (u && !out->consumer) out->consumer = u;
                }
            }
        }
    }
}
