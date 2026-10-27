"""Identifies the part inside the focus ring with a local vision-language model.

Why not an object detector: the loupe shows two or three parts at high magnification. There is no open dataset
for that view, and detectors trained on whole boards were plainly wrong on it. A vision-language model answers
the narrower question "what is THIS part" and reads markings along the way. Measurements: docs/identification.md.

Three kinds of model server, detected automatically from PL_VLM_URL:
  LM Studio  /api/v0/models lists the loaded models — the currently loaded vision model is used, nothing is
             ever loaded on purpose (PL_VLM_MODEL pins one instead).
  Ollama     /api/tags answers — PL_VLM_MODEL names the model (default gemma3:12b).
  OpenAI-compatible (llama.cpp server, vLLM, LocalAI, …) — /v1/chat/completions with PL_VLM_MODEL.
"""

from __future__ import annotations

import base64
import io
import json
import os
import threading
import time
import urllib.request

from PIL import Image

PARTS = ["resistor", "resistor_array", "ceramic_capacitor", "electrolytic_capacitor", "tantalum_capacitor", "ic",
         "transistor", "mosfet", "diode", "zener_diode", "led", "inductor", "ferrite", "crystal", "connector",
         "pin_header", "fuse", "button", "solder_joint", "trace", "via", "none", "unclear"]

SCHEMA = {
    "type": "object",
    "properties": {
        "part": {"type": "string", "enum": PARTS},
        "package": {"type": "string"},
        "marking": {"type": "string"},
        "value": {"type": "string"},
        "confidence": {"type": "string", "enum": ["high", "medium", "low"]},
        "note": {"type": "string"},
    },
    "required": ["part", "package", "marking", "value", "confidence", "note"],
}

LANGUAGES = {"en": "English", "de": "German"}

PROMPT = """You see a macro photo of an electronics circuit board taken through a loupe camera (ring light). \
Identify the component in the CENTRE of the image. Answer only with JSON following the schema.

- part: exactly one key from the list. If nothing is recognisable: "none" or "unclear".
- package: as precise as you can tell for sure, e.g. "SMD 0603", "SMD 0805", "SOT-23", "SOD-123", "SOIC-8", \
"QFN", "THT axial", "THT radial", "TO-92". If unsure, leave it empty.
- marking: only characters you can really read, otherwise empty. List colour bands as colours.
- value: only if it follows from marking or colour bands, e.g. "103" → "10 kΩ", "4R7" → "4.7 Ω", \
"104" on a capacitor → "100 nF". Otherwise empty.
- confidence: be honest. Never "high" for a guess.
- note: one short sentence in {language}: what gives it away (shape, colour, terminals).
White print on the board (silkscreen, reference designators) is NOT a component.
Do not make anything up. Black rectangular SMD parts with digits are usually resistors; \
brown/beige ones without marking are usually ceramic capacitors."""

# Models do not always stick to the list ("integrated circuit", "SMD resistor") — map to the fixed key so that
# colour and translation work. Order matters: specific before general.
_MAP = [("array", "resistor_array"), ("network", "resistor_array"), ("resist", "resistor"),
        ("electrolytic", "electrolytic_capacitor"), ("tantal", "tantalum_capacitor"), ("capacit", "ceramic_capacitor"),
        ("mosfet", "mosfet"), ("transistor", "transistor"), ("solder", "solder_joint"), ("zener", "zener_diode"), ("led", "led"),
        ("light emitting", "led"), ("diode", "diode"), ("integrated", "ic"), ("chip", "ic"), ("ic", "ic"),
        ("induct", "inductor"), ("coil", "inductor"), ("ferrite", "ferrite"), ("crystal", "crystal"),
        ("oscillator", "crystal"), ("header", "pin_header"), ("connector", "connector"), ("socket", "connector"),
        ("fuse", "fuse"), ("button", "button"), ("switch", "button"),
        ("trace", "trace"), ("via", "via"), ("no component", "none"), ("nothing", "none")]


def normalize(name: str) -> str:
    key = (name or "").strip().lower().replace(" ", "_")
    if key in PARTS:
        return key
    low = (name or "").lower()
    return next((target for part, target in _MAP if part in low), "unclear")


def crop(jpeg: bytes, fx: float, fy: float, fr: float, edge: int = 448) -> bytes | None:
    """Square around the focus ring (centre fx/fy, radius fr — all relative to the image width) as JPEG."""
    try:
        img = Image.open(io.BytesIO(jpeg)).convert("RGB")
    except Exception:
        return None
    w, h = img.size
    r = max(16, int(fr * w * 1.15))                   # a little margin so the whole part is inside
    cx, cy = int(fx * w), int(fy * h)
    box = (max(0, cx - r), max(0, cy - r), min(w, cx + r), min(h, cy + r))
    if box[2] - box[0] < 4 or box[3] - box[1] < 4:
        return None
    out = io.BytesIO()
    img.crop(box).resize((edge, edge), Image.Resampling.BICUBIC).save(out, "JPEG", quality=92)
    return out.getvalue()


