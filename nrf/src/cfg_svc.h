// Configuration service: a Web Bluetooth page on either computer reads the hub's state (JSON) and
// sends short text commands. Both need an encrypted link, so only a paired computer can use them.
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <zephyr/bluetooth/conn.h>

// implemented in main.c
bool cfg_access_ok(struct bt_conn *conn);      // a paired computer, or any device while setup is open (then it is kept as the config link)
int cfg_status(char *buf, size_t n);          // writes the JSON state, returns its length
void cfg_command(const char *cmd);            // runs one command line, e.g. "pair 2", "swap 1 0", "name 1 Mac"
