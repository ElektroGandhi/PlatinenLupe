# How the loupe was cracked

*The short story of turning a cheap WiFi ear camera into an app-free PCB microscope.*

## The itch

A WiFi "otoscope" turns out to be a brilliant loupe for circuit boards: tiny, bright LED ring, sharp at a few
millimetres. But it only works with a phone app — one more app, one more account screen, ads, and no way to get the
image onto a laptop or share it with the person next to you.

Goal: **the same picture in any browser, without the app, without the cloud.**

## Step 1 — the app

The Android package (`EarVision 1.3.11`) unpacks into a Flutter app. Java decompilation (JADX) shows only a thin
shell; the logic sits in the Dart AOT snapshot `libapp.so`. Snapshots are hard to decompile, but they keep constants:

- the strings `00009900-…` and `00000099-…` — a Bluetooth service and characteristic,
- the numbers 61500 and 61501 as immediate operands in the machine code — UDP ports,
- JSON keys `battery_percentage`, `device_chip`, `debug_rssi` — the telemetry.

The open-source viewer [rbeilvert/otoscope](https://github.com/rbeilvert/otoscope) confirmed the overall shape: wake
over BLE, join the camera's WiFi, ask for video over UDP.

## Step 2 — the firmware

The app carries camera firmware for over-the-air updates. One family is an RT-Thread "RBL" package, AES-256-CBC
encrypted — with the **default key from the chip vendor's public SDK** — and then gzipped. Decrypted, it is plain
ARM/Thumb code (load address `0x10000`); wrapped into an ELF with Thumb symbols, Ghidra reads it well. That is where
the light command (`10 03 NN 00`, exactly four bytes) and the meaning of `20 02` (video *and* light off) came from.
The other firmware family is C-SKY code, for which Ghidra has no processor module.

## Step 3 — the bench

Theory is cheap; every claim in [protocol.md](protocol.md) was measured on the real device:

- The WiFi password is the BSSID in lowercase hex — tried, works.
- The picture froze for exactly one second every five seconds. The official app does not have that problem, the
  open-source viewer does. Measuring the gaps showed the rule: **after the first control command, the camera wants a
  heartbeat at least every 3 s** — the viewer sent one every 5 s. One per second: no gap in 60 s.
- The light was "not adjustable" at first, because `10 02` (which fetches the vendor logo) looked like the obvious
  candidate. The firmware said `10 03`.
- The button: pressed while watching every channel — video headers, telemetry, 199 more UDP ports, Bluetooth.
  Nothing. The firmware has a button task but keeps the event to itself.

## Step 4 — two boards

The camera **always** runs its own WiFi and cannot join yours. A phone can be in only one WiFi at a time. So: one
ESP32 lives in the camera's WiFi (and wakes it over Bluetooth), a second board lives where you are — as a hotspot
or in your home network. Three data wires between them (plus 5 V) carry complete frames at 1 Mbaud.

## Ground rules

- Interoperability only: nothing here circumvents a purchase, a licence or a lock; the camera's own WiFi password
  is derived exactly like the official app derives it.
- No vendor code or keys are part of this repository.
- Never send the firmware update command (`10 04`).
