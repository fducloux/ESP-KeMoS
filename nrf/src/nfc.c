// NFC tag: a phone tapped on the coil on the XIAO's NFC pads reads one URL (the setup page).
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nfc_t2t_lib.h>
#include <nfc/ndef/uri_msg.h>
#include "nfc.h"

LOG_MODULE_REGISTER(nfc, LOG_LEVEL_INF);

#define PAGE_URL "fducloux.github.io/NRF-KeMoSw/"      // the published setup page; "https://" is added by the tag

static uint8_t ndef_buf[128];
static volatile int n_field, n_read;
static struct k_work tap_work;

static void tap_fn(struct k_work *w) { nfc_tapped(); }

static void nfc_cb(void *ctx, nfc_t2t_event_t ev, const uint8_t *data, size_t len)
{
    switch (ev) {
    case NFC_T2T_EVENT_FIELD_ON: n_field++; break;
    case NFC_T2T_EVENT_DATA_READ: n_read++; k_work_submit(&tap_work); break;   // the phone read the tag
    default: break;
    }
}

void nfc_tag_init(void)
{
    uint32_t len = sizeof(ndef_buf);
    k_work_init(&tap_work, tap_fn);
    if (nfc_t2t_setup(nfc_cb, NULL) < 0) { LOG_ERR("nfc setup failed"); return; }
    if (nfc_ndef_uri_msg_encode(NFC_URI_HTTPS, (const uint8_t *)PAGE_URL, strlen(PAGE_URL), ndef_buf, &len) < 0) {
        LOG_ERR("nfc ndef encode failed"); return;
    }
    if (nfc_t2t_payload_set(ndef_buf, len) < 0) { LOG_ERR("nfc payload failed"); return; }
    if (nfc_t2t_emulation_start() < 0) { LOG_ERR("nfc start failed"); return; }
    LOG_INF("nfc tag on, https://%s", PAGE_URL);
}

int nfc_field_count(void) { return n_field; }
int nfc_read_count(void) { return n_read; }
