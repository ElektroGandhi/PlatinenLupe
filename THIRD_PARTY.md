# Third-party components

Everything this project builds on is open source. Nothing proprietary is bundled.

## In the firmware images (`firmware/`, release `.bin` files)

| Component | Version | Licence | Use |
|---|---|---|---|
| [Arduino core for ESP32](https://github.com/espressif/arduino-esp32) (incl. ESP-IDF) | 2.0.17 (PlatformIO `espressif32@6.10.0`) | LGPL-2.1-or-later (ESP-IDF: Apache-2.0) | loupe side, ESP32 home side |
| [Arduino core for ESP8266](https://github.com/esp8266/Arduino) | 3.1.2 (PlatformIO `espressif8266@4.2.1`) | LGPL-2.1-or-later | NodeMCU home side |
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) | 1.4.3 | Apache-2.0 | Bluetooth LE on the loupe side |

The LGPL cores are linked statically. The complete source of this project and the exact versions above are public,
so anyone can rebuild and relink the firmware with a modified core (`cd firmware && pio run`).

## In the browser

| Component | Licence | Use |
|---|---|---|
| `web/index.html` | MIT (this project) | no third-party code; icons and pictures are drawn by this project |
| [ESP Web Tools](https://github.com/esphome/esp-web-tools) 10.1.0 | Apache-2.0 | only the flasher page loads it from unpkg.com — the device page itself loads nothing from the internet |

## Case (`enclosure/`)

| Component | Licence | Use |
|---|---|---|
| Lettering: DejaVu Sans Bold glyph outlines | Bitstream Vera licence (free, derivatives allowed) | turned into polygons by `lettering.py`, part of the badge STL |
| `tools/bambu_p1s_template.json` | AGPL-3.0 (derived from Bambu Studio profiles) | 3MF project settings — see `tools/NOTICE-bambu-template.md` |
| Python: [NumPy](https://numpy.org) (BSD-3-Clause), [Matplotlib](https://matplotlib.org) (Matplotlib licence, PSF-based), [trimesh](https://github.com/mikedh/trimesh) (MIT), [Pillow](https://python-pillow.org) (MIT-CMU) | | build, check and render scripts; not shipped |

## Tools (used, not shipped)

[PlatformIO](https://platformio.org) (Apache-2.0), esptool (GPL-2.0-or-later, merges the release images),
[OpenSCAD](https://openscad.org) (GPL-2.0), [Bambu Studio](https://github.com/bambulab/BambuStudio) CLI (AGPL-3.0, optional
test slicing), GitHub Actions from `actions/*` (MIT) and `softprops/action-gh-release` (MIT).

## Knowledge, not code

The loupe's protocol was cross-checked with [rbeilvert/otoscope](https://github.com/rbeilvert/otoscope); no code was
taken from it. No code, keys or assets of the camera vendor or the EarVision app are part of this repository.
