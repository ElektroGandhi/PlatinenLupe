"""Connection to the loupe — through the home-side board (server mode) or directly (PL_DEVICE_IP).

The home side speaks exactly the loupe's protocol (protocol.py) and broadcasts its state every 2 s on
UDP 61499. From that the page learns which link of the chain is missing.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import socket
import time

from . import protocol as p

log = logging.getLogger("platinenlupe.loupe")


class _Receiver(asyncio.DatagramProtocol):
    def __init__(self, target) -> None:
        self.target = target

    def datagram_received(self, data: bytes, sender) -> None:
        self.target(data, sender)


class Loupe:
    def __init__(self, on_frame, fixed_ip: str | None = None) -> None:
        self.on_frame = on_frame                      # on_frame(jpeg, angle)
        self.fixed_ip = fixed_ip or os.environ.get("PL_DEVICE_IP") or None
        self.beacon: dict | None = None               # last broadcast of the home side
        self.beacon_ip: str | None = None
        self.beacon_time = 0.0
        self.telemetry: dict = {}
        self.light = int(os.environ.get("PL_LIGHT", "70"))
        self.running = False
        self.last_frame = 0.0
        self.frames = 0
        self._window: list[float] = []
        self._assembler = p.FrameAssembler()
        self._video = self._control = None
        self._tasks: list[asyncio.Task] = []

    # ---- Beacon of the home side, always listening ------------------------------------
    async def listen(self) -> None:
        loop = asyncio.get_running_loop()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", p.PORT_BEACON))
        await loop.create_datagram_endpoint(lambda: _Receiver(self._on_beacon), sock=sock)

    def _on_beacon(self, data: bytes, sender) -> None:
        try:
            info = json.loads(data)
        except ValueError:
            return
        if not isinstance(info, dict) or info.get("device") != "platinenlupe":
            return
        if sender[0] != self.beacon_ip:
            log.info("Home side found at %s (firmware %s)", sender[0], info.get("firmware"))
        self.beacon, self.beacon_ip, self.beacon_time = info, sender[0], time.monotonic()

    @property
    def target_ip(self) -> str | None:
        if self.fixed_ip:
            return self.fixed_ip
        return self.beacon_ip if time.monotonic() - self.beacon_time < 10 else None

    # ---- Video stream, only while somebody watches ------------------------------------
    async def start(self) -> None:
        if self.running:
            return
        self.running = True
        loop = asyncio.get_running_loop()
        v = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        v.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        v.bind(("0.0.0.0", 0))
        self._video, _ = await loop.create_datagram_endpoint(lambda: _Receiver(self._on_video), sock=v)
        self._control, _ = await loop.create_datagram_endpoint(
            lambda: _Receiver(self._on_control), local_addr=("0.0.0.0", 0))
        self._tasks = [asyncio.create_task(self._tick())]
        log.info("Video stream requested")

    async def stop(self) -> None:
        if not self.running:
            return
        self.running = False
        for task in self._tasks:
            task.cancel()
        ip = self.target_ip
        if ip and self._video:
            for _ in range(5):
                self._video.sendto(p.VIDEO_STOP, (ip, p.PORT_VIDEO))
        for transport in (self._video, self._control):
            if transport:
                transport.close()
        self._video = self._control = None
        log.info("Video stream stopped")

    def set_light(self, percent: int) -> None:
        self.light = max(0, min(100, int(percent)))
        ip = self.target_ip
        if ip and self._control:
            self._control.sendto(p.light(self.light), (ip, p.PORT_CONTROL))

    async def _tick(self) -> None:
        """Start and heartbeat every second; light every 5 s (it is 0 again after a restart of the loupe)."""
        n = 0
        while True:
            ip = self.target_ip
            if ip:
                try:
                    for _ in range(3 if n == 0 else 1):
                        self._video.sendto(p.VIDEO_START, (ip, p.PORT_VIDEO))
                    # Every second, not every 5 s: after its first control command the loupe switches camera
                    # and light off when none arrives for 3 s — a 5 s heartbeat means a 1 s gap every 5 s.
                    self._control.sendto(p.HEARTBEAT, (ip, p.PORT_CONTROL))
                    if n % 5 == 1:
                        self._control.sendto(p.light(self.light), (ip, p.PORT_CONTROL))
                except OSError as e:
                    log.warning("Sending to %s: %s", ip, e)
                n += 1
            await asyncio.sleep(1)

    def _on_video(self, data: bytes, sender) -> None:
        done = self._assembler.feed(data)
        if done is None:
            return
        now = time.monotonic()
        self.last_frame = now
        self.frames += 1
        self._window = [t for t in self._window if now - t < 2] + [now]
        self.on_frame(*done)

    def _on_control(self, data: bytes, sender) -> None:
        self.telemetry.update(p.parse_telemetry(data))

    # ---- State for the page ------------------------------------------------------------
    def state(self) -> dict:
        """stage live | wait | error and a code; the page turns the code into a sentence in its language."""
        now = time.monotonic()
        live = now - self.last_frame < 3
        a = (self.beacon or {}).get("a") if self.target_ip and not self.fixed_ip else None
        if live:
            stage, code = "live", "live"
        elif not self.target_ip:
            stage, code = "error", "no_device"
        elif self.fixed_ip:
            stage, code = "wait", "direct"
        elif a is None:
            stage, code = "error", "no_link"
        else:
            stage = "wait"
            code = {"search": "searching", "wake": "waking", "join": "joining"}.get(a.get("state"), "first_frame")
        return {
            "stage": stage, "code": code, "fps": round(len(self._window) / 2, 1) if live else 0,
            "light": self.light, "ip": self.target_ip, "firmware": (self.beacon or {}).get("firmware"),
            "link": {"a": a, "b": (self.beacon or {}).get("b")}, "dropped": self._assembler.dropped, **self.telemetry,
        }
