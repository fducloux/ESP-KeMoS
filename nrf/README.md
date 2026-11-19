# KVM hub on the nRF52840 (Seeed XIAO nRF52840)

Port of the ESP32 hub to Zephyr and the nRF Connect SDK (v3.4.2, Nordic SoftDevice controller).
The MX Keys and MX Master 3 connect to the board over Bluetooth LE (the board is the central).
Two computers see the board as one Bluetooth keyboard, mouse and media-key device called "KVM CMP2".
Ctrl+Shift+Left Arrow sends everything to computer 1, Ctrl+Shift+Right Arrow to computer 2.

There is no Wi-Fi on this chip, so the ESP32 setup web page does not exist here yet. Pairing and
settings use single letters on the UART console for now (a Web Bluetooth page is planned).

## Build and flash
```
nrfutil sdk-manager toolchain launch --ncs-version v3.4.2 -- bash -c \
  'cd /opt/nordic/ncs/v3.4.2 && west build -b xiao_ble/nrf52840/sense <abs path>/nrf -d <abs path>/nrf/build'
```
The result is `build/nrf/zephyr/zephyr.uf2`. Copy it to the `XIAO-SENSE` drive that shows up after a
double tap on the tiny reset button next to the USB-C port. Without touching the board: send `B` on
the UART console and the firmware reboots into the bootloader.

## UART console (115200 baud, D6 = TX, D7 = RX)
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
| `B` | reboot into the UF2 bootloader |

## Notes
- The MX devices use identity 0, the computers identity 1 (own address, extended advertising set).
- The active computer gets a 7.5 ms link; the idle one a relaxed 30 to 50 ms.
- The fixed passkey is only offered on connections to the MX devices; computers pair without a code
  and an unknown computer is refused unless its pairing window is open.
