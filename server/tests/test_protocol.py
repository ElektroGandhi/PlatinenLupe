"""Tests for protocol.py — run with pytest, or plainly: python tests/test_protocol.py"""

import pathlib
import random
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from platinenlupe import protocol as p  # noqa: E402

JPEG = b"\xff\xd8" + bytes(random.Random(1).randrange(256) for _ in range(5000)) + b"\xff\xd9"


def test_frame_arrives_whole():
    a = p.FrameAssembler()
    packets = p.build_packets(JPEG, 7, 42.5)
    assert len(packets) == 4
    results = [a.feed(x) for x in packets]
    assert results[:-1] == [None] * 3
    jpeg, angle = results[-1]
    assert jpeg == JPEG and angle == 42.5


def test_out_of_order():
    a = p.FrameAssembler()
    packets = p.build_packets(JPEG, 1)
    done = [r for r in (a.feed(x) for x in [packets[0], packets[2], packets[1], packets[3]]) if r]
    assert done and done[0][0] == JPEG


def test_missing_chunk_drops_the_frame():
    """Failure case: one lost UDP packet must never produce half a frame."""
    a = p.FrameAssembler()
    one, two = p.build_packets(JPEG, 1), p.build_packets(JPEG, 2)
    out = [a.feed(x) for x in one[:2] + one[3:] + two]
    done = [r for r in out if r]
    assert len(done) == 1 and done[0][0] == JPEG     # only frame 2
    assert a.dropped == 1


def test_foreign_middle_chunk_is_ignored():
    a = p.FrameAssembler()
    one = p.build_packets(JPEG, 1)
    assert a.feed(one[2]) is None                     # start never seen
    assert a.feed(b"\x00" * 10) is None               # too short
    assert [r for r in (a.feed(x) for x in one) if r][0][0] == JPEG


def test_missing_end_marker_is_added():
    a = p.FrameAssembler()
    (r,) = [x for x in (a.feed(k) for k in p.build_packets(JPEG[:-2], 3)) if x]
    assert r[0].endswith(b"\xff\xd9")


def test_header_format():
    k = p.build_packets(JPEG, 300, 1.0)[1]
    assert k[0] == 300 & 0xFF and k[1] == 1 and k[2] == 4
    assert struct.unpack_from("<f", k, 3)[0] == 1.0 and len(k) == p.HEADER + 1400


def test_advertisement():
    payload = bytes(6) + bytes([0xA4, 0xC1, 0x38, 0x0D, 0x0E, 0xB0])
    w = p.parse_advertisement("softish-0D0EB0", {0x270F: payload})
    assert w == {"ssid": "softish-0D0EB0", "bssid": "A4:C1:38:0D:0E:B0", "password": "a4c1380d0eb0"}
    assert p.parse_advertisement("softish-x", {0x004C: payload}) is None     # other company
    assert p.parse_advertisement(None, {0x270F: payload}) is None
    assert p.parse_advertisement("softish-x", {0x270F: payload[:8]}) is None


def test_light_command_is_exactly_four_bytes():
    assert p.light(70) == bytes([0x10, 0x03, 70, 0])
    assert p.light(250) == bytes([0x10, 0x03, 100, 0]) and p.light(-5) == bytes([0x10, 0x03, 0, 0])


def test_telemetry():
    raw = b'\x10\x01{"battery_percentage":64,"battery_charging":true,"device_chip":"TXW810",' \
          b'"device_version":125,"width":1280,"height":720,"fps":25,"debug_rssi":-51}'
    assert p.parse_telemetry(raw) == {"battery": 64, "charging": True, "chip": "TXW810", "loupe_firmware": 125,
                                      "resolution": "1280×720", "camera_fps": 25, "loupe_rssi": -51}
    assert p.parse_telemetry(b"\x10\x01broken{") == {}
    assert p.parse_telemetry(b'{"battery_percentage":300}') == {}


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
