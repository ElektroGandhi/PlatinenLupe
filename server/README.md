# Server

Optional. Adds **part identification** to the loupe and serves the page from a machine of your choice — a Raspberry
Pi, a NAS, a home server. The firmware works without it ([three modes](../docs/modes.md)).

```
board (server mode) ── UDP 61500/61501 ──► server ── WebSocket ──► browser
        └─ broadcast UDP 61499 ("here I am") ─┘   └── local vision model (LM Studio / Ollama / OpenAI-compatible)
```

## Run with Docker

```bash
git clone <this repo> && cd Platinenlupe
# set PL_VLM_URL (and PL_VLM_MODEL for Ollama) in docker-compose.yml
docker compose up -d
```

Open `http://<host>:8080`. Put the board into **Server** mode on its own page (*Device settings*); the server finds
it within two seconds.

## Run without Docker

```bash
cd server
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
PL_VLM_URL=http://localhost:11434 PL_VLM_MODEL=gemma3:12b python -m platinenlupe
```

Python 3.10+, two dependencies: `aiohttp` and `Pillow`.

## Settings

| Variable | Default | |
|---|---|---|
| `PL_PORT` | `8080` | HTTP port |
| `PL_VLM_URL` | `http://localhost:1234` | model server — LM Studio, Ollama or OpenAI-compatible (detected automatically) |
| `PL_VLM_MODEL` | *(loaded model)* | model name; LM Studio picks the loaded vision model when empty |
| `PL_VLM_API_KEY` | | bearer token for OpenAI-compatible servers |
| `PL_DEVICE_IP` | | fixed address of the board (or of the loupe itself) instead of discovery |
| `PL_IDENTIFY` | `1` | `0` = viewer only |
| `PL_IDENTIFY_CONTINUOUS` | `1` | identify all the time, not only on tap |
| `PL_IDENTIFY_INTERVAL_S` | `1` | minimum seconds between identifications |
| `PL_IDLE_S` | `30` | keep streaming this long after the last viewer left, then stop the camera (and its light) |
| `PL_LIGHT` | `70` | light ring brightness at start, % |
| `PL_WEB_DIR` | *(auto)* | folder that holds `index.html` |

## Endpoints

| Path | |
|---|---|
| `/` | the page |
| `/ws` | WebSocket: frames, state, results (format in `platinenlupe/app.py`) |
| `/api/state` | JSON state — the page uses it to tell a server from a board |
| `/healthz` | `{"ok": true, "home_side": "…", "live": …}` |

## Without hardware

```bash
python tools/simulator.py some.jpg other.jpg --fps 10 --loss 0.03   # plays board + loupe, drops 3 % of packets
python -m platinenlupe                                              # second terminal
python tools/viewer.py ws://127.0.0.1:8080/ws --seconds 10          # plays the phone, counts frames
```

The viewer exits with 1 if no frame arrived. With 3 % packet loss it must report `broken 0` — half frames are dropped,
never shown.

## Tests

```bash
pip install -r requirements-dev.txt && pytest -q
```
