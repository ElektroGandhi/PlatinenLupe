# Two modes

The home-side board decides how you reach the loupe. Switch on the page under **Device settings** — both modes
have the full viewer (live image, light, straighten, mirror, freeze, zoom, photo).

| | Hotspot *(default)* | Home network |
|---|---|---|
| Needs a router | no | yes |
| Needs a computer | no | no |
| Address | `http://192.168.4.1` (opens by itself) | the board's address, shown under *Device settings* (computers also: `http://platinenlupe.local`) |
| Part identification | – | ✓ with a vision model somewhere in your network |
| Internet | never | never needed |

## Hotspot — the loupe in your pocket

```
loupe ))) ESP32 ═══ NodeMCU ((( "Platinenlupe-3F2A" ((( phone / laptop
```

The board opens its own WPA2 WiFi `Platinenlupe-XXXX` (the last four characters come from the chip). Join it and
your phone shows the loupe straight away: a tiny DNS server answers every name with the board's own address, so the
phone's "sign in to network" check lands on the page. Nothing leaves the room. Nothing needs to be installed.

- Default password: `platinenlupe`. **Change it** under *Device settings → Hotspot password* (8–63 characters).
  Until you do, the page shows a reminder.
- Works on a workbench, at a repair café, in the field — anywhere with USB power.
- A laptop that is in the hotspot loses its internet while it stays there — every name points to the board.

## Home network — every browser in the house, plus part identification

The board joins your WiFi and serves the same page at its address. The settings card shows that address; your
router lists it as `platinenlupe` too. `http://platinenlupe.local` works on computers, not in Chrome on Android.

Enter a **vision model** under *Device settings* (address of LM Studio, Ollama or an OpenAI-compatible server) and
the page identifies the part in the focus ring — your browser asks the model directly, no server of ours in between.
See [identification.md](identification.md).

If the configured WiFi cannot be reached within 30 s after power-on, the board **also** opens its hotspot
(labelled *fallback hotspot* on the page) so you can always get back into the settings.

## Factory reset

Hold `FLASH` (NodeMCU) / `BOOT` (ESP32) for 5 s right after plugging in. Back to hotspot mode, default password.

## Privacy

- The firmware has no cloud code, no telemetry, no update check.
- Passwords are stored on the board only and are never sent back to the page (`/api/settings` only says
  *whether* a password is set).
- The page talks to the board and — only if you entered one — to your model server. Nothing else.
