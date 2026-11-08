# ESP KVM: one keyboard and mouse, two computers, all over Bluetooth

```
MX Keys ─────┐                       ┌──> Computer 1 (Win 11)   BLE keyboard+mouse
             ├── BLE ──> ESP32 ──────┤
MX Master 3 ─┘                       └──> Computer 2 (MacBook)  BLE keyboard+mouse
```

Runs on one original ESP32 (the 38-pin ESP32S / ESP-WROOM-32 boards). The
ESP32 stays connected to both computers at once, so switching is instant.

**Hotkey:** hold **Ctrl+Shift** and press **Left Arrow** (computer 1) or **Right Arrow** (computer 2).
The arrow key never reaches either computer.
The on-board LED (GPIO2) lights when computer 2 is active.

## Build and flash

You need ESP-IDF 5.1 or newer (5.3 recommended). The VS Code ESP-IDF extension
is the easiest way to install it.

```
cd esp_kvm
idf.py set-target esp32
idf.py build flash monitor
```

Keep the serial monitor open for the first-time setup below: it shows the
pairing code for the keyboard.

## First-time setup (about 5 minutes)

Pair the computers first, using each computer's own trackpad or keyboard. The
MX Keys won't type into a computer until the ESP32 is paired with it.

1. **Computer 1 = Windows.** Settings → Bluetooth & devices → Add device →
   Bluetooth → **ESP KVM**. It pairs without a PIN. The first computer to pair
   becomes computer 1.
2. **Computer 2 = MacBook.** System Settings → Bluetooth → **ESP KVM** → Connect.
3. **MX Master 3.** Press the Easy-Switch button underneath until a free
   channel's light blinks, then hold it 3 seconds until it blinks fast (pairing
   mode). The ESP32 finds it and pairs on its own.
4. **MX Keys.** Pick a free Easy-Switch key (F1/F2/F3) and hold it 3 seconds
   until it blinks fast. The serial monitor prints a 6-digit code. **Type the
   code on the MX Keys, then press Enter.**

Done. Leave the keyboard and mouse on that Easy-Switch channel. Their other
channels still work normally if you ever want to bypass the ESP32.

Pair things one at a time. While the keyboard is pairing, the ESP32 briefly
asks for a passkey, which a computer pairing at the same moment would also see.

## Everyday use

* When the keyboard or mouse sleeps and wakes, the ESP32 reconnects it
  automatically. The first key press after waking can be lost, same as
  with any Bluetooth keyboard.
* Caps Lock follows whichever computer is active.
* Media keys (volume, play/pause) work. Logitech-only keys (emoji, dictation,
  screenshot) send nothing, because they need Logi Options+.
* MX Keys OS layout: press Fn+P on the Windows-style layout or Fn+O for Mac.
  The keyboard sends one layout to both computers, so pick the one you prefer,
  or swap Cmd/Ctrl in macOS Settings → Keyboard → Modifier Keys.

## Start over

Hold the **BOOT** button while plugging in or pressing **EN/RST**. The ESP32
forgets all four devices. Then remove "ESP KVM" from both computers'
Bluetooth lists and do the setup again.

## Tweaks (top of `main/main.c`)

| Setting | Default | Meaning |
|---|---|---|
| `DEVICE_NAME` | `"ESP KVM"` | Name the computers see |
| `PAIR_NAME_PREFIX` | `"MX "` | Only pair input devices whose name starts with this. Use `""` for any brand. |
| `HK_KEY`, `HK_ONE`, `HK_TWO` | ], 1, 2 | HID usage codes for the hotkey |
| `LED_GPIO` | 2 | Status LED |

## Troubleshooting

* **The keyboard won't pair.** Watch the monitor. If no code appears, the
  keyboard may be using Just Works pairing, which needs no code. If you see
  "encryption failed", press BOOT+EN to reset and try again.
* **A computer shows connected but nothing types.** Remove ESP KVM from that
  computer and pair it again. Windows caches the device description, so
  re-pair after any change to `report_map` in `hid_dev.c`.
* **The mouse feels sluggish.** macOS sometimes picks a slow connection
  interval. Moving the ESP32 closer, or keeping the Mac's Wi-Fi on 5 GHz,
  usually helps.
* **Turn on debug logs:** `idf.py menuconfig` → Component config → Log output.

## How it works

* `main.c`: connects to the MX devices as a BLE central, discovers their HID
  service, subscribes to their input reports, runs the hotkey state machine
  and routes everything to the active computer.
* `hid_parse.c`: a small HID descriptor parser, so any BLE keyboard or mouse
  layout works (MX Master 3 sends 12-bit X/Y, horizontal scroll and so on).
* `hid_dev.c`: the BLE keyboard, mouse and media-key device both computers
  pair with (HID over GATT, with Battery and Device Information services).

## Setup page (Wi-Fi)

The board opens its own Wi-Fi network **ESP-KVM-Setup** (password `espkvm-setup`).
Join it with a phone and open **http://192.168.4.1**. Login: user `admin`,
password `admin1234`. Change both passwords on the page.

From the page: switch computer, forget either computer or either input device,
wipe all pairings, change the hotkey key and Ctrl+Shift requirement, the
device-name filter for pairing, and the Wi-Fi timeout.

The Wi-Fi shares the radio with Bluetooth, so it switches off 10 minutes after
the last phone leaves (setting on the page). Press **BOOT** on the board to
switch it on again. Do not hold BOOT during a reset: that enters download mode.
