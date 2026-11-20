# KVM hub on the nRF52840 (Seeed XIAO nRF52840)

Port of the ESP32 hub to Zephyr and the nRF Connect SDK (v3.4.2, Nordic SoftDevice controller).
The MX Keys and MX Master 3 connect to the board over Bluetooth LE (the board is the central).
Two computers see the board as one Bluetooth keyboard, mouse and media-key device called "KVM CMP2".
Ctrl+Shift+Left Arrow sends everything to computer 1, Ctrl+Shift+Right Arrow to computer 2.

There is no Wi-Fi on this chip, so the setup page works over Bluetooth instead: open `web/index.html` in
Chrome or Edge on either computer (Safari has no Web Bluetooth), press Connect and pick "KVM CMP2".
The page shows both computers, the keyboard and mouse, and lets you switch, pair, forget, rename,
swap Alt/Command and set the mouse spacing. The UART console keys below do the same and more.
The page needs an encrypted link, so the computer must already be paired with the hub. On the first
run with no computer paired yet, use the UART keys `1` or `2`.

## Build and flash
```
nrfutil sdk-manager toolchain launch --ncs-version v3.4.2 -- bash -c \
  'cd /opt/nordic/ncs/v3.4.2 && west build -b xiao_ble/nrf52840/sense <abs path>/nrf -d <abs path>/nrf/build'
```
The result is `build/nrf/zephyr/zephyr.uf2`. Copy it to the `XIAO-SENSE` drive that shows up after a
double tap on the tiny reset button next to the USB-C port. Without touching the board: send `B` on
the UART console and the firmware reboots into the bootloader.

## Console
The log and the keys below are on the board's own USB-C port: it shows up as a serial port (`/dev/cu.usbmodem*` on a Mac), 115200 baud,
no wires needed. For the D6 TX / D7 RX adapter instead, set `zephyr,console` and `zephyr,shell-uart` to `&uart0` in
`boards/xiao_ble_nrf52840_sense.overlay`. The phone link also has its own advertisement ("8REC-KM setup", its own Bluetooth address,
no keyboard or mouse appearance) because iPhone apps are unreliable with devices that look like a keyboard. It is on for 5 minutes
after every power-up or reset, and after an NFC tap or Ctrl+Shift+8REC.

## UART keys
| Key | Action |
|-----|--------|
| `P` | 60 s window to pair the MX Keys or MX Master (put it in pairing mode; the keyboard asks for 123456, the blue LED blinks) |
| `1` / `2` | 120 s window to pair computer 1 / 2 (red LED) |
| `3` / `4` | switch to computer 1 / 2 |
| `S` | toggle Alt/Command swap for the active computer |
| `K` | print state |
| `M` | cycle the mouse report spacing (8 ms is the default) |
| `U` | forget every bond except the keyboard and mouse |
| `X` | forget all bonds |
| `L` | list bonds on both Bluetooth identities |
| `D` | log every incoming input report |
| `T` | test the blue LED |
| `Y` | toggle the always-on beacon (for the config page) |
| `N` | print every link's interval and latency |
| `J` | diagnosis: jiggle the pointer by +-1 count for 6 s and report delivery timing |
| `R` | reboot |
| `W` | show the last bond removals and why (kept in flash) |
| `Z` | toggle pointer resampling (off by default) |
| `a`-`e` | pointer spacing test settings, `f`-`k` mouse link interval test |
| `K` | also prints the movement totals in and out since boot |
| `B` | reboot into the UF2 bootloader |

## Notes
- Reconnecting a sleeping keyboard or mouse scans for 10 ms every 100 ms. A bigger scan duty (the Zephyr default of 30 ms every 60 ms) delayed the computers' links by 30 to 90 ms and made the pointer jumpy and inaccurate on macOS.
- The MX devices use identity 0, the computers identity 1 (own address, extended advertising set).
- The active computer gets a 7.5 ms link; the idle one a relaxed 30 to 50 ms.
- The fixed passkey is only offered on connections to the MX devices; computers pair without a code
  and an unknown computer is refused unless its pairing window is open.

## Configuration service
Custom GATT service `5a6f0001-6b76-4d48-8f2e-7d9c1b2a3c4d`: characteristic `...0002` (read) returns the
state as JSON, `...0003` (write) takes one text command: `pair 1|2`, `pairmx`, `switch 1|2`,
`swap 1|2 0|1`, `gap <ms>`, `forget 1|2`, `name 1|2 <text>`, `reboot`. Reads and writes need an
encrypted link. A slow non-connectable advertisement stays on, so the page can find the hub even
when both computers are connected.

## Pointer feel and the dot test
The hub forwards every movement exactly (the totals in and out match). What decides the feel on macOS is the timing of the
reports, because macOS accelerates the pointer from it. Two things matter: the mouse link and the active computer's link
both run at 7.5 ms, and when their radio events line up badly the mouse's reports reach the hub in lumps. The hub watches
the arrival gaps and re-sets the mouse link (7.5 / 8.75 ms) when they are lumpy. A reconnect scan for a sleeping keyboard
or mouse uses 10 ms every 100 ms for the same reason.

`tools/dots/` is a measuring page: `python3 tools/dots/server.py <results_dir>` and open http://127.0.0.1:8766/. It has a dot
click test, a circles test and a stops test, normal or raw (pointer lock, no acceleration), and saves every run as JSON.
Compare the hub with the Unifying dongle and with direct Bluetooth using the same mouse and the same Mac tracking speed.
