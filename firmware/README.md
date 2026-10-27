# Firmware

Two small programs for two cheap boards, joined by three wires.

| Board | Firmware | Job |
|---|---|---|
| **ESP32** (any classic ESP32 DevKit / WROOM-32) | `loupe_side` | Finds the loupe over Bluetooth, wakes it, joins the loupe's own WiFi, assembles every video frame and passes it over the cable. |
| **NodeMCU v3** (ESP8266) — or a second ESP32 | `home_side_esp8266` / `home_side_esp32` | Serves the web page — as its own hotspot, in your home network, or as a relay for your server. |

Why two boards? The loupe always opens its **own** WiFi and cannot join yours. One ESP has one radio, so one
board stays with the loupe and the other one talks to you. Only the ESP32 has Bluetooth, which is needed to wake
the loupe.

## Flash

Easiest: the [web flasher](../flasher/) — Chrome or Edge, USB cable, one click per board.

With [PlatformIO](https://platformio.org/install/cli):

```bash
cd firmware
pio run -e loupe_side        -t upload   # the ESP32
pio run -e home_side_esp8266 -t upload   # the NodeMCU
# or, with a second ESP32 as the home side:
pio run -e home_side_esp32   -t upload
```

Flash one board at a time and unplug the 5 V wire between the boards while flashing (see [power](../docs/hardware.md#power)).
The home side embeds `../web/index.html` at build time (`scripts/embed_page.py`), so the page on the device is
always the same page the server serves.

## Wiring

| ESP32 (loupe side) | | NodeMCU (home side) | second ESP32 (home side) |
|---|---|---|---|
| GPIO 17 · `TX2` | → | `D7` (GPIO 13) | GPIO 16 · `RX2` |
| GPIO 16 · `RX2` | ← | `D8` (GPIO 15) | GPIO 17 · `TX2` |
| `GND` | — | `G` | `GND` |
| `VIN` | ← 5 V | `VU` | `VIN` ↔ `VIN` |

UART at 2 Mbaud, framed with a CRC (`include/frame.h`). The ESP8266 moves its UART to D7/D8 with `Serial.swap()`;
its log output goes to `D4` (TX only, 115200 baud).

## First start

The home side starts in **hotspot mode**: WiFi `Platinenlupe-XXXX`, password `platinenlupe`. Join it and the
page opens by itself (captive portal), otherwise browse to <http://192.168.4.1>. Change the password under
**Device settings** — the page reminds you as long as the default is set. Modes: [docs/modes.md](../docs/modes.md).

**Factory reset:** hold `FLASH` (NodeMCU) or `BOOT` (ESP32) for 5 s right after power-on — the LED flickers, then
the board starts in hotspot mode with the default password.

## LEDs

| Board | LED | Meaning |
|---|---|---|
| loupe side | slow blink | searching for the loupe (switch it on) |
| | fast blink | joining the loupe's WiFi |
| | on | frames are flowing |
| home side | blinking | nobody connected and no home network |
| | on | somebody is watching |
| | off | ready |

## HTTP API of the home side

| Method | Path | |
|---|---|---|
| GET | `/` | the page (gzip) |
| GET | `/api/state` | JSON: `device`, `mode`, `stage`, `code`, battery, light, link statistics |
| GET | `/api/frame.jpg` | next complete frame; header `X-Angle` = rotation in degrees |
| POST | `/api/light` | `v=0…100` |
| GET | `/api/settings` | mode, hotspot name, WiFi name — **never passwords** |
| POST | `/api/settings` | `mode`, `wifi_ssid`, `wifi_password`, `ap_password` → saves and restarts |
| GET | `/api/scan` | nearby WiFi networks |

## Tests

```bash
g++ -std=c++17 -Iinclude test/frame_test.cpp -o /tmp/frame_test && /tmp/frame_test
```

Covers good frames, a flipped bit (CRC error), an oversized length and resynchronisation after garbage.

## Memory notes (ESP8266)

- The frame buffer (up to 24 KB) is allocated **after** WiFi is up. Allocated statically, it left the WiFi stack too
  little RAM — pages arrived cut off.
- One buffer only: the next frame is collected when the browser has fetched the previous one. The browser's
  fetch rate paces the stream.

## Flow control

The loupe delivers ~16 frames/s of up to 21 KB. Sent non-stop at 2 Mbaud, the ESP8266 could not keep up next to
its WiFi: its 128-byte UART FIFO overflowed and almost every frame arrived broken (measured: 1310 CRC errors,
0 frames). Now the home side grants frames one at a time (`READY`): one when its frame buffer is empty, up to
10/s in server mode. The loupe side only sends a whole frame when it holds a grant.

Measured on real hardware (NodeMCU in hotspot mode, loupe at −63 dBm): **3.9 frames/s**, 1 miss in 30 s.
The single frame buffer of the ESP8266 is the limit; a second ESP32 as home side has room for more.

## ESP32 UART pitfall

`Serial2.availableForWrite()` reports the largest **contiguous** free piece of the driver's no-split ring buffer —
at most half of it. Checking for room for a whole 20 KB frame therefore never succeeded. Frames now go out link
frame by link frame (≤ 1.6 KB each) from a single slot buffer, in which every chunk already sits in link-frame
layout. Two 64 KB buffers left too little RAM for WiFi and Bluetooth (boot loop, `esp_nimble_hci_init`
`ESP_ERR_NO_MEM`).

## Bluetooth note (ESP32)

The loupe side reports why it last restarted (`reset` in `/api/state` → `link.a`: `power_on`, `brownout`, `crash`, …).
If the loupe is already awake, it skips Bluetooth and joins the WiFi found by a scan (password = BSSID).

NimBLE is initialised once and **never deinitialised**. After `NimBLEDevice::deinit(true)` a pending NimBLE
timer fired into freed memory (`InstrFetchProhibited` in `os_callout_timer_cb`) and the board looped in reboots.
