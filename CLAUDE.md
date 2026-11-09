# CLAUDE.md: project memory for ESP-Logi (repo name ESP-KeMoS)

Read `PROJECT.md` for full details (parts of it predate the web page and explicit pairing).
This file holds the short version so a future session can pick up quickly.

## What this is
An ESP-IDF (NimBLE) firmware for one **original ESP32 (38-pin ESP32S)** that works as a
Bluetooth keyboard/mouse switch, published at https://github.com/fducloux/ESP-KeMoS (public):
- **Logitech MX Keys + MX Master 3** connect to the ESP32 over BLE. The ESP32 is the central.
- The ESP32 appears as "ESP KVM", one BLE HID keyboard+mouse, to two computers. Today computer 1 is the **MacBook** and computer 2 is **Windows 11** (the page has a Swap button, so check `host[]` rather than assuming).
- **Ctrl+Shift+Left/Right Arrow** switches to computer 1/2. Ctrl+Shift+click of a learned mouse button (MX Master "button 5"; the thumb button is invisible to the ESP32) toggles. Per-computer Alt/Command swap on the web page.
- **No-sleep** per computer: Ctrl+Shift+mouse button 4 toggles it for the active computer (saved in NVS `kvmcfg` key `nosleep`). While on, a ghost F15 key goes to that computer every 30 s, and its LED flashes for 100 ms every 2 s.
- LEDs: GPIO 25 = computer 1, GPIO 23 = computer 2, on-board GPIO 2 = computer 2 (330 ohm resistors).

## Owner preferences (fducloux)
- No cables to the computers: everything must be BLE. No software on the computers (one is a locked-down corporate laptop; Logi Flow was rejected for that reason).
- One ESP32 board only. The 38-pin ESP32S boards are what's on hand (no S3).
- Two computers only for now; a third was considered and postponed.
- Answers should be concise; the owner often reads on a phone.
- Asked to commit and push explicitly each time; do not push on your own.

## Status (2026-11-08)
- Built, flashed and running on hardware. Both computers and both MX devices work; bonds survive reboots.
- Stable config: all four links at 15 ms, latency 0. Committed firmware at `c86be9b` and later is the known-good baseline.
- Open points: occasional BLE controller crash risk (see gotchas), `failed to configure restored IRK` at boot (harmless, from duplicate Mac bond entries), `phy_init` calibration warning each boot.

## Code map
- `main/main.c`: central side (scan/connect/bond/GATT discovery), hotkey (`hotkey()`, `process_keys()`), routing (`send_keys/send_mouse`), mouse coalescing (`mouse_flush`, 15 ms callout), host slots (`assign_host()`), explicit pairing windows (`kvm_pair()`, `pairing_on()`), link timing (`upd_tick`), LEDs (`led_update()`), NVS, `kvm_cfg`.
- `main/web.c` + `main/index.html` (embedded): SoftAP `ESP-KVM-Setup`, http://192.168.4.1, HTTP Basic auth (user `admin`), captive-portal probe handlers, UDP DNS responder, AP auto-off after `ap_minutes` (default 3) without a phone.
- `main/kvm.h`: API between `main.c` and `web.c`.
- `main/hid_parse.c`: generic HID descriptor parser. Unit test lives outside the repo.
- `main/hid_dev.c`: GATT HID service the computers see. Changing `report_map`, the PnP ID or the device name means the computers must re-pair (Windows and macOS cache them).
- NVS: namespace `kvm` (in0, in1, host0, host1 addresses), `kvmcfg` (prefix, apmin, appass, webpass, modswap, swbtn), `nimble_bond`.
- `sdkconfig.defaults`: 4 MB flash, large single-app partition, size optimization (Wi-Fi made the app ~950 KB).

## Gotchas to remember
- Mark hotkey keys as suppressed **before** `switch_to()`, or the new computer sees the arrow key.
- `ble_hs_cfg.sm_io_cap` is `DISP_ONLY` only while the keyboard pairs (passkey shown on the page); otherwise `NO_IO` with `sm_mitm = 0` (Just Works). `sm_mitm = 1` made Windows re-pair on every reconnect.
- Unknown computers are refused (bond deleted, link terminated) unless Pair for a computer is active. Same for input devices: scan only pairs a new device in the targeted slot (slot 0 keyboard, slot 1 mouse).
- macOS flips the peer address type (public/random) between connects: match hosts by the 6 address bytes only, and `fix_bond_type()` copies the bond. `delete_peer_both()` on forget.
- Holding BOOT at reset enters ROM download mode, so there is no BOOT wipe. Wipe from the page, or `idf.py erase-flash`.
- Never let a browser autofill one generated password into both password fields (the Wi-Fi and login passwords were lost once; fixed by erasing NVS).

## Link timing: do not retry these (all tried on 2026-11-08)
- The computers are the central on their link, so the ESP32 can only request an interval; macOS decides. macOS stays at **15 ms** and ignored 7.5 and 11.25 ms. Windows may pick 7.5 ms by itself.
- 7.5 ms active links plus a 50 to 60 ms idle computer: repeated `LoadProhibited` BLE controller crashes right after the computers connect (same backtrace), then a reboot loop.
- Mouse flush tick of 8 ms: floods the Mac link, one batch per 15 ms event, massive lag. Keep 15 ms.
- Spoofing an Apple Magic Mouse (PnP ID 0x05AC/0x0269 and name): Mac loads Apple's driver and ignores our input; Windows dropped to 15 ms. Reverted.
- Link-update requests are sent only from `upd_tick` for the computers (one per tick, 2 s after the last connect or disconnect): overlapping updates crashed the controller in `r_lld_evt_update_create`. Input devices still get their 15 ms request right after setup, because a later request is ignored. Input GATT handles are cached per device (`cached` in `input_t`) to speed up reconnects.
- The Mac connects with latency 22; `upd_tick` (every 3 s) fixes it within about 10 s.
- Faster boards (nRF52840) will not help: the limit is macOS, not the ESP32.

## Dev workflow
- Toolchain: `. ~/esp/esp-idf/export.sh` (ESP-IDF v5.3.2), port `/dev/cu.usbserial-0001`.
- Serial logging without resetting the board: a small pyserial script with `dtr = rts = False` writing to `log/console.log` (gitignored). Kill it before `idf.py flash`, restart it afterwards. System Python has no pyserial: use the ESP-IDF Python.
- The log has binary bytes: use `grep -a`. Crashes show as `Guru Meditation Error` then `rst:0xc (SW_CPU_RESET)`; `POWERON_RESET` after a flash is normal.
- Log lines worth knowing: `Computer N link X ms lat L -> asking Y ms` (timing retries), `Computer N link ...` after each switch, `Unknown computer refused`, `encryption failed, status 7`.
- Untracked and not ours to commit: `.DS_Store`, `STL box/` (enclosure files, owner's).
