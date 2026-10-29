# Changelog

## Unreleased — case 2

After the first real print (2026-10-29). Case only, no firmware change.

- **No glue any more.** Every part clicks in: snap prongs for fin, quiver, Saturn and hooks; clip tongues
  for ring, stripes, badge and light frames; each tongue checked to ≤ 2 % bending strain.
- **Boards latch into the body** (four arms per board over the PCB edge): the Dupont plugs cannot push
  them up, with or without the lid. Hold-downs in the lid now lean 45° and press 0.2 mm as a second safety.
- **USB slider** under the bottom: only one USB socket is open at a time (RUN = NodeMCU, FLASH = ESP32),
  so two power supplies cannot meet. Two click positions, engraved labels. The case now stands on a plinth.
- **Light windows fixed:** the 0.6 mm skin only touched the lid along an edge and printed as a loose plate
  (two holes instead of two lights). It now reaches 1.2 mm under the full lid.
- **Quiver** moved 4.5 mm away from the wall: its flared top jammed against the wall when plugging it in.
- Clearance in all slots and recesses 0.2 mm (was 0.15).
- `build.py`: counts bodies layer by layer as the printer sees them (catches edge-only contact), plus
  retention, clearance and "plug blocked" checks — 24 in total.

## 1.2.0 — 2026-10-30

Faster picture, measured on the real boards.

- **Frames go to the browser while they still arrive over the cable.** The loupe side announces each frame's
  length; the home side streams it, so frames larger than its buffer pass too (up to 33 KB measured).
  Older loupe-side firmware still works (whole frames).
- **Link at 2 Mbaud**, NodeMCU at 160 MHz, lwIP with full 1460-byte segments, faster link reader (block reads,
  table CRC). 10 minutes with 21–33 KB frames: 3.7 frames/s, no restart; small frames 7–10 frames/s.
- The home side reports its reset reason and uptime in `/api/state` (`link.b.reset`, `link.b.uptime`).
- Page: a frame cut off on the cable is fetched again after 100 ms instead of 1 s.
- ⚠️ Flash **both** boards: the link rate changed.

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
