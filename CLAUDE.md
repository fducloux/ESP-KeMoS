# CLAUDE.md: project memory for ESP-Logi (repo name ESP-KeMoS)

Read `PROJECT.md` for full details (parts of it predate the web page and explicit pairing).
This file holds the short version so a future session can pick up quickly.

## What this is
An ESP-IDF (NimBLE) firmware for one **original ESP32 (38-pin ESP32S)** that works as a
Bluetooth keyboard/mouse switch, published at https://github.com/fducloux/ESP-KeMoS (public):
- **Logitech MX Keys + MX Master 3** connect to the ESP32 over BLE. The ESP32 is the central.
- The ESP32 appears as "ESP KVM", one BLE HID keyboard+mouse, to two computers. Today computer 1 is the **MacBook** and computer 2 is **Windows 11** (the page has a Swap button, so check `host[]` rather than assuming).
- **Ctrl+Shift+Left/Right Arrow** switches to computer 1/2. Ctrl+Shift+click of a learned mouse button (MX Master "button 5"; the thumb button is invisible to the ESP32) toggles. Per-computer Alt/Command swap on the web page.
- **No-sleep** per computer: Ctrl+Shift+mouse button 4 toggles it for the active computer (saved in NVS `kvmcfg` key `nosleep`). While on, a ghost Left Shift tap goes to that computer every 30 s, and its LED flashes for 100 ms every 2 s.
- LEDs: GPIO 25 = computer 1, GPIO 23 = computer 2, on-board GPIO 2 = computer 2 (330 ohm resistors).

