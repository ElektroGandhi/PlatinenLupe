# Contributing

Thanks for wanting to make the loupe better! A few ground rules keep this project what it is:

1. **Local first.** No cloud services, no telemetry, no accounts, no CDN in anything that runs on the device or in
   the page. The page must work fully offline in hotspot mode.
2. **No app.** Everything happens in a browser.
3. **Measure, then claim.** Protocol facts, frame rates, model accuracy: write down how and on which device you
   measured. "Should work" goes into an issue, not into the docs.
4. **Small RAM.** The ESP8266 has ~40 KB of heap left with WiFi up. The page is embedded gzip-compressed into its
   firmware — keep `web/index.html` lean and dependency-free.

## Getting started

| Part | How to test |
|---|---|
| `web/` | open `web/index.html` directly in a browser — it starts a demo with a synthetic circuit board |
| `firmware/` | `g++ -std=c++17 -Ifirmware/include firmware/test/frame_test.cpp && ./a.out`, then `pio run` |
| `enclosure/` | `python enclosure/build.py` (OpenSCAD 2021.01+) — aborts on any warning or collision |

## Wanted

- Reports from other loupe models (chip, firmware number from *Loupe and connection*) — does it work?
- ESP32-S3 / C3 as the loupe side.
- Translations of `web/index.html` (the `T` object at the top of the script).
- Better prompts or models for part identification, with a measured comparison.

## Pull requests

One topic per PR, a short description of what you measured, and English in code and docs.
