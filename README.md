# پارسی‌گو — pocket-tts-farsi-v2, native C++ (Windows 7)

A fully native (C++) port of the Persian text-to-speech + voice-cloning pipeline
from [`nimaone/persian_tts`](https://github.com/nimaone/persian_tts), running the
ONNX package [`Nimaone/pocket-tts-farsi-v2-onnx`](https://huggingface.co/Nimaone/pocket-tts-farsi-v2-onnx).

**No Python at runtime.** Persian text → phonemes → speech, entirely offline on
CPU, with a CLI, a local HTTP server (+ Persian web UI) and a native Win32 GUI.
Targets **Windows 7 SP1 x64**; also builds and runs on Linux/macOS.

> Like llama.cpp, this project links one inference core (ONNX Runtime) and
> implements every model-specific host step in C++: the ByT5 greedy G2P, the
> FlowLM KV-cache stepping, the Mimi decoder's 26 state tensors, chunking,
> voice cloning and audio post-processing. sherpa-onnx cannot run this
> architecture — see [`docs/win7.md`](docs/win7.md#1-why-not-sherpa-onnx).

## Features

- **Persian G2P** — ByT5 byte tokenizer + ONNX T5 encoder/decoder, host-side
  greedy decode, Latin transliteration, number spell-out (ports the reference
  exactly; phonemes match character-for-character).
- **Pronunciation dictionary** — `dicts/user.dict` (`word⇥phonemes`) overrides
  the G2P per word (fix a name or a term like `بُردارسازی`), editable from the
  CLI (`--dict-set/--dict-list`), both UIs and `POST /api/dict`; lookup ignores
  case, punctuation, ZWNJ and diacritics.
- **Phonemes in the open** — `--phonemes` / `POST /api/phonemes` show what will
  be spoken, `--phonemes-in` / `?input=phonemes` / the GUI's phoneme tab speak a
  hand-edited string (G2P skipped), `--chunks` shows the model-sized plan.
- **All parameters everywhere** — mode, pace, seed, temperature, retries,
  chunk-tokens, pause-scale, ezafe, zero-noise: one name each in the CLI, the
  HTTP API and both UIs (`--help` and `GET /api/help` document them all).
- **One-time reference voice** — `--preload-voice`, `--bulk`, `POST
  /api/voice/load` and the GUI button encode the prompt once for a whole batch.
- **Voice cloning** — any ~5 s reference WAV is encoded with the Mimi encoder
  and used to condition the flow LM. Built-in voices + drag-your-own; the GUI
  loads a voice **once** and reuses it, offloading it when you switch.
- **Chunking** — punctuation-aware sentence/phrase planning and token-budget
  packing (`split` and `pack` modes), ported from the reference.
- **Quality gates** — dead-air trimming, pause compression, loudness-matched
  stitching and a configurable 1–4 attempt retry, ported from the reference.
- **Three front-ends** — CLI (`--list-voices`, `--play`, …), HTTP server +
  RTL web UI (HTTP/1.1 keep-alive, unload beacon) and a Win32 GUI with **5
  tabs**: text · phonemes · dictionary · settings · **model download**.
- **Persian / English UI** — the GUI switches language live (فارسی ⇄ English):
  Persian text in the **Sahel** font, English in Segoe UI; the input box is RTL
  and supports Ctrl+A.
- **Model download dialog** — the GUI reads `models.json` (17 files with size
  and description) and fetches them with WinInet (progress bar, `.part` temp
  file + rename), so a fresh install gets the ~470 MB weights without a
  browser.
- **Windows 7** — ONNX Runtime 1.14 (the last Win7 release) + IR-stamp lowering;
  optional static CRT so no redistributable is needed. The server picks a free
  port automatically when the preferred one is busy.
- **Fast** — ~3–5× faster than the reference Python engine on the same CPU
  (varies with the retry count, as in the reference).

## Screenshots

**Web UI (Persian RTL)**

![web — text & synthesis](screenshots/01-web-tab-sazesh.png)

![web — API help](screenshots/11-web-help.png)

**Desktop GUI — فارسی / English**

![GUI — فارسی](screenshots/12-gui-fa-main.png)

![GUI — English](screenshots/17-gui-en-main.png)

<details>
<summary>All 18 screenshots (web 01–11, GUI 12–18)</summary>

| # | Web UI | # | Desktop GUI |
|---|--------|---|--------------|
| 01 | ![01](screenshots/01-web-tab-sazesh.png) | 12 | ![12](screenshots/12-gui-fa-main.png) |
| 02 | ![02](screenshots/02-web-preload-voice.png) | 13 | ![13](screenshots/13-gui-fa-phonemes.png) |
| 03 | ![03](screenshots/03-web-after-synth.png) | 14 | ![14](screenshots/14-gui-fa-settings.png) |
| 04 | ![04](screenshots/04-web-tab-vanaye.png) | 15 | ![15](screenshots/15-gui-fa-download.png) |
| 05 | ![05](screenshots/05-web-tab-vajehname.png) | 16 | ![16](screenshots/16-gui-fa-dictionary.png) |
| 06 | ![06](screenshots/06-web-tab-tanzimat.png) | 17 | ![17](screenshots/17-gui-en-main.png) |
| 07 | ![07](screenshots/07-web-tab-tolid-anboh.png) | 18 | ![18](screenshots/18-gui-en-settings.png) |
| 08 | ![08](screenshots/08-web-quiet-reference.png) |  |  |
| 09 | ![09](screenshots/09-web-quiet-reference-synth.png) |  |  |
| 10 | ![10](screenshots/10-web-reject-nonwav.png) |  |  |

</details>

## Layout

```
src/
  main_cli.cpp        CLI (--help is the full option reference)
  server_main.cpp     HTTP API + web UI server (--help = API reference)
  gui_win32.cpp       native Win32 GUI (5 tabs, fa/en, Sahel font)
  tts.{h,cpp}         engine: FlowLM + Mimi + voice cache + quality gates
  dict.{h,cpp}        pronunciation dictionary (word -> phonemes)
  g2p.{h,cpp}         ByT5 G2P + Latin transliteration
  chunk.{h,cpp}       sentence/phrase planning + phoneme chunking
  normalize_fa.{h,cpp} Persian normalization (port of normalize_fa.py)
  spm.{h,cpp}         SentencePiece wrapper
  ort_util.{h,cpp}    ONNX Runtime C-API wrapper (version-portable)
  npz.{h,cpp}         NumPy .npz/.npy reader (no deps)
  audio.{h,cpp}       WAV I/O + polyphase resampler + voice trimming
  rng.{h,cpp}         deterministic RNG
  sahel_font.h        generated: the Sahel TTF embedded for the GUI
  win7shim/           api-set shim that lets ORT 1.14 run on stock Win7
web/index.html        Persian RTL web UI (offline, single file)
dicts/user.dict       pronunciation dictionary (created on first save)
voices/               built-in reference voices
model/onnx, model/v2  the ONNX package + phone tokenizer (weights: releases/HF)
scripts/              build + verification + Windows 7 tooling
docs/win7.md          Windows 7 build / deployment / compatibility guide
screenshots/          UI screenshots (embedded above)
ref/                  the reference Python engine (for verification only)
howto.md              how to publish this tree on GitHub (repo, releases, shots)
CHANGELOG.md          what changed since the last published package
```

## Build

Windows (VS2019, see [`docs/win7.md`](docs/win7.md)):

```bat
set ORT_DIR=C:\deps\onnxruntime-win-x64-1.14.1
set SPM_DIR=C:\deps\sentencepiece
scripts\build_win.bat
```

Windows (MinGW64 / MSYS2 — produces all three exes in `build-win/`):

```bash
./scripts/build_win_mingw.sh
```

Linux (verification host):

```bash
scripts/build_linux.sh
```

## Run

```bash
# CLI — Persian text
build/farsi-tts "سلام، حال شما چطور است؟" --voice voices/male_hello.wav --out out.wav

build/farsi-tts --help               # every option, explained
build/farsi-tts --list-voices        # reference voices on disk, one per line
build/farsi-tts --phonemes "بردارسازی"      # see what will be spoken
build/farsi-tts --text-in --phonemes "123"  # force the G2P (numeric input)
build/farsi-tts --dict-set "بردارسازی=bordArAzi"   # fix it for good
build/farsi-tts --bulk lines.txt --out-dir output/bulk   # one load, N files
build/farsi-tts "…" --out a.wav --play      # write + play (Windows)

# server + web UI (moves to the next free port if 8000 is taken)
build/farsi-tts-server --base . --port 8000     # -> http://127.0.0.1:8000
curl -s localhost:8000/api/help                 # full API reference

# GUI (Windows)
farsi-tts-gui.exe   # text | phonemes | dictionary | settings | model download
                    # fa/en switcher, RTL input + Ctrl+A, Sahel font
```

## Downloads

Prebuilt packages are published on the **[Releases page](../../releases)**
(see [`howto.md`](howto.md) for how they are built and uploaded):

| Asset | Contents |
|-------|----------|
| `parsi-go-win64-v1.1.0.zip` | the three executables + runtime DLLs, web UI, docs, voices, `models.json` — **no weights** |
| `parsi-go-model-onnx-v1.1.0.zip` | the full `model/` package (~470 MB) — unpack it next to the executables |

Unpack both into the same folder and start `farsi-tts-gui.exe`.

## Model files

`model/onnx` must contain the graphs from the HuggingFace package — either
unpack the model release above, or fetch them yourself:

```bash
huggingface-cli download Nimaone/pocket-tts-farsi-v2-onnx --local-dir model/onnx
python3 scripts/prepare_win7_onnx.py model/onnx   # for ONNX Runtime <= 1.17 / Win7
```

`model/v2/tokenizer_ph.model` is the SentencePiece phone tokenizer (kept in
this repository). The GUI's **model download** tab does the same job from
`models.json` — point every `url` at your own release asset if you prefer to
host the files on GitHub (see [`howto.md`](howto.md#43-optional-host-the-model-files-on-your-own-release)).

## Verification

The port is validated stage-by-stage against `ref/` — G2P and chunking match
exactly, the FlowLM produces bit-identical latents given identical conditioning,
and the Mimi decoder matches to 16-bit precision. See
[`docs/win7.md`](docs/win7.md#7-verification) and `scripts/verify_*.py`.

Gates (all green at release time):

```bash
python3 scripts/verify_engine.py     # 11/11 — byte-exact vs the Python reference
python3 scripts/test_web_ui.py       # 25/25 — web UI/API (writes screenshots/)
python3 scripts/test_protocol.py     # 12/12 — keep-alive, close, stress
```

## License

**Non-commercial use only.** The TTS model is **CC-BY-NC-4.0** and this port is
a derivative of it; the Persian G2P component is MIT; the base `pocket-tts`
architecture is MIT (Kyutai). All model credit belongs to the original authors.
See the model card for attribution.
