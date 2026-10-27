# Three modes

The home-side board decides how you reach the loupe. Switch modes on the page under **Device settings** —
every mode keeps the full viewer (live image, light, straighten, mirror, freeze, zoom, photo).

| | Hotspot *(default)* | Home network | Server |
|---|---|---|---|
| Needs a router | no | yes | yes |
| Needs a computer | no | no | yes, any Docker host or Raspberry Pi |
| Address | `http://192.168.4.1` (opens by itself) | `http://platinenlupe.local` | your server, e.g. `http://pi.local:8080` |
| Part identification | – | – | ✓ with a local vision model |
| Internet | never | never needed | never needed |

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
- Several people can watch at the same time; the frame rate is shared.

## Home network — every browser in the house

The board joins your WiFi and serves the same page at `http://platinenlupe.local` (mDNS; on older Android use the
IP address shown in your router or on the page under *Loupe and connection*).

If the configured WiFi cannot be reached within 30 s after power-on, the board **also** opens its hotspot
(labelled *fallback hotspot* on the page) so you can always get back into the settings.

## Server — add part identification

Like home network, plus: the board behaves exactly like the loupe itself on UDP 61500/61501 and announces itself
by broadcast on UDP 61499. A small Python server ([server/](../server/)) finds it, streams the image over a
WebSocket and sends the part inside the focus ring to a **local** vision-language model (LM Studio, Ollama, or any
OpenAI-compatible server). See [identification.md](identification.md).

The board's own page keeps working at the same time — the server is an addition, never a dependency.

## Factory reset

Hold `FLASH` (NodeMCU) / `BOOT` (ESP32) for 5 s right after plugging in. Back to hotspot mode, default password.

## Privacy

- The firmware has no cloud code, no telemetry, no update check.
- Passwords are stored on the board only and are never sent back to the page (`/api/settings` only says
  *whether* a password is set).
- The server talks to the model server you configure and to nothing else.
