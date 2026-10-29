# Hardware

## Shopping list

| Part | Notes |
|---|---|
| WiFi ear camera ("otoscope") whose WiFi is called `softish-XXXXXX` | A pen-shaped camera with an LED ring, 640 × 480, made for the **EarVision** app (`com.atomath.wifi_camera`). Tested: chip TXW810, firmware 125. The app also ships firmware for BK7252N-based models; those should speak the same protocol but are untested. |
| ESP32 DevKit (WROOM-32, 30 or 38 pins) | Loupe side. Needs Bluetooth — an ESP32-S2 will not do; ESP32-S3/C3 are untested. |
| NodeMCU v3 (ESP8266) **or** a second ESP32 | Home side. |
| 4 Dupont wires (female–female) | TX, RX, GND, 5 V |
| USB power supply + cable | One is enough for both boards. |
| Optional: 3D-printed case | [enclosure/](../enclosure/) — beige + a little red PLA (badge lettering brown and black), ~88 g, no supports |

## Wiring

```
        ESP32  (loupe side)                       NodeMCU (home side)
      ┌───────────────────┐                     ┌───────────────────┐
      │ GPIO17 / TX2  ●───┼────────────────────►┼──● D7  (RX)       │
      │ GPIO16 / RX2  ●◄──┼─────────────────────┼──● D8  (TX)       │
      │ GND           ●───┼─────────────────────┼──● G               │
      │ VIN           ●◄──┼───────── 5 V ───────┼──● VU              │
      └───────────────────┘                     └────────┬──────────┘
                                                         │ USB  ◄── the only power supply
```

Some ESP32 boards print `D16`/`D17` or `RX2`/`TX2` instead of `GPIO16`/`GPIO17` — same pins.

## Power

**One USB cable on the NodeMCU powers both boards**: `VU` carries the USB 5 V, the ESP32 takes it on `VIN`.

⚠️ Never feed the pair from **two** supplies while the 5 V wire is connected (for example USB into both boards).
The two 5 V rails would push against each other through the boards' protection diodes. To flash, either unplug the
5 V wire or power only the board you are flashing.

Typical datasheet peaks are ~240 mA for the ESP32 transmitting and ~170 mA for the ESP8266 — a 1 A phone charger is plenty. (Not measured on this build.)

## Range and placement

- Loupe ↔ ESP32: normal WiFi range, but the closer the better — the image is ~200 KB/s.
- The cable between the boards carries up to ~195 KB/s at 2 Mbaud; keep it short (< 30 cm). The NodeMCU runs at 160 MHz for this.
- In hotspot mode the phone is the only other device; a metre or ten is fine.