## Two-board mode (branch `two-board`, 2026-11-12): the Mac gets its own ESP32
Why: macOS gives a 7.5 ms link only to the mouse appearance `0x03C2`, but one ESP32 controller crashes with two computers on fast links (see "Link timing"). So the Mac is served by a second board, the **pad** (`experiments/pad/`), and the **hub** (`main/`) keeps the MX Keys/MX Master and one direct computer (Windows).
- Pad: peripheral only, advertises appearance `0x03C2`, reuses `main/hid_dev.c`. Link ends at `7.50 ms lat 0` (asks once for latency 0 after 8 s). Holds one known computer (address in NVS `pad`/`peer`), refuses others. Hub's Pair button for computer 1 sends the pad a pairing window (120 s); pad's BOOT button does the same. Blue LED lit = linked.
- Hub: `REMOTE_HOST 0` (computer 1 = Mac) is reached over UART1, `DIRECT_HOST 1` (computer 2) stays a normal BLE link with the pair-merged mouse. Mouse reports to the pad go 1:1 (no merge): the Mac feels like a direct connection. Swap computers is disabled in this mode. Pad heartbeat every 0.5 s, hub marks it offline after 3.5 s.
- UART 460800 baud, frame `0xA5 | type | len | payload | xor`. Hub to pad: 1 keyboard(8), 2 mouse(7), 3 consumer(2), 5 pair, 6 forget. Pad to hub: 0x81 LEDs, 0x82 status {linked, known, pairing open}.
- Wiring (both boards): GPIO17 (TX2/G17) to the other board's GPIO16 (RX2/G16), crossed, plus GND. Never use RX0/TX0 (USB serial). Each board on its own USB data cable while developing; one charger later.
- Setup: flash the hub (`idf.py flash` in the repo) and the pad (`experiments/pad`), join `ESP-KVM-Setup` (`espkvm-setup`), open 192.168.4.1 (admin / `admin1234`), pair MX Keys and MX Master, press Pair computer 1 and pair "ESP KVM" on the Mac (forget old entries first), then Pair computer 2 for Windows. Turn on Alt/Command swap for computer 1 on the page (a wiped board starts with it off).
- Result on 2026-11-12: Mac pointer "0 lag, 0 latency", no controller crash in 30+ minutes with Mac pad, mouse and keyboard. Windows on the hub not tested yet.
- The test hub board is MAC `b4:bf:e9:60:8d:94`, the pad `d8:bc:38:fd:48:f0`. The board with `b4:bf:e9:61:d4:18` runs tag `Stable` (single board) and is the daily driver: do not flash it without being asked.

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
- Link-update requests are sent only from `upd_tick` for the computers (one per tick, 2 s after the last connect or disconnect): overlapping updates crashed the controller in `r_lld_evt_update_create`. Input devices get no update request (removed 2026-11-12). The per-device GATT handle cache (`cached` in `input_t`) is disabled.
- The Mac connects with latency 22; `upd_tick` (every 3 s) fixes it within about 10 s.
- 2026-11-12: every controller crash (`r_lld_evt_update_create`) is a connection-update procedure overlapping another link's setup. Do not set a 6 s supervision timeout (crash loop). Rejecting the mouse's own update requests did nothing. Input scanning starts 15 s after boot so the computers settle first. No update request to the mouse at ready (it ends at 7.5 ms lat 44 anyway). The `c86be9b` baseline also crashes at boot.
- Mouse drops (`0x208`, supervision timeout) are the MX Master sleeping at lat 44; reconnects work. `encryption failed (7)` is ENOTCONN (link lost mid-handshake), not a bad bond: never delete bonds on it.
- Mouse forwarding (2026-11-12): the MX Master sends ~110 reports/s, a 15 ms Mac link takes ~66/s. Sending 1:1 is smooth but queues up (pointer reacts late, `notify` failures); merging by elapsed time is uneven and macOS acceleration pumps (jumpy). Merging in fixed pairs (every 2nd report carries both, ~55/s) is smooth and responsive. The tick only sweeps a lone leftover older than 12 ms.
- Mouse tick runs on a fixed 15 ms grid (re-arming 15 ms after each run beat against the Mac's 15 ms events: pointer sped up and slowed down). Log shows `mouse in/out` per 3 s and the tick min..max.
- 2026-11-12 appearance test (do not retry): macOS gives the link its own settings by the advertised GAP appearance. Mouse `0x03C2` gets `7.50 ms lat 44` even for a combined keyboard+mouse descriptor and in any collection order. Generic `0x03C0` (what we advertise) gets `15 ms lat 22` and ignores asks for 7.5 and 11.25 ms. On the real firmware the mouse appearance put both computers on 7.5 ms links and crashed the controller (`r_lld_evt_update_create`) within seconds of boot, in a loop. Branch `mouse-appearance` holds that change (not for use). Test app: `experiments/mouse-only/` on branch `split-link`.
- Split link (keyboard and mouse as two separate devices to each computer): dropped. NimBLE keeps bonds per computer address only, so two identities from one computer overwrite each other's keys, and the original ESP32 (BLE 4.2) cannot advertise two identities at once. A fast Mac link would need a better controller (nRF52840, or a Pi with USB dongles).
- Git tag `Stable` (ee15408) = known-good single combined link with pair-merged mouse reports.
- Faster boards (nRF52840) will not help: the limit is macOS, not the ESP32.

## Dev workflow
- Toolchain: `. ~/esp/esp-idf/export.sh` (ESP-IDF v5.3.2), port `/dev/cu.usbserial-0001`.
- Serial logging without resetting the board: a small pyserial script with `dtr = rts = False` writing to `log/console.log` (gitignored). Kill it before `idf.py flash`, restart it afterwards. System Python has no pyserial: use the ESP-IDF Python.
- The log has binary bytes: use `grep -a`. Crashes show as `Guru Meditation Error` then `rst:0xc (SW_CPU_RESET)`; `POWERON_RESET` after a flash is normal.
- Log lines worth knowing: `Computer N link X ms lat L -> asking Y ms` (timing retries), `Computer N link ...` after each switch, `Unknown computer refused`, `encryption failed, status 7`.
- Untracked and not ours to commit: `.DS_Store`, `STL box/` (enclosure files, owner's).
