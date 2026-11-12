#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// hid_dev.c — the HID device the computers see
void hid_dev_init(void);
void hid_dev_send_keyboard(uint16_t conn, const uint8_t rpt[8]);
void hid_dev_send_mouse(uint16_t conn, const uint8_t rpt[7]);
void hid_dev_send_consumer(uint16_t conn, uint16_t usage);
const uint8_t *hid_dev_report_map(size_t *len);

// main.c — called when a computer writes keyboard LEDs (Caps Lock etc.)
void kvm_on_host_leds(uint16_t conn, uint8_t leds);

// ---- settings (stored in NVS namespace "kvmcfg"; changed from the web page)
typedef struct {
    char     pair_prefix[16];   // only pair new input devices whose name starts with this ("" = any)
    uint16_t ap_minutes;        // Wi-Fi page switches off this long after the last phone left (0 = never)
    char     ap_pass[32];       // Wi-Fi password of the setup network
    uint8_t  sw_btn;            // mouse button (1..16) that switches computers with Ctrl+Shift, 0 = off
    uint8_t  mod_swap;          // bit 0 = computer 1, bit 1 = computer 2: swap Alt and Command/Win
    char     web_pass[32];      // login password of the web page (user: admin)
} kvm_cfg_t;
extern kvm_cfg_t kvm_cfg;
void kvm_cfg_load(void);
void kvm_cfg_save(void);
void kvm_cfg_reset(void);

// ---- status and actions used by the web page (main.c)
typedef struct {
    bool known, connected, ready;
    char addr[18];
} kvm_dev_status_t;
typedef struct {
    int active;
    int pairing;              // 0 none, 1/2 = computer 1/2, 3 = keyboard, 4 = mouse
    int pair_left;            // seconds left in the pairing window
    bool learning;            // waiting for a mouse button click to learn
    int32_t passkey;          // pairing code to type somewhere, or -1
    char passkey_for[12];     // "keyboard" or "computer"
    kvm_dev_status_t host[2], input[2];
} kvm_status_t;
void kvm_get_status(kvm_status_t *s);
void kvm_switch(int target);
void kvm_learn_button(bool on);
bool kvm_pair(int target);       // 1/2 computers, 3 keyboard, 4 mouse, 0 cancel
void kvm_forget_host(int h);
void kvm_swap_hosts(void);
void kvm_forget_input(int i);
void kvm_forget_all(void);

// ---- padflash.c: the hub flashes the pad over a serial link
bool kvm_pad_alive(void);       // main.c: pad heartbeat seen in the last 3.5 s
bool kvm_pad_pair_open(void);   // main.c: the pad reports its pairing window open
bool padflash_start(void);      // false if one is already running
void padflash_init(void);       // typing F on the USB console starts it

// ---- web.c: Wi-Fi access point + settings page
void web_start(void);
