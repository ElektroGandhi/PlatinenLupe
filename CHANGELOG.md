# Changelog

## 1.1.0 — 2026-10-28

No server, no Docker any more.

- **Two modes** instead of three: hotspot (default) and home network.
- **Part identification runs in the browser:** it crops the focus ring and asks your vision model directly
  (LM Studio, Ollama, OpenAI-compatible). The board only stores the model's address under *Device settings*.
  Tested against LM Studio: SOIC-8 "LM358" recognised with high confidence in 5.3 s.
- The settings page shows the board's real address (`.local` names do not resolve in Chrome on Android).
- Settings from 1.0 are carried over on update (a board in "server" mode lands in home-network mode).
- Link between the boards at 1 Mbaud: at 2 Mbaud a distant loupe froze the picture in a loop.
- Removed: `server/` (Python, Docker), its simulator and tests.

## 1.0.0 — 2026-10-27

First public version.

- **Three modes** on the home-side board: hotspot (default, WPA2, captive portal, no network at all), home network
  (mDNS `platinenlupe.local`, fallback hotspot), server (UDP relay + discovery broadcast).
- Settings page on the device: mode, WiFi, hotspot password; passwords never leave the board.
- One bilingual page (English / German) for device, server and demo — retro-futurist design, round "porthole" view
  with rotation compensation, mirror, freeze, zoom, photo, light ring 0–100 %.
- Server: part identification in a tap-to-place focus ring with a local vision model (LM Studio, Ollama,
  OpenAI-compatible), simulator and test viewer.
- Firmware: whole-frame assembly on the ESP32, all-or-nothing transfer over a CRC-framed 2 Mbaud UART.
- Web flasher (ESP Web Tools), 3D-printable "atomic age capsule" case for an IKEA Skådis pegboard.
