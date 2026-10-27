# Part identification

In **server mode** you can tap the image to place a focus ring on a part. The server cuts out that square, sends it to
a vision-language model running **on your own machine**, and shows the answer:

> **Resistor** · SMD 0805 · marking `103` · value 10 kΩ · *high confidence*
> *Black rectangular body with three digits.*

## Why a vision-language model and not an object detector

The loupe shows two or three parts, hugely magnified. Object detectors trained on PCB datasets learned whole boards
from a distance; on loupe close-ups they were plainly wrong. Open-vocabulary detectors (prompted with "resistor",
"capacitor", …) found next to nothing. A vision-language model answers the narrower question *"what is the part in
the middle of this picture?"* — and reads markings on the way.

## Supported model servers

The server detects the kind from `PL_VLM_URL`:

| Server | `PL_VLM_URL` | Model |
|---|---|---|
| [LM Studio](https://lmstudio.ai) | `http://host:1234` | the vision model that is **currently loaded** — nothing is ever loaded on purpose. `PL_VLM_MODEL` pins one. |
| [Ollama](https://ollama.com) | `http://host:11434` | `PL_VLM_MODEL`, default `gemma3:12b` |
| llama.cpp server, vLLM, LocalAI, … (OpenAI-compatible) | `http://host:port` | `PL_VLM_MODEL`; `PL_VLM_API_KEY` if the server wants one |

Thinking is switched off (`reasoning_effort: "none"` / `think: false`): thinking models otherwise spend their token
budget on reasoning and return an empty answer.

The model must answer in a fixed JSON schema: `part` from a fixed list of 23 keys (resistor, ceramic capacitor, IC,
SOT-23 transistor, LED, via, solder joint, "none", "unclear", …), `package`, `marking`, `value`, `confidence`, and a
one-sentence `note` in the language of your browser.

## How good is it?

Measured on a real loupe image of an INA219 breakout board, five focus points (SMD resistors, a SOIC-8, ceramic
capacitors, silkscreen):

| Model | Correct part type | Time per answer | Hardware |
|---|---|---|---|
| Gemma 4 26B (Ollama) | 5 / 5 | ~5 s | desktop GPU |
| a Qwen 3.6 35B-A3B fine-tune (LM Studio) | 4 / 5 — package SOIC-8 right; one SMD resistor called "ceramic capacitor" | 1–1.6 s | Apple-Silicon Mac |
| Gemma 4 E4B (Ollama) | 2 / 5 | 1.4 s | desktop GPU |
| Gemma 3 12B (Ollama, ROCm) | unusable — five times the identical answer | ~50 s | AMD GPU (ROCm) |

⚠️ **No model reads markings reliably.** `01E` came back as `103`. The page says so next to every value — treat
markings and values as a hint, not a measurement.

Bigger models are better at telling resistors from capacitors; the package (0603/0805, SOT-23, SOIC-8) is usually
right. Silkscreen text is sometimes taken for a part — the prompt explicitly tells the model it is not.

## Tuning

| Variable | Default | |
|---|---|---|
| `PL_IDENTIFY` | `1` | `0` switches identification off entirely |
| `PL_IDENTIFY_CONTINUOUS` | `1` | identify continuously (otherwise only on tap / "Identify now") |
| `PL_IDENTIFY_INTERVAL_S` | `1` | minimum seconds between two identifications |

The identification never builds a queue: it always takes the newest frame, so a slow model just answers less often.
