# ESP-Logi: project details

## Goal

Share one Logitech **MX Keys** keyboard and **MX Master 3** mouse between two
computers, a **Windows 11 PC** and a **MacBook Pro**, using a single
**ESP32S (38-pin, original ESP32 / ESP-WROOM-32)** board. There are no cables
to the computers: everything runs over Bluetooth LE. A keyboard hotkey
switches which computer gets the input.

## Requirements (as agreed)

| # | Requirement | Status |
|---|---|---|
| 1 | One ESP32S board, no extra hardware | Done in design |
| 2 | Computers connect to the ESP32 over BLE (no USB cables) | Done |
| 3 | Keyboard and mouse connect to the ESP32 over BLE. The Logitech USB receiver isn't usable because the ESP32S has no USB host. | Done |
| 4 | Hotkey **Ctrl+Shift+Left Arrow = computer 1, Ctrl+Shift+Right Arrow = computer 2** | Done |
| 5 | Hotkey keystrokes never reach either computer | Done |
| 6 | No stuck keys after switching (release-all sent to the old computer) | Done |

## Architecture

```
MX Keys ─────┐  BLE (ESP32 = central)    BLE (ESP32 = peripheral)  ┌─> Computer 1: Win 11
             ├──────────────────> ESP32 ───────────────────────────┤
MX Master 3 ─┘                                                     └─> Computer 2: MacBook Pro
```

* The ESP32 holds 4 BLE links at once (limit set to 5): 2 input devices and 2 computers.
* It stays connected to **both** computers all the time, so switching only
  changes where reports are sent. No reconnect is needed, so switching is instant.
* Stack: **ESP-IDF 5.1+ (developed against 5.3.2), NimBLE host**, BLE only.

## Files

| File | Purpose |
|---|---|
| `main/main.c` | Central role (scan, pair and reconnect the MX devices, GATT discovery, subscribe to input reports), hotkey state machine, routing to the active computer, host slot tracking, NVS persistence, reset |
| `main/hid_parse.c/.h` | Generic HID report-descriptor parser and input decoder (keyboard, mouse buttons, X/Y, wheel, AC Pan, consumer keys) |
| `main/hid_dev.c` | Peripheral role: the HID-over-GATT keyboard + mouse + media-key device the computers pair with (plus Battery and Device Info services) |
| `main/kvm.h` | Shared declarations |
| `sdkconfig.defaults` | BLE only, NimBLE central+peripheral, 5 connections, bonding in NVS |
| `README.md` | Build, flash, first-time pairing, troubleshooting |

## Key design decisions

* **The ESP32S is the original ESP32.** It has no native USB, so it can't be
  a USB host for the Logi receiver or act as a USB HID device. Everything is BLE.
* **Rejected:** CH9329 UART→USB HID modules on the output side, because the
  user doesn't want cables to the computers.
* **Rejected:** a multi-board design (ESP-NOW hub plus dongles), because one
  board is enough when both computers are within BLE range.
* **Pairing I/O capability:** computers pair with *Just Works* (`NO_IO`).
  While a *new* input device pairs, the I/O capability temporarily switches
  to `DISP_ONLY` so the MX Keys can enter a passkey printed on the serial
  console. Pair devices one at a time.
* **Report handling:** the input devices' report maps are parsed, so nothing
  is hard-coded to Logitech layouts. Output uses a fixed, standard
  descriptor: report 1 is the keyboard (8 bytes + LED output), report 2 the
  mouse (8 buttons, int16 X/Y, int8 wheel, int8 pan), report 3 consumer keys
  (one 16-bit usage).
* **Computer slots:** the first computer to bond is computer 1, the second
  is computer 2. Identity addresses are saved in NVS (`host0`, `host1`).
  Input devices are saved as `in0` and `in1`.
* **Reset:** hold BOOT (GPIO0) at power-up to clear the bonds and NVS slots.
* **Status LED:** GPIO2 lights when computer 2 is active.

## Status

* Code written.
* Checked so far:
  * The HID parser passes host unit tests: our own descriptor, plus an
    MX Master–style descriptor with 12-bit X/Y, AC Pan and a vendor HID++
    report that must be ignored.
  * `main.c` and `hid_dev.c` compile cleanly (`-Wall -Wextra`) against the
    real NimBLE headers pinned by ESP-IDF v5.3.2.
  * All `sdkconfig.defaults` option names exist in ESP-IDF v5.3.2.
* Not tested yet:
  * A full `idf.py build`.
  * Anything on hardware.

## Known limitations

* Logitech-only keys (emoji, dictation, screenshot, Smart Actions) need
  Logi Options+ / HID++, so they send nothing.
* The MX Keys OS layout (Fn+P / Fn+O) applies to both computers.
* Not usable in BIOS/UEFI or at pre-boot screens, because BLE needs the OS running.
* Added latency is about 15–30 ms: one BLE hop in, one out.

## Next steps / ideas

1. Run `idf.py build flash monitor` and fix any compile or link issues.
2. Pair Windows, then the Mac, then the MX Master 3, then the MX Keys
   (see README), and confirm each step in the serial log.
3. Check the mouse feel on macOS. Tune the connection intervals
   (`assign_host()` and `connect_to()`) if it feels laggy.
4. Possible additions:
   * Per-computer modifier remap (swap Cmd/Ctrl for the Mac).
   * Hold the hotkey state across reboots (save `active` to NVS).
   * A short beep or LED blink when switching.
   * Forward HID++ for the Logitech-specific keys.

## Alternatives considered (no ESP32 needed)

* The MX Keys and MX Master 3 both have 3 **Easy-Switch** channels, so a
  button press can switch computers.
* **Logi Options+ Flow** switches by moving the mouse to the edge of the
  screen; it needs both computers on the same network.
* **Input Leap / Barrier** works the same way, in software over the network.
