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
- Power (2026-11-12): only the hub's USB-C cable is plugged in; a wire from the hub's 5 V pin (`VIN`, silkscreen `VN` bottom-left) to the pad's `V5` powers the pad. Never plug both boards into USB with that wire in. The pad then has no USB link to the Mac: to flash it, unplug the hub, plug the pad's own micro-USB cable (the hub can power the pad the other way round). The hub can also flash the pad (see "Flashing the pad from the hub").
- Names: pad = "KVM CMP1" (computer 1, the Mac), hub = "KVM CMP2" (computer 2, Windows). They differ so the computers' Bluetooth lists show which is which. Old "ESP KVM" / "ESP KVM Win" entries must be forgotten on both computers. A computer that cached a board's address under an old name may not list it in a new scan: toggle Bluetooth, wait up to 30 s, press Pair again.
- Boards (2026-11-12): hub = USB-C board with the LEDs, MAC `b4:bf:e9:61:d4:18` (the former daily driver, now on hub firmware; tag `Stable` goes back on it with a checkout and `idf.py flash`). Pad = micro-USB board `d8:bc:38:fd:48:f0`. Pair on the hub's page after a flash with no erase: the old pairings are still saved, press Forget first.
- Setup: flash the hub (`idf.py flash` in the repo) and the pad (`experiments/pad`), join `ESP-KVM-Setup` (`espkvm-setup`), open 192.168.4.1 (admin / `admin1234`), pair MX Keys and MX Master, press Pair computer 1 and pair the pad on the Mac (forget old entries first), then Pair computer 2 for Windows. Alt/Command swap for computer 1 is now on by default (`mod_swap = 1`); the hub logs the value at boot. A board that already saved `modswap` keeps its saved value: change it on the page.
- Result on 2026-11-12: Mac pointer "0 lag, 0 latency" and Windows "perfect", no controller crash in 35+ minutes with the pad, Windows, mouse and keyboard all connected. The mouse still drops (`0x208`) when idle and reconnects when moved: that is the MX Master sleeping.
- The other test board is MAC `b4:bf:e9:60:8d:94` (was the hub during development, now spare). Before any flash check the MAC with `python -m esptool --port X chip_id`, and ask before flashing `b4:bf:e9:61:d4:18` (the daily-driver hub).

## Mouse/keyboard drops (2026-11-14)
- MX Master dropped with `0x208` (supervision timeout, link 7.5 ms lat 44 timeout 2160 ms), mostly after idle gaps; the MX Keys also dropped now and then. Computers did not see it when the devices were connected to them directly.
- Ruled out: Wi-Fi AP, Windows link, the pad's radio next to the hub (pad held in reset: drops continued), weak signal (RSSI sampled at each report was -45 to -64 dBm; `ble_gap_conn_rssi` polled while idle reads -100 and is stale), the `CONN_UPDATE_REQ` handler (never fires).
- Changes that helped: `CONFIG_BTDM_CTRL_MODEM_SLEEP=n` (timeouts per minute about 3 times lower in one 19 min run), and a continuous scan (40 ms / 40 ms) while a known input device is missing (reconnects in 1 to 8 s when it advertises). The log line `Input device N ready ... ms after it disconnected` measures it.
- Also: the hub no longer re-forwards an unchanged host LED state to the keyboard (computers echo it after the no-sleep ghost Shift tap, which lit the MX Keys backlight).
- Keep-alive (committed 2026-11-15): `keepalive()` in `mouse_tick` reads the Device Name of an input device that has been quiet for 0.8 s, about once a second. Runs: 14 timeouts in 16 min (modem sleep on), 6 in 19 min (off), 3 in 14 min (off + keep-alive): not clearly better, harmless. Timeouts come 2 to 3 s after the mouse's last report, so the mouse goes silent by itself. Plan: nRF52840 BLE sniffer to see what the mouse does before it times out; maybe port the hub to the nRF52840 later.
- Open: timeouts still happen. Delete `sdkconfig` once after pulling (new default in `sdkconfig.defaults`). Do not trust the polled RSSI.

## Flashing the pad from the hub (2026-11-12): no USB cable on the pad
- Extra wires, hub to pad (all four needed): hub `D32` (GPIO32) to pad `EN`, hub `D33` (GPIO33) to pad `G0` (BOOT), hub `D26` (GPIO26) to pad `RXD`/`RX0` (GPIO3), hub `D27` (GPIO27) to pad `TXD`/`TX0` (GPIO1). `D35` and the other GPIO34-39 are input only: they cannot drive `G0`. The pad's `EN` and `G0` sit on opposite headers.
- The hub (`main/padflash.c`, library `espressif/esp-serial-flasher` via `main/idf_component.yml`, `managed_components/` is not committed) pulls `G0` low, resets the pad through `EN`, and writes bootloader (0x1000), partition table (0x8000) and app (0x10000) through the pad's ROM boot loader at 115200 baud (about 1 minute; 460800 and `FLASH_END` were not used: the ROM rejected `FLASH_END` with error 0x06, so each part is verified with MD5 instead). The pad's NVS is not touched, so its pairing survives.
- The firmware to write sits in the hub's `padfw` partition (`partitions.csv`, 0x190000, 1 MB). After building `experiments/pad`: `python tools/make_padfw.py`, then `python -m esptool --port <hub port> write_flash 0x190000 padfw.blob` (`padfw.blob` is gitignored). Start it with the **Update** button on the web page, or type `F` on the hub's USB console. Console `T` = wire test of EN/BOOT through the pad's heartbeat, `P` = probe the wires with pull-downs.
- The hub's partition table is custom now (`sdkconfig.defaults`: `CONFIG_PARTITION_TABLE_CUSTOM`); after pulling this, delete `sdkconfig` once so it regenerates, then flash the hub with `idf.py flash` (NVS stays at 0x9000, pairings survive).
- Gotcha: opening the hub's serial port from a script resets the hub (DTR/RTS). Do not close or reopen the port while a pad flash is running (it takes about 70 s).

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
