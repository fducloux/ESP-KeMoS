// Minimal HID report-descriptor parser: enough to pull keyboard keys,
// mouse buttons/motion/wheel and consumer (media) keys out of input reports.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define HID_MAX_FIELDS   64
#define HID_MAX_USAGES   16

typedef struct {
    uint8_t  report_id;
    uint16_t usage_page;
    uint16_t usage_min, usage_max;      // used when n_usages == 0
    uint16_t usages[HID_MAX_USAGES];    // explicit usage list
    uint8_t  n_usages;
    uint16_t bit_off;                   // offset inside report payload (no ID byte)
    uint8_t  bit_size;
    uint8_t  count;
    int32_t  logical_min;
    uint8_t  flags;                     // Input item flags (bit1 variable, bit2 relative)
} hid_field_t;

typedef struct {
    hid_field_t f[HID_MAX_FIELDS];
    int n;
} hid_desc_t;

// Decoded contents of one input report.
typedef struct {
    // keyboard (usage page 0x07)
    bool     has_keys;
    uint8_t  key_cover[32];   // bitmap: usages this report describes
    uint8_t  key_down[32];    // bitmap: usages currently pressed
    // mouse
    bool     has_mouse;
    uint16_t buttons;         // bit n = button n+1
    int32_t  x, y, wheel, pan;
    // consumer (usage page 0x0C, non-relative)
    bool     has_consumer;
    uint16_t consumer;        // first pressed consumer usage, 0 = none
} hid_input_t;

int  hid_parse_descriptor(const uint8_t *d, size_t len, hid_desc_t *out);
void hid_decode_input(const hid_desc_t *desc, uint8_t report_id,
                      const uint8_t *data, size_t len, hid_input_t *out);

static inline void bm_set(uint8_t *bm, uint16_t u) { if (u < 256) bm[u >> 3] |= 1 << (u & 7); }
static inline bool bm_get(const uint8_t *bm, uint16_t u) { return u < 256 && (bm[u >> 3] >> (u & 7)) & 1; }