class Identifier:
    def __init__(self) -> None:
        self.url = os.environ.get("PL_VLM_URL", "http://localhost:1234").rstrip("/")
        self.pinned = os.environ.get("PL_VLM_MODEL", "")
        self.api_key = os.environ.get("PL_VLM_API_KEY", "")
        self.model = self.pinned or "loaded model"     # shown on the page
        self.kind: str | None = None                    # lmstudio | ollama | openai

    def _call(self, path: str, data: dict | None = None, timeout: float = 10) -> dict:
        headers = {"Content-Type": "application/json"}
        if self.api_key:
            headers["Authorization"] = "Bearer " + self.api_key
        req = urllib.request.Request(self.url + path, json.dumps(data).encode() if data else None, headers)
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())

    def _detect(self) -> str:
        for kind, path in (("lmstudio", "/api/v0/models"), ("ollama", "/api/tags")):
            try:
                self._call(path)
                return kind
            except Exception:
                pass
        return "openai"

    def _lmstudio_model(self) -> str:
        if self.pinned:
            return self.pinned
        models = self._call("/api/v0/models").get("data", [])
        loaded = [m for m in models if m.get("state") == "loaded"]
        vision = [m["id"] for m in loaded if m.get("type") == "vlm"]
        if not vision:
            names = ", ".join(m["id"] for m in loaded) or "none"
            raise RuntimeError(f"No vision model loaded in LM Studio (loaded: {names})")
        return vision[0]

    def identify(self, image_jpeg: bytes, lang: str = "en") -> dict:
        b64 = base64.b64encode(image_jpeg).decode()
        prompt = PROMPT.replace("{language}", LANGUAGES.get(lang, "English"))
        if self.kind is None:
            self.kind = self._detect()
        if self.kind == "ollama":
            self.model = self.pinned or "gemma3:12b"
            # think=False: thinking models otherwise spend the token budget on reasoning and return nothing.
            answer = self._call("/api/chat", {
                "model": self.model, "stream": False, "format": SCHEMA, "think": False, "keep_alive": "30m",
                "options": {"temperature": 0, "num_predict": 300},
                "messages": [{"role": "user", "content": prompt, "images": [b64]}],
            }, timeout=180)
            text = answer["message"]["content"]
        else:
            self.model = self._lmstudio_model() if self.kind == "lmstudio" else (self.pinned or "default")
            request = {
                "model": self.model, "temperature": 0, "max_tokens": 400,
                "response_format": {"type": "json_schema", "json_schema": {"name": "part", "schema": SCHEMA}},
                "messages": [{"role": "user", "content": [
                    {"type": "text", "text": prompt},
                    {"type": "image_url", "image_url": {"url": "data:image/jpeg;base64," + b64}}]}],
            }
            if self.kind == "lmstudio":
                request["reasoning_effort"] = "none"     # no thinking: thinking models get slow or empty
            text = self._call("/v1/chat/completions", request, timeout=180)["choices"][0]["message"]["content"]
        result = json.loads(text[text.find("{"):text.rfind("}") + 1])
        result["part"] = normalize(result.get("part", ""))
        return result


class IdentifyLoop(threading.Thread):
    """Identifies the newest frame at most every `interval_s` seconds — never builds a queue."""

    def __init__(self, done) -> None:
        super().__init__(daemon=True, name="identify")
        self.done = done                     # done(result: dict) — called from this thread
        self.identifier = Identifier()
        self.interval_s = float(os.environ.get("PL_IDENTIFY_INTERVAL_S", "1"))
        self.busy = False
        self.continuous = os.environ.get("PL_IDENTIFY_CONTINUOUS", "1") != "0"
        self.focus = (0.5, 0.5, 0.15)        # centre x, centre y (relative), radius relative to the width
        self.lang = "en"
        self._new = threading.Condition()
        self._frame: bytes | None = None
        self._now = False

    def submit(self, jpeg: bytes) -> None:
        with self._new:
            self._frame = jpeg
            self._new.notify()

    def trigger(self) -> None:
        with self._new:
            self._now = True
            self._new.notify()

    def run(self) -> None:
        last = 0.0
        while True:
            with self._new:
                while not (self._frame and (self._now or (self.continuous and time.monotonic() - last >= self.interval_s))):
                    self._new.wait(timeout=0.5)
                jpeg, self._frame, self._now = self._frame, None, False
            last = time.monotonic()
            part = crop(jpeg, *self.focus)
            if part is None:
                continue
            t = time.perf_counter()
            self.busy = True
            try:
                result = self.identifier.identify(part, self.lang)
                result["error"] = None
            except Exception as e:
                result = {"error": f"{type(e).__name__}: {e}"}
            self.busy = False
            result.update({"ms": round((time.perf_counter() - t) * 1000), "time": time.time(),
                           "model": self.identifier.model, "focus": self.focus})
            self.done(result)
