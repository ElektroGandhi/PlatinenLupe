"""Protocol of the WiFi loupe "softish-XXXXXX" (sold with the EarVision app). Details: docs/protocol.md.

Reverse-engineered from the Android app (static analysis of the Dart AOT snapshot) and the camera firmware,
then measured on the real device. Standard library only.

  1. BLE advertisement: manufacturer data, company ID 0x270F, 12-byte payload.
     Bytes 0–5 build stamp, bytes 6–11 BSSID of the camera's WiFi.
  2. Wake-up: a GATT read of service 0x9900 / characteristic 0x0099. Only then does the WiFi appear.
  3. WiFi: SSID = BLE name, WPA2, password = BSSID as 12 lowercase hex digits. Camera at 192.168.1.1.
  4. Video: UDP to 61501, "20 01" start, "20 02" stop (also switches the light off).
  5. Control: UDP to 61500, "10 01" EVERY SECOND — the reply carries JSON telemetry. Once it has received
     one control command, the loupe switches camera and light off when none arrives for 3 s.
"""

from __future__ import annotations

import json
import struct

COMPANY_ID = 0x270F
WAKE_SERVICE = "00009900-0000-1000-8000-00805f9b34fb"
WAKE_CHARACTERISTIC = "00000099-0000-1000-8000-00805f9b34fb"
CAMERA_IP = "192.168.1.1"
PORT_VIDEO = 61501
PORT_CONTROL = 61500
PORT_BEACON = 61499
VIDEO_START = bytes([0x20, 0x01])
VIDEO_STOP = bytes([0x20, 0x02])
HEARTBEAT = bytes([0x10, 0x01])
GET_LOGO = bytes([0x10, 0x02])        # reply: the vendor's logo as WebP
HEADER = 24                           # bytes of header per UDP packet
MIN_FRAME = 1000                      # smaller "frames" are leftovers of an aborted one


def light(percent: int) -> bytes:
    """Light ring 0–100 %. Exactly 4 bytes, otherwise the loupe ignores it. No reply.
    After power-on the light is at 0, so it has to be sent again after every restart of the loupe."""
    return bytes([0x10, 0x03, max(0, min(100, int(percent))), 0x00])


def parse_advertisement(name: str | None, manufacturer: dict[int, bytes]) -> dict | None:
    """BLE advertisement → {ssid, bssid, password}, or None if it is not a loupe."""
    payload = manufacturer.get(COMPANY_ID)
    if not name or payload is None or len(payload) < 12:
        return None
    b = payload[6:12]
    return {"ssid": name, "bssid": ":".join(f"{x:02X}" for x in b), "password": "".join(f"{x:02x}" for x in b)}


class FrameAssembler:
    """Reassembles frames from UDP chunks.

    Header of every packet:
      0     u8   frame number (wraps)
      1     u8   chunk index, 0 = first
      2     u8   number of chunks in this frame
      3–6   f32  rotation in degrees, little-endian (the camera's motion sensor)
      7–23       unused
      24…        JPEG data

    If a chunk is missing, the frame is dropped: half a JPEG shows coloured stripes.
    """

    def __init__(self) -> None:
        self.number = -1
        self.expected = 0
        self.angle = 0.0
        self.chunks: dict[int, bytes] = {}
        self.dropped = 0

    def feed(self, packet: bytes) -> tuple[bytes, float] | None:
        """One packet in; a complete frame (jpeg, angle) out, or None."""
        if len(packet) < HEADER:
            return None
        number, index, total = packet[0], packet[1], packet[2]
        angle = struct.unpack_from("<f", packet, 3)[0]
        data = packet[HEADER:]

        if index == 0:
            done = self._finish()
            self.number, self.expected, self.angle = number, total, angle
            self.chunks = {0: data}
            return done or self._if_complete()

        if number != self.number or self.number < 0:
            done = self._finish()
            self.number = -1
            self.chunks = {}
            return done

        self.chunks[index] = data
        return self._if_complete()

    def _if_complete(self) -> tuple[bytes, float] | None:
        if self.expected <= 0 or len(self.chunks) < self.expected:
            return None
        return self._finish()

    def _finish(self) -> tuple[bytes, float] | None:
        if not self.chunks:
            return None
        complete = self.expected > 0 and len(self.chunks) == self.expected
        chunks, self.chunks, self.number = self.chunks, {}, -1
        if not complete:
            self.dropped += 1
            return None
        jpeg = b"".join(chunks[i] for i in sorted(chunks))
        if not jpeg.endswith(b"\xff\xd9"):
            jpeg += b"\xff\xd9"
        if len(jpeg) < MIN_FRAME:
            return None
        return jpeg, self.angle


def parse_telemetry(packet: bytes) -> dict:
    """Reply to the heartbeat → fields for the page. A few bytes precede the JSON."""
    text = packet.decode("utf-8", "replace")
    start = text.find("{")
    if start < 0:
        return {}
    try:
        raw = json.loads(text[start:text.rfind("}") + 1])
    except ValueError:
        return {}
    out: dict = {}
    if isinstance(raw.get("battery_percentage"), int) and 0 <= raw["battery_percentage"] <= 100:
        out["battery"] = raw["battery_percentage"]
        out["charging"] = bool(raw.get("battery_charging", False))
    if raw.get("device_chip"):
        out["chip"] = str(raw["device_chip"])
    if isinstance(raw.get("device_version"), int):
        out["loupe_firmware"] = raw["device_version"]
    if isinstance(raw.get("width"), int) and isinstance(raw.get("height"), int):
        out["resolution"] = f'{raw["width"]}×{raw["height"]}'
    if isinstance(raw.get("fps"), int):
        out["camera_fps"] = raw["fps"]
    if isinstance(raw.get("debug_rssi"), int):
        out["loupe_rssi"] = raw["debug_rssi"]
    return out


def build_packets(jpeg: bytes, number: int, angle: float = 0.0, chunk_size: int = 1400) -> list[bytes]:
    """The counterpart of FrameAssembler — for the simulator and the tests."""
    parts = [jpeg[i:i + chunk_size] for i in range(0, len(jpeg), chunk_size)] or [b""]
    rest = bytes(HEADER - 7)
    return [bytes([number & 0xFF, i, len(parts)]) + struct.pack("<f", angle) + rest + p for i, p in enumerate(parts)]
