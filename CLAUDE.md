# CLAUDE.md: project memory for ESP-Logi

Read `PROJECT.md` for full details. This file holds the short version so a
future session can pick up quickly.

## What this is
An ESP-IDF (NimBLE) firmware for one **original ESP32 (38-pin ESP32S)** that
works as a Bluetooth keyboard/mouse switch:
- **Logitech MX Keys + MX Master 3** connect to the ESP32 over BLE. The ESP32 is the central.
- The ESP32 appears as "ESP KVM", one BLE HID keyboard+mouse, to **Win 11** (computer 1) and a **MacBook Pro** (computer 2).
- **Ctrl+Shift+Left/Right Arrow** switches to computer 1/2. Ctrl+Shift+click of a learned mouse button (MX Master button 5) toggles. Per-computer Alt/Command swap on the web page.

## Owner preferences (fducloux)
- No cables to the computers: everything must be BLE.
- One ESP32 board only. The 38-pin ESP32S boards are what's on hand (no S3).
- Answers should be concise; the owner often reads on a phone.

## Status (2026-11-08)
- Code complete. The parser passes its host tests, and `main.c`/`hid_dev.c` compile against the ESP-IDF v5.3.2 NimBLE headers.
- **Never built with idf.py and never run on hardware.** The first build and flash is the next step.
- The highest-risk area is pairing the MX Keys with the ESP32 as central: a passkey is printed to serial and typed on the keyboard.

## Code map
- `main/main.c`: central side (scan/connect/bond/GATT discovery), hotkey state machine (`hotkey()`, `process_keys()`), routing (`send_keys/send_mouse`), host slots (`assign_host()`), NVS, reset via BOOT.
- `main/hid_parse.c`: generic HID descriptor parser. Unit test lives outside the repo; re-create it with gcc on the host if needed.
- `main/hid_dev.c`: GATT HID service the computers see. Changing `report_map` means the computers must re-pair (Windows caches it).

## Gotchas to remember
- Mark hotkey digits as suppressed **before** `switch_to()`, or the new computer sees the "1"/"2" (bug fixed once already).
- `ble_hs_cfg.sm_io_cap` is toggled to `DISP_ONLY` only while a new input device pairs; otherwise it's `NO_IO` (Just Works for the computers).
- Pair the computers first, using their own keyboards or trackpads.
- Input devices are matched on reconnect by their saved identity address. If the MX devices start using resolvable private addresses, matching may need the IRK or bond store instead.

## Web setup page (added 2026-11-08)
- `main/web.c` + `main/index.html` (embedded): SoftAP `ESP-KVM-Setup`, http://192.168.4.1, HTTP Basic auth (user `admin`). Settings live in NVS namespace `kvmcfg` (`kvm_cfg` in `main.c`); pairings in namespace `kvm`.
- Hotkey: Ctrl+Shift+Left/Right Arrow (fixed). The old ]-] sequence and its settings were removed.
- Holding BOOT at reset enters ROM download mode, so the old "hold BOOT at power-up to wipe" is gone: wipe from the page.
- 4 MB flash and the large single-app partition (`sdkconfig.defaults`) are needed because Wi-Fi made the app ~950 KB.
- Never tested on hardware yet: check BLE latency with Wi-Fi on.
