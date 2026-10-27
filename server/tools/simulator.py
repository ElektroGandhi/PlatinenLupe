"""Simulator: behaves like the home side in server mode with a loupe attached — for testing without hardware.

  python tools/simulator.py frame1.jpg frame2.jpg … [--fps 10] [--loss 0.02] [--state search]

Listens on UDP 61501/61500 like the loupe, sends frames only after "20 01" and stops 3 s after the last start
command. Broadcasts its state to 127.0.0.1:61499 every 2 s like the home side.
--loss drops random UDP packets (failure case: half frames must never be shown).
--state puts the loupe side into search/wake/join/connected; "cable" leaves it out entirely.
"""

import argparse
import json
import pathlib
import random
import socket
import sys
import threading
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from platinenlupe import protocol as p  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("frames", nargs="+", help="JPEG files to loop through")
    ap.add_argument("--fps", type=float, default=10)
    ap.add_argument("--loss", type=float, default=0.0)
    ap.add_argument("--state", default="connected", choices=["search", "wake", "join", "connected", "cable"])
    ap.add_argument("--beacon-to", default="127.0.0.1", help="where the beacon goes")
    a = ap.parse_args()
    jpegs = [pathlib.Path(f).read_bytes() for f in a.frames]

    video = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    video.bind(("0.0.0.0", p.PORT_VIDEO))
    control = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    control.bind(("0.0.0.0", p.PORT_CONTROL))
    beacon = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    beacon.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    st = {"peer": None, "last_start": 0.0, "frames": 0, "lost": 0}

    def video_commands():
        while True:
            data, sender = video.recvfrom(64)
            if data == p.VIDEO_START:
                st["peer"], st["last_start"] = sender, time.monotonic()
            elif data == p.VIDEO_STOP:
                st["peer"] = None

    def heartbeat():
        while True:
            data, sender = control.recvfrom(64)
            if data == p.HEARTBEAT:
                reply = {"battery_percentage": 87, "battery_charging": False, "device_chip": "SIMULATOR",
                         "device_version": 125, "width": 640, "height": 480, "fps": int(a.fps), "debug_rssi": -42}
                control.sendto(b"\x10\x01" + json.dumps(reply).encode(), sender)

    def announce():
        while True:
            side_a = None if a.state == "cable" else {
                "state": a.state, "ssid": "softish-5150A7", "rssi": -48, "frames": st["frames"],
                "dropped": 0, "aborted": 0, "crc": 0, "uptime": int(time.monotonic())}
            info = {"device": "platinenlupe", "v": 2, "firmware": "simulator", "a": side_a,
                    "b": {"rssi": -60, "crc": 0, "link_full": 0, "udp_errors": 0, "uptime": int(time.monotonic()), "web": True}}
            beacon.sendto(json.dumps(info).encode(), (a.beacon_to, p.PORT_BEACON))
            time.sleep(2)

    for f in (video_commands, heartbeat, announce):
        threading.Thread(target=f, daemon=True).start()
    print(f"Simulator ready: {len(jpegs)} frames, {a.fps} fps, loss {a.loss:.0%}, loupe side '{a.state}'", flush=True)

    n = 0
    while True:
        time.sleep(1 / a.fps)
        peer = st["peer"]
        if not peer or a.state != "connected" or time.monotonic() - st["last_start"] > 3:
            continue
        for packet in p.build_packets(jpegs[n % len(jpegs)], n, (n * 3) % 360):
            if random.random() < a.loss:
                st["lost"] += 1
                continue
            video.sendto(packet, peer)
        n += 1
        st["frames"] = n


if __name__ == "__main__":
    main()
