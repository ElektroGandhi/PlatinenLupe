"""Test viewer: connects like the phone over the WebSocket and measures what arrives.

  python tools/viewer.py ws://127.0.0.1:8080/ws [--seconds 10] [--save last.jpg] [--identify]

Checks every frame for a JPEG start and end marker. Exits with 1 if not a single frame arrived,
so it also works in scripts and CI.
"""

import argparse
import asyncio
import json
import sys
import time

import aiohttp


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("url")
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--save")
    ap.add_argument("--identify", action="store_true", help="ask for one identification and print it")
    a = ap.parse_args()
    frames = broken = 0
    state, header, result, last = {}, None, None, None
    end = time.monotonic() + a.seconds
    async with aiohttp.ClientSession() as s, s.ws_connect(a.url) as ws:
        if a.identify:
            await ws.send_str(json.dumps({"t": "continuous", "on": False}))
            await ws.send_str(json.dumps({"t": "identify"}))
        while time.monotonic() < end:
            try:
                msg = await asyncio.wait_for(ws.receive(), timeout=max(0.1, end - time.monotonic()))
            except asyncio.TimeoutError:
                break
            if msg.type == aiohttp.WSMsgType.TEXT:
                m = json.loads(msg.data)
                if m["t"] == "state":
                    state = m
                elif m["t"] == "frame":
                    header = m
                elif m["t"] == "result":
                    result = m
            elif msg.type == aiohttp.WSMsgType.BINARY and header:
                ok = msg.data[:2] == b"\xff\xd8" and msg.data[-2:] == b"\xff\xd9"
                frames += ok
                broken += not ok
                last, header = msg.data, None
            elif msg.type in (aiohttp.WSMsgType.CLOSED, aiohttp.WSMsgType.ERROR):
                break
    print(f"frames {frames} ({frames / a.seconds:.1f}/s), broken {broken}")
    print(f"state: {state.get('stage')} / {state.get('code')}, home side {state.get('ip')}, dropped {state.get('dropped')}")
    if result:
        print("result:", json.dumps({k: v for k, v in result.items() if k != "t"}, ensure_ascii=False))
    if a.save and last:
        open(a.save, "wb").write(last)
    return 0 if frames else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
