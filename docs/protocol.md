# Protocol of the loupe

The camera talks plain UDP inside its own WiFi. Everything below was read from the EarVision Android app and the
camera firmware and then **measured on a real device** (chip TXW810, firmware 125). The free viewer
[rbeilvert/otoscope](https://github.com/rbeilvert/otoscope) documents the same family and was used as a
cross-check — credit where it is due.

## 1. Find it — Bluetooth LE advertisement

| Field | Value |
|---|---|
| Name | `softish-XXXXXX` (also the WiFi name) |
| Manufacturer data | company ID `0x270F`, 12-byte payload |
| Payload bytes 0–5 | build stamp |
| Payload bytes 6–11 | BSSID of the camera's WiFi |

## 2. Wake it — one GATT read

Connect and **read** service `0x9900`, characteristic `0x0099` (the only characteristic the loupe has). The value
does not matter; the read itself makes the camera switch its WiFi on.

## 3. Join its WiFi

| | |
|---|---|
| SSID | the BLE name, `softish-XXXXXX` |
| Security | WPA2 |
| Password | the BSSID from the advertisement as 12 **lowercase** hex digits, e.g. `a4c1380d0eb0` |
| Camera address | `192.168.1.1` |

## 4. Video — UDP port 61501

| Send | Effect |
|---|---|
| `20 01` | start streaming to the sender's address and port; switches the light on at the last brightness |
| `20 02` | stop streaming — and switch the light off |

Every packet: a 24-byte header, then a slice of a JPEG.

| Offset | Type | Meaning |
|---|---|---|
| 0 | u8 | frame number (wraps) |
| 1 | u8 | chunk index, 0 = first |
| 2 | u8 | number of chunks in this frame |
| 3–6 | f32 LE | rotation in degrees from the camera's motion sensor (averaged by the firmware) |
| 7–23 | – | unused |
| 24… | bytes | JPEG data |

Measured: 640 × 480, ~16 frames/s, 6–21 KB per frame depending on light and subject. A frame with a missing chunk
must be dropped — half a JPEG decodes to coloured stripes.

## 5. Control — UDP port 61500

| Send | Reply | Notes |
|---|---|---|
| `10 01` | `01` + JSON | heartbeat + telemetry — **send it every second** (see below) |
| `10 02` | WebP image | the vendor's logo |
| `10 03 NN 00` | – | light ring brightness, `NN` = 0…100. Exactly 4 bytes or it is ignored. After power-on the brightness is 0. |
| `10 04 …` | – | ⛔ firmware update. Writes to flash. Never send it. |

Telemetry example:

```json
{"battery_percentage": 64, "battery_charging": true, "device_chip": "TXW810", "device_version": 125,
 "width": 640, "height": 480, "fps": 16, "debug_rssi": -51, "offset_degree": 0}
```

### The 3-second rule

Once the camera has received **one** control command, it switches camera and light off if no further control
command arrives for 3 s. With a heartbeat every 5 s you get a one-second black-out every 5 s — exactly 3 s after
each heartbeat (measured twice over 30 s). With a heartbeat every second: no gap in 60 s.

## Things that do not work

- **The button on the loupe** — a short press is not reported anywhere: not in the video header, not in telemetry,
  not on 199 other UDP ports, not over Bluetooth. Holding it switches the loupe off.
- **Tapping the loupe** as a gesture — the rotation value is averaged over 20 readings in the firmware; taps vanish.
