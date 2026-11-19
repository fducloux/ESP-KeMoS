// HID over GATT peripheral that the computers pair with ("KVM CMP2"):
// one combined keyboard + mouse + consumer-control device, same report map as the ESP32 hub.
// Two computers can be connected at once; main.c decides which one is "active".
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/bluetooth/conn.h>

#define NUM_HOSTS 2

void hid_dev_init(void);
// make sure the computer-facing identity exists (call after settings_load)
void hid_dev_id_setup(void);
void hid_dev_adv_start(void);
void hid_dev_adv_start_later(void);     // 300 ms later, from a work item (use after a connection event)

// A computer connected (role peripheral). It is "pending" until main.c knows which computer it is.
void hid_dev_conn_open(struct bt_conn *c);
void hid_dev_conn_close(struct bt_conn *c);
bool hid_dev_is_host_conn(const struct bt_conn *c);
int  hid_dev_host_of(const struct bt_conn *c);          // 0..NUM_HOSTS-1, or -1 while pending
void hid_dev_assign(struct bt_conn *c, int host);       // c is computer `host`; an older link of it is dropped
bool hid_dev_connected(int host);
struct bt_conn *hid_dev_conn_for(int host);             // or NULL
struct bt_conn *hid_dev_pending(void);                  // a connected computer nobody has identified yet, or NULL

int hid_dev_send_keyboard(int host, const uint8_t rpt[8]);
int hid_dev_send_consumer(int host, uint16_t usage);

// Mouse movement goes to the active computer. It is summed up and sent when the previous
// notification has gone out, so nothing is lost and nothing waits on a timer.
void hid_dev_set_active(int host);
int hid_dev_cycle_mouse_gap(void);                      // 0 -> 8 -> 15 -> 22 ms minimum between mouse reports; returns the new value
void hid_dev_mouse_in(uint8_t buttons, int dx, int dy, int wheel, int pan);

// called when a computer sets the keyboard LEDs (caps lock etc.)
void hid_dev_on_leds(int host, uint8_t leds);
