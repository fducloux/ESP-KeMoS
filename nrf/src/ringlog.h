// RAM log of the last ~150 lines, readable over Bluetooth.
#pragma once
#include <stddef.h>
#include <stdint.h>
size_t ringlog_read(uint32_t from, char *out, size_t n);
