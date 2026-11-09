# ESP-KeMoS: Keyboard Mouse Switcher, two computers over Bluetooth

```
MX Keys ─────┐                       ┌──> Computer 1 (currently a Mac)      BLE keyboard+mouse
             ├── BLE ──> ESP32 ──────┤
MX Master 3 ─┘                       └──> Computer 2 (currently Windows 11) BLE keyboard+mouse
```

Runs on one original ESP32 (the 38-pin ESP32S / ESP-WROOM-32 boards, 4 MB flash).
The ESP32 stays connected to both computers at once, so switching is instant.
Nothing is installed on either computer, so it works on locked-down corporate laptops.

**Switching**
* Keyboard: hold **Ctrl+Shift** and press **Left Arrow** (computer 1) or **Right Arrow** (computer 2).
  The arrow key never reaches either computer.
* Mouse: hold **Ctrl+Shift** and click a mouse button you teach it on the setup page
  (MX Master "button 5" works; the thumb button is not visible to the ESP32).
* LEDs show the active computer: GPIO 25 = computer 1, GPIO 23 = computer 2.
  The on-board LED (GPIO 2) also lights for computer 2.
  Wire each LED anode to its pin through a 330 Ω resistor, cathode to GND.

## Build and flash

You need ESP-IDF 5.3.x (developed on 5.3.2).

```
. ~/esp/esp-idf/export.sh
idf.py set-target esp32
idf.py build flash monitor
```

Close any serial monitor or logger before flashing, or the port is busy.

## First-time setup (about 5 minutes)

Pairing is explicit: nothing pairs until you press a **Pair** button on the setup page
(see "Setup page" below). Each Pair button opens a 120 second window for that one slot.

1. Join the Wi-Fi **ESP-KVM-Setup** with your phone and open **http://192.168.4.1**.
2. **Computer 1 and 2.** Press **Pair** next to the computer, then add **ESP KVM** in that
   computer's Bluetooth settings. It pairs without a PIN. Do the same for the other computer.
3. **Mouse (MX Master 3).** Press **Pair** next to Mouse, then hold the Easy-Switch button
   under the mouse for 3 seconds on a free channel until it blinks fast.
4. **Keyboard (MX Keys).** Press **Pair** next to Keyboard, then hold a free Easy-Switch key
   (F1/F2/F3) for 3 seconds. The page shows a 6-digit code. **Type it on the MX Keys, then press Enter.**

Leave the keyboard and mouse on that Easy-Switch channel. Their other channels still
work normally if you ever want to bypass the ESP32.

To replace a device, press **Forget** next to it, then **Pair** again. If you forget a computer
on the page, also remove "ESP KVM" from that computer's Bluetooth list.

## Everyday use

* When the keyboard or mouse sleeps and wakes, the ESP32 reconnects it automatically.
  The first key press after waking can be lost, as with any Bluetooth keyboard.
* After a reboot of the board both computers and both input devices reconnect on their own
  within about 10 to 30 seconds. The Mac first connects slowly (latency 22) and the board
  corrects that within about 10 seconds.
* Caps Lock follows whichever computer is active.
* Media keys (volume, play/pause) work. Logitech-only keys (emoji, dictation, screenshot)
  send nothing, because they need Logi Options+.
* Alt/Command swap is a per-computer checkbox on the page (tick it for the Mac), so the
  Mac gets Command where the Windows layout has Alt, and the other way round.
* **Swap** on the page exchanges which physical computer is number 1 and 2, without re-pairing.

## Setup page (Wi-Fi)

The board opens its own Wi-Fi network **ESP-KVM-Setup** (WPA2, default password `espkvm-setup`).
Join it with a phone and open **http://192.168.4.1**. Login: user `admin`, default password
`admin1234`. Change both passwords on the page (do not let a browser autofill one generated
password into both fields).

From the page:
* switch computer, Pair / Forget for both computers, the keyboard and the mouse, swap computers;
* Alt/Command swap per computer, learn or clear the mouse switch button;
* name prefix filter for input devices (default `MX `), Wi-Fi timeout, both passwords;
* switch Wi-Fi off, reboot, reset settings to defaults (keeps pairings), wipe all pairings.

