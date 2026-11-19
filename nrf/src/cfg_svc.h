// Configuration service: a Web Bluetooth page on either computer reads the hub's state (JSON) and
// sends short text commands. Both need an encrypted link, so only a paired computer can use them.
#pragma once
#include <stddef.h>

// implemented in main.c
int cfg_status(char *buf, size_t n);          // writes the JSON state, returns its length
void cfg_command(const char *cmd);            // runs one command line, e.g. "pair 2", "swap 1 0", "name 1 Mac"
