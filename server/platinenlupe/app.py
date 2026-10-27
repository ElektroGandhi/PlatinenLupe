"""Web server: serves web/index.html, streams frames and identification results over one WebSocket.

To the page:
  text   {"t": "frame", "angle": a}   immediately followed by the JPEG as a binary message
  text   {"t": "state", …}            every second
  text   {"t": "result", …}           after every identification
From the page:
  {"t": "light", "v": 0…100}   {"t": "focus", "x", "y", "r"}   {"t": "identify"}   {"t": "continuous", "on"}
  every message carries "lang" ("en" | "de") — the language of the model's one-sentence note.

Run: python -m platinenlupe    Settings: environment variables PL_*, see server/README.md.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import pathlib
import time

from aiohttp import WSMsgType, web

from . import __version__
from .identify import IdentifyLoop
from .loupe import Loupe

log = logging.getLogger("platinenlupe")


def _find_page() -> pathlib.Path:
    here = pathlib.Path(__file__).resolve()
    for candidate in (os.environ.get("PL_WEB_DIR"), here.parent / "web", here.parents[2] / "web"):
        if candidate and (pathlib.Path(candidate) / "index.html").exists():
            return pathlib.Path(candidate) / "index.html"
    raise FileNotFoundError("web/index.html not found — set PL_WEB_DIR")


class Server:
    def __init__(self) -> None:
        self.loop: asyncio.AbstractEventLoop | None = None
        self.viewers: dict[web.WebSocketResponse, asyncio.Queue] = {}
        self.idle_s = float(os.environ.get("PL_IDLE_S", "30"))
        self.identify_enabled = os.environ.get("PL_IDENTIFY", "1") != "0"
        self._stop_task: asyncio.Task | None = None
        self.identify = IdentifyLoop(self._identified)
        self.last_result: dict | None = None
        self.loupe = Loupe(self.on_frame)
        self.page = _find_page()

    # ---- Frames -------------------------------------------------------------------------
    def on_frame(self, jpeg: bytes, angle: float) -> None:
        if self.identify_enabled:
            self.identify.submit(jpeg)
        header = json.dumps({"t": "frame", "angle": round(angle, 1)})
        for queue in self.viewers.values():
            if queue.full():                          # slow phone: drop the old frame, never fall behind
                queue.get_nowait()
            queue.put_nowait((header, jpeg))

    def _identified(self, result: dict) -> None:      # runs in the identify thread
        self.loop.call_soon_threadsafe(self._send_result, result)

    def _send_result(self, result: dict) -> None:
        self.last_result = result
        text = json.dumps({"t": "result", **result}, ensure_ascii=False)
        for ws in list(self.viewers):
            asyncio.create_task(ws.send_str(text))

    def state(self) -> dict:
        s = self.loupe.state()
        s.update({"t": "state", "server": True, "version": __version__, "viewers": len(self.viewers)})
        if self.identify_enabled:
            s["identify"] = {"busy": self.identify.busy, "continuous": self.identify.continuous,
                             "model": self.identify.identifier.model, "last": self.last_result}
        return s

    # ---- Viewers ------------------------------------------------------------------------
    async def _viewer_joined(self) -> None:
        if self._stop_task:
            self._stop_task.cancel()
            self._stop_task = None
        await self.loupe.start()

    def _viewer_left(self) -> None:
        if not self.viewers:
            self._stop_task = asyncio.create_task(self._stop_later())

    async def _stop_later(self) -> None:
        await asyncio.sleep(self.idle_s)
        if not self.viewers:
            await self.loupe.stop()

    def _command(self, cmd: dict) -> None:
        if cmd.get("lang") in ("en", "de"):
            self.identify.lang = cmd["lang"]
        kind = cmd.get("t")
        if kind == "light":
            self.loupe.set_light(cmd.get("v", 70))
        elif kind == "focus":
            self.identify.focus = (min(1.0, max(0.0, float(cmd.get("x", 0.5)))),
                                   min(1.0, max(0.0, float(cmd.get("y", 0.5)))),
                                   min(0.5, max(0.02, float(cmd.get("r", 0.15)))))
        elif kind == "identify":
            self.identify.trigger()
        elif kind == "continuous":
            self.identify.continuous = bool(cmd.get("on"))

    async def ws(self, request: web.Request) -> web.WebSocketResponse:
        ws = web.WebSocketResponse(heartbeat=20, max_msg_size=64 * 1024)
        await ws.prepare(request)
        queue: asyncio.Queue = asyncio.Queue(maxsize=1)
        self.viewers[ws] = queue
        await self._viewer_joined()
        sender = asyncio.create_task(self._sender(ws, queue))
        try:
            await ws.send_str(json.dumps(self.state(), ensure_ascii=False))
            async for msg in ws:
                if msg.type != WSMsgType.TEXT:
                    continue
                try:
                    cmd = json.loads(msg.data)
                    if isinstance(cmd, dict):
                        self._command(cmd)
                except (ValueError, TypeError):
                    continue
        finally:
            sender.cancel()
            self.viewers.pop(ws, None)
            self._viewer_left()
        return ws

    async def _sender(self, ws: web.WebSocketResponse, queue: asyncio.Queue) -> None:
        last_state = 0.0
        while not ws.closed:
            try:
                header, jpeg = await asyncio.wait_for(queue.get(), timeout=1.0)
                await ws.send_str(header)
                await ws.send_bytes(jpeg)
            except asyncio.TimeoutError:
                pass
            except (ConnectionResetError, RuntimeError):
                return
            if time.monotonic() - last_state >= 1:
                last_state = time.monotonic()
                try:
                    await ws.send_str(json.dumps(self.state(), ensure_ascii=False))
                except (ConnectionResetError, RuntimeError):
                    return

    # ---- HTTP ---------------------------------------------------------------------------
    async def index(self, request: web.Request) -> web.FileResponse:
        return web.FileResponse(self.page, headers={"Cache-Control": "no-cache"})

    async def api_state(self, request: web.Request) -> web.Response:
        s = self.state()
        return web.json_response(s, dumps=lambda o: json.dumps(o, ensure_ascii=False))

    async def healthz(self, request: web.Request) -> web.Response:
        return web.json_response({"ok": True, "version": __version__, "home_side": self.loupe.target_ip,
                                  "live": self.loupe.state()["stage"] == "live"})

    async def on_startup(self, app: web.Application) -> None:
        self.loop = asyncio.get_running_loop()
        if self.identify_enabled:
            self.identify.start()
        await self.loupe.listen()


def make_app() -> web.Application:
    server = Server()
    app = web.Application()
    app.router.add_get("/", server.index)
    app.router.add_get("/ws", server.ws)
    app.router.add_get("/api/state", server.api_state)
    app.router.add_get("/healthz", server.healthz)
    app.on_startup.append(server.on_startup)
    return app


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    port = int(os.environ.get("PL_PORT", "8080"))
    log.info("Platinenlupe server %s on port %d", __version__, port)
    web.run_app(make_app(), port=port, access_log=None, print=None)