The Wi-Fi shares the radio with Bluetooth, so it switches off **3 minutes** after the last
phone leaves (setting on the page, 0 = never). Press **BOOT** on the board to switch it on
again, or use Reboot. Do not hold BOOT during a reset: that enters download mode.
Phones keep the connection because the board answers their "is there internet" probes.

## Start over

Press **Wipe** on the setup page, then remove "ESP KVM" from both computers' Bluetooth
lists and set up again. If the page is unreachable, `idf.py erase-flash` and flash again.

## Tweaks (top of `main/main.c`)

| Setting | Default | Meaning |
|---|---|---|
| `DEVICE_NAME` | `"ESP KVM"` | Name the computers see |
| `HK_LEFT`, `HK_RIGHT` | Left, Right arrow | HID usage codes for the hotkey (with Ctrl+Shift) |
| `LED_GPIO`, `LED1_GPIO`, `LED2_GPIO` | 2, 25, 23 | On-board LED, computer 1 LED, computer 2 LED |
| `PAIR_SECONDS` | 120 | Length of a Pair window |

Everything else (name prefix, Wi-Fi timeout, swap, switch button, passwords) is on the setup page.

## Speed and limits

All four Bluetooth links run at a **15 ms** connection interval with latency 0 (the board
retries until each computer accepts it). Typing is fine. Mouse pointer motion reaches the
Mac in steps about every 15 ms. Windows may negotiate 7.5 ms by itself.

macOS decides the interval for a Bluetooth device, and it refused 7.5 ms and 11.25 ms from
this board. These experiments were tried and rejected, so don't repeat them:
* 7.5 ms on the active links plus a slow (50 to 60 ms) idle computer: the Bluetooth
  controller crashed (`LoadProhibited`) and rebooted in a loop.
* Mouse flush every 8 ms: floods the Mac link and causes huge lag. The flush stays at 15 ms.
* Pretending to be an Apple Magic Mouse (Apple vendor/product ID and name): the Mac stayed
  at 15 ms, loaded Apple's driver and ignored our input, and Windows slowed down.

A faster board will not fix this, because the limit is macOS, not the ESP32.
The only way to get wired-like speed is a USB connection (for example a Logitech receiver
on a USB switch).

## Security

* The links use Bluetooth LE Secure Connections with bonding. The computers pair "Just Works"
  (no PIN), so they give no protection against a man-in-the-middle during pairing. Unknown
  computers are disconnected and their bond deleted unless a Pair window is open.
* The keyboard pairing uses a passkey shown on the setup page.
* The setup page is WPA2 plus a login password, and the Wi-Fi is off after 3 minutes of inactivity.
  Change both default passwords.

## Troubleshooting

* **Computer shows connected but nothing types.** Remove ESP KVM from that computer, press
  Forget then Pair for it on the page, and add it again. Windows caches the device description,
  so re-pair after any change to `report_map` in `hid_dev.c` or to the PnP ID or name.
* **Log says `Unknown computer refused`.** Press Pair for that computer on the page first.
* **Log says `encryption failed, status 7` or `MIC failure`.** A stale bond on one side.
  Forget on both the page and the computer, then pair again.
* **Mouse laggy right after a Mac reconnect.** Wait about 10 seconds for the board to fix the
  Mac link latency; the log shows `Computer 1 link ... asking 15.00 ms`.
* **Temperature.** The board runs warm (about 42 °C measured), which is normal.
* **Serial log:** `idf.py monitor`, or a logger that does not reset the board (DTR/RTS low).
  The log contains binary bytes, so use `grep -a`.

## How it works

* `main/main.c`: connects to the MX devices as a BLE central, discovers their HID service,
  subscribes to their input reports, runs the hotkey and mouse-button switching, merges mouse
  motion into one report every 15 ms, swaps Alt/Command per computer, drives the LEDs, keeps
  link timing healthy, and routes everything to the active computer.
* `main/hid_parse.c`: a small HID descriptor parser, so any BLE keyboard or mouse layout works.
* `main/hid_dev.c`: the BLE keyboard, mouse and media-key device both computers pair with
  (HID over GATT, with Battery and Device Information services).
* `main/web.c` and `main/index.html`: the Wi-Fi access point, captive-portal answers, tiny DNS
  responder, HTTP server with login, and the mobile setup page.
* `main/kvm.h`: the interface between `main.c` and `web.c`.
