# Enclosure: the "atomic age capsule"

![Assembled case, front view](../docs/images/case_front.png)

A 3D-printable case in 1950s retro-futurist style: porthole with an atom emblem, speed stripes,
tail fin, Saturn antenna and a silo-shaped quiver. It holds both boards of the Platinenlupe:

- **board A**: ESP32 DevKit V1 (30 pins), right side, marked "A"
- **board B**: NodeMCU v3 LoLin (ESP8266), left side, marked "B"

The quiver on the left holds the loupe pen. The whole case hangs on an **IKEA Skådis** pegboard.
Colours: beige body, brown accents. Every colour has its own print plate, so there is no filament
change except on the small two-colour name badge.

| File | Content |
|---|---|
| `platinenlupe_case.scad` | the model; all dimensions are parameters at the top |
| `lettering.py` → `lettering.scad` | letters as polygons (see [Pitfalls](#pitfalls)) |
| `check.scad` | dummies for both boards, plugs and pen; collisions as intersections |
| `build.py` | builds everything: lettering, STLs, mesh and collision checks, 3MF, test slice |
| `tools/bambu_3mf.py`, `tools/bambu_p1s_template.json` | 3MF writer for Bambu Studio, P1S/PLA settings (the template is AGPL-3.0, see `tools/NOTICE-bambu-template.md`) |
| `tools/render.py` | renders the pictures in `docs/images/` from the STLs |
| `print/` | ready-to-print STLs and `platinenlupe_case.3mf` |

## Build

```sh
pip install trimesh matplotlib numpy pillow
python build.py                 # STLs + checks + 3MF, then a test slice if Bambu Studio is found
python build.py --no-slice
python tools/render.py          # optional: README pictures
```

`build.py` writes intermediate files to `~/.cache/platinenlupe-enclosure` (`--work`) and copies the
printable STLs and the 3MF to `print/` (`--print-dir`). It aborts on **any** OpenSCAD warning, on a
mesh that is not watertight, on a wrong number of solids per part, and on any collision.

Tools are found like this (override with environment variables or `--openscad` / `--bambu-studio`):

| Variable | Default |
|---|---|
| `OPENSCAD` | `openscad` on `PATH`, else `flatpak run org.openscad.OpenSCAD`, else `flatpak-spawn --host flatpak run org.openscad.OpenSCAD` (when `build.py` itself runs inside a Flatpak, e.g. an IDE) |
| `BAMBU_STUDIO` | `bambu-studio` on `PATH`; if missing, the test slice is skipped with a message |

Example for an extracted Bambu Studio AppImage, started from inside a Flatpak sandbox:

```sh
BAMBU_STUDIO="flatpak-spawn --host $HOME/Apps/bambu-studio/squashfs-root/AppRun" python build.py
```

Sandboxed tools (Flatpak OpenSCAD, `flatpak-spawn --host`) usually see only your home directory.
Keep `--work` under `~`, not in `/tmp`.

## 1. Measure before printing (2 minutes, calipers)

The ESP32 board size comes from **one** source only. Please measure, enter any difference at the
top of `platinenlupe_case.scad`, and run `python build.py` again.

| What | In the model | Confirmed |
|---|---|---|
| Loupe pen: diameter × length, cap length | 13.1 × 142.7 mm, cap 50 mm (`pen_d`, `pen_l`, `cap_l`) | ✅ measured 2026-10-27 |
| NodeMCU: PCB length × width | 58.0 × 31.0 mm (`nodemcu`) | ☐ |
| ESP32 30 pins: PCB length × width | 51.8 × 28.2 mm (`esp32`) | ☐ |
| PCB underside to the end of a Dupont socket | ≤ 25 mm (`cable_h`) | ☐ |
| Your USB-C plug including strain relief | ≤ 13.5 × 9 mm (`usb_w`, `usb_h`) | ☐ |
| Skådis board thickness | 5.2 mm (`pegboard_t` − 0.4) | ☐ |
| NodeMCU on USB: voltage VU to G (multimeter) | 5 V expected, **not confirmed** | ☐ |

## 2. Print (Bambu Lab, AMS: slot 1 beige, slot 2 brown)

![The three print plates](../docs/images/case_parts.png)

Open `print/platinenlupe_case.3mf`. Three plates, **no supports**:

| Plate | Parts | Time | Filament |
|---|---|---|---|
| 1 Beige | body (back wall down), lid (face side down), 2 hooks | 1 h 48 min | 67.0 g |
| 2 Brown | porthole ring, speed stripes, 2 light frames, tail fin, Saturn, quiver (upright) | 30 min | 10.4 g |
| 3 Badge | name badge in brown with beige lettering inlaid (face side down, 2 filament changes) | 9 min | 2.6 g |

Sliced with the Bambu Studio CLI, P1S profile, 0.4 mm nozzle, PLA, 0.20 mm layers (2026-10-27).
The plain STLs in `print/` work in any slicer.

The two light bands (3–13 mm from the USB end, almost as wide as the PCB) let the board LEDs shine
through 0.6 mm of beige. The exact LED position is not measured; the band covers the usual area.

## 3. Assembly

1. Glue the **hooks** into the two pockets in the back wall with a drop of superglue, leg pointing down.
2. Press the **brown parts** into the recesses of the lid (0.15 mm clearance). If one is loose: a drop of glue.
3. **Tail fin** into the right side slot, **quiver** with its tenon into the lower slot on the left,
   **Saturn** into the slot on top. One drop of glue each.
4. Put the **boards** in from the front, USB socket down into the notch. Each PCB rests on a support
   with a small rim at both ends.
5. **Wire** them (Dupont, female–female), then press the lid on. Its latches snap in, and four
   hold-downs press the boards onto the supports.

### Wiring

| ESP32 (board A, right) | Wire | NodeMCU (board B, left) |
|---|---|---|
| GPIO 17 (TX2) | → | D7 (GPIO 13, RX after `Serial.swap()`) |
| GPIO 16 (RX2) | ← | D8 (GPIO 15, TX) |
| GND | — | G |
| VIN | ← | VU (5 V from the NodeMCU's USB) |

### Power rule

**One** USB-C cable, plugged into the **NodeMCU**, powers both boards: the ESP32 gets 5 V through
VU → VIN.

- ⚠️ **Never connect two power supplies while VU–VIN is wired.** The case to avoid: a USB cable in
  the ESP32 while the NodeMCU's USB is powered too.
- Flashing: one board at a time on its own USB cable is fine.

## 4. Mounting

Two hooks, 40 mm apart, go into **one** row of the Skådis board: push in, then lower by 7 mm.
The vertical hole pitch of the Skådis board is not confirmed, so there is only one pair of hooks.

Stand the loupe pen **cap down** in the quiver. The quiver is 55 mm deep (cap 50 mm plus floor),
so about 90 mm of the pen sticks out at the top to grab it. A 9 mm hole in the floor lets dust fall
through, and the lens never rests on anything.

## Collision check

`build.py` places dummies for both boards in the case (PCB, pin headers, Dupont sockets on the
outermost pins, radio module, USB-C socket, buttons) and computes the intersections with OpenSCAD.
The build stops on any collision.

| Check | Target | Result |
|---|---|---|
| Boards vs. body | 0 mm³ | 0.00 mm³ |
| Boards vs. lid | 0 mm³ | 0.00 mm³ |
| USB-C plug (12.4 × 6.5 mm) up to the socket | 0 mm³ | 0.00 mm³ |
| Lid vs. body | 0 mm³ (faces touch) | 0.00 mm³ |
| Quiver vs. body | 0 mm³ | 0.00 mm³ |
| Pen in the quiver | 0 mm³ | 0.00 mm³ |
| Hold-downs touch the PCBs | > 0 | 4.50 mm³ (all four) |

⚠️ **Dummy assumptions, not measured:** pin headers centred along the PCB, ESP32 row spacing
25.4 mm, module and button heights. If you measure your board, enter it at the top of `check.scad`.

## Pitfalls

- **`text()` renders nothing in the OpenSCAD 2021.01 Flatpak.** "Current top level object is empty",
  no error, also with `use <…ttf>` and `OPENSCAD_FONT_PATH`. That is why `lettering.py` turns the
  letters into polygons with matplotlib. A lid without lettering does not fail any mesh check.
- **OpenSCAD builds even with an undefined variable** — a warning, but exit code 0. `build.py`
  therefore aborts on every warning.
- **Touching faces give loose bodies.** Hold-downs and latches that only touched the lid became
  separate solids. They now reach 0.1 mm into it, and `build.py` counts the solids per part.
- **PNG export fails in the OpenSCAD Flatpak** (no offscreen GL context). `tools/render.py` uses a
  small software rasteriser instead.
- **STL output is not byte-stable.** OpenSCAD may write the same triangles in a different order on
  every run, so the files in `print/` change in git even when the geometry does not. Compare
  geometry as a set of triangles, not by file hash.

## License

The 3D design files are licensed CC BY-SA 4.0, the scripts MIT. See [LICENSE](LICENSE).
