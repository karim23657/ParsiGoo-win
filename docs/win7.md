# پارسی‌گو روی ویندوز ۷ — Windows 7 build

A native Windows 7 port of [`nimaone/persian_tts`](https://github.com/nimaone/persian_tts)
running the [`Nimaone/pocket-tts-farsi-v2-onnx`](https://huggingface.co/Nimaone/pocket-tts-farsi-v2-onnx)
package. The whole pipeline — Persian G2P, chunking, FlowLM, Mimi codec — runs
in **C++** with **no Python at runtime**. It ships a CLI, a local HTTP server
with a Persian web UI, and a native Win32 GUI; all can clone any reference
voice from a 5-second WAV.

---

## 1. Why not sherpa-onnx?

The instruction was "use a ready-made exe (sherpa-onnx) if one exists, otherwise
implement it natively." sherpa-onnx **cannot** run this model: its TTS runtimes
are VITS / Kokoro / Matcha / Piper-style acoustic models with a text front-end
it controls. `pocket-tts-farsi-v2` is a different architecture:

| stage | sherpa-onnx | this model |
|---|---|---|
| text → phonemes | built-in lexicon/espeak | a **ByT5 T5 encoder/decoder** (`g2p_*.onnx`) |
| acoustic model | VITS-family | a **6-layer flow LM** with a custom KV cache (`flow_lm_step.onnx`) |
| vocoder | HiFiGAN | **Mimi** streaming decoder with 26 state tensors (`mimi_decoder_step_kv.onnx`) |
| voice cloning | limited | **speaker conditioning from a Mimi-encoded prompt** |

So this project implements the whole pipeline natively. Like llama.cpp, it
links one inference core (ONNX Runtime, the same engine sherpa-onnx itself
bundles) and implements all of the model-specific host logic in C++.

---

## 2. Windows 7 constraint: ONNX Runtime version

This is the single thing that makes Win7 harder than Win10.

| ONNX Runtime | max IR | max opset | Windows 7 |
|---|---|---|---|
| **1.14** (Nov 2022) | **8** | **18** | **last release that runs on Win7** |
| 1.15 – 1.17 | 9 | 19–20 | ✗ (dropped) |
| ≥ 1.18 | 10 | 21 | ✗ |

The published package is stamped **IR 10 / opset 18**. IR 10 support only
arrived in ORT 1.18 — which no longer supports Win7 — so the graphs must have
their IR stamp lowered before Win7 can load them:

```bat
python scripts\prepare_win7_onnx.py model\onnx
```

This rewrites only `ir_version` (10 → 8); the opsets stay at 18, which ORT 1.14
supports. The graphs use **no IR-10-specific features**, and the engine was
verified to produce identical output with the lowered graphs (see §7). Opset
downgrading is *not* possible — the graphs use `LayerNormalization` (opset 17+)
and the current `Split`/`Pad`/`Shape` semantics, and `onnx.version_converter`
rejects the conversion.

### Other Win7 requirements

- **Visual C++ 2015–2022 Redistributable** (x64) — required by ONNX Runtime.
  Install `vc_redist.x64.exe` (the 2015–2022 redist supports Win7 SP1).
- **Universal C Runtime (UCRT)** — Win7 SP1 needs **KB2999226** (installed by
  Windows Update, or bundled by `vc_redist`).
- **Common Controls v6** — the GUI uses `comctl32`; the manifest in
  `cmake/win7.manifest` opts into v6 (Win7 already ships it).
- **Windows 7 SP1** is required (the CRT and TLS 1.2s dependencies assume it).

If the prebuilt ORT 1.14 DLL still refuses to load on your Win7 image, rebuild
ORT from source with the v142 toolset and the Windows 7.1 SDK:

```bat
git clone --recursive -b v1.14.1 https://github.com/microsoft/onnxruntime
cd onnxruntime
build.bat --config Release --build_shared_lib --parallel ^
          --cmake_generator "Visual Studio 16 2019" ^
          --cmake_extra_defines CMAKE_SYSTEM_VERSION=6.1
```

`CMAKE_SYSTEM_VERSION=6.1` is the Win7 target; ORT issue
[#25910](https://github.com/microsoft/onnxruntime/issues/25910) documents this
path. Point `ORT_DIR` at the resulting `onnxruntime-win-x64-1.14.1` folder.

### The `0xc0000005` startup crash (reproduced on real Windows 7, fixed)

A real Windows 7 SP1 x64 machine (`7601.26321.amd64fre.win7sp1_ldr_escrow.230105-1520`,
x64, elevated) reported for the shipped exes:

```
The application was unable to start correctly (0xc0000005). Click OK to close the application.
```

**Root cause:** the official `onnxruntime.dll` 1.14.1 is not Windows-7-clean.
It statically imports **33 `api-ms-win-*` API-set contracts that Windows 7 does
not have**, among them:

| import DLL | function | available since |
|---|---|---|
| `api-ms-win-core-sysinfo-l1-2-0` | `GetSystemTimePreciseAsFileTime` | Windows 8 |
| `api-ms-win-core-file-l1-2-0` | `CreateFile2` | Windows 8 |
| `api-ms-win-core-path-l1-1-0` | `PathCchRemoveBackslash` / `PathCchRemoveFileSpec` | Windows 8.1 / KB2999226 |

and all three exes imported `onnxruntime.dll` **statically**, so the loader
resolved those contracts *while creating the process* — before `main()` could
run — and the process died with `0xc0000005` (`0xc0000135` when a contract is
missing outright). Wine does not model Windows 7's API-set schema, which is why
the earlier Wine smoke tests passed. Microsoft support confirms the same
conclusion for this ORT build: the prebuilt 1.14.1 `onnxruntime.dll` does not
support Windows 7 ([SO 76324358](https://stackoverflow.com/questions/76324358/failed-to-use-onnxruntime-on-windows-7)).

**The fix — three parts, all in this package:**

1. **`scripts/patch_ort_win7.py` rewrites every `api-ms-win-*` import
descriptor of `onnxruntime.dll` to a local `w7shim.dll`.** The name string of
one existing descriptor is overwritten in place, so the DLL keeps its exact
byte size (9 271 704) and every other import (MSVCP140/VCRUNTIME140) is
untouched. The script refuses to run if anything else would change.
2. **`w7shim.dll`** (generated by the same script, implemented in
`src/win7shim/win7shim.c`) re-exports all **206** affected functions as
trampolines that are resolved *lazily on first use* — first from the DLLs
Windows 7 really has (`kernel32`, `kernelbase`, `ntdll`, `advapi32`,
`ucrtbase`, `msvcrt`), then from four own implementations of the
Windows-8-only functions (`GetSystemTimePreciseAsFileTime`, `CreateFile2`,
`PathCchRemoveBackslash`, `PathCchRemoveFileSpec`). Because nothing is
resolved inside `DllMain`, a symbol missing on some machine can no longer fail
process startup — it degrades to a logged stub instead. Set `W7SHIM_LOG=1` to
get a `w7shim.log` line per import (`ok` / `ours` / `MISSING`).
3. **The exes no longer link `onnxruntime.dll`** (the `-lonnxruntime` flag was
removed). `ort_util.cpp` calls `LoadLibraryW("onnxruntime.dll")` on first use
and prints `farsi-tts: loading onnxruntime.dll ...` to stderr *before* the
call, so a problem inside the DLL shows up as a readable error (with the
Windows error code and a hint) instead of an opaque loader dialog.

One trap worth recording: **`CreateFile2` takes five arguments** (the
extended-struct form), not the seven of `CreateFileW` — the four call sites in
`onnxruntime.dll` push exactly one stack argument. The first version of the
shim used the 7-argument shape, read garbage, and failed every file open.
`w7shim-test.exe` (`src/win7shim/test_w7shim.c`) now compares all four
implementations against the real OS ones and must print `RESULT: PASS`.

---

## 3. Quick start (prebuilt)

```
farsi-tts\                     <- deployment folder
├─ farsi-tts.exe               CLI
├─ farsi-tts-server.exe        HTTP API + web UI
├─ farsi-tts-gui.exe           native GUI
├─ onnxruntime.dll             (from the ORT zip, api-set imports patched)
├─ w7shim.dll                  Win7 API-set shim for onnxruntime.dll (required)
├─ w7shim-test.exe             optional self-test of w7shim.dll (prints PASS)
├─ model\
│  ├─ onnx\                    4 graphs + .data + weights.npz + decode_state_init.npz
│  └─ v2\tokenizer_ph.model
├─ voices\                     built-in reference voices (≤5 s)
├─ uploads\                    voices added from the GUI / web UI
├─ dicts\user.dict             pronunciation dictionary (word ⇥ phonemes)
├─ sahel.ttf                   optional: overrides the font embedded in the GUI
└─ web\index.html              server UI (+ web\sahel.ttf)
```

1. Download the model package:
   ```bat
   huggingface-cli download Nimaone/pocket-tts-farsi-v2-onnx --local-dir model\onnx
   ```
   (or copy the `model/onnx` folder from this project), then lower the IR stamp:
   ```bat
   python scripts\prepare_win7_onnx.py model\onnx
   ```
2. Copy a reference voice (5 s, mono, 24 kHz) into `voices\`.
3. Run:
   ```bat
   farsi-tts.exe "سلام، حال شما چطور است؟" --voice voices\male_hello.wav --out out.wav
   farsi-tts-server.exe --base . --port 8000      ->  http://127.0.0.1:8000
                                                      (moves to 8001… if busy)
   farsi-tts-gui.exe
   ```
4. When in doubt: `farsi-tts.exe --help` (every option, every mode), and
   `farsi-tts-server.exe --help` or `http://127.0.0.1:8000/api/help` (every
   endpoint and query parameter). Both are reproduced in §5.

The executables locate `model\` relative to `--base` (default: the working
directory for the CLI/server, the executable's own folder for the GUI).

---

## 4. Building from source

### 4.1 sentencepiece (static)

```bat
git clone -b v0.2.0 https://github.com/google/sentencepiece
cd sentencepiece
cmake -B build -DSPM_ENABLE_SHARED=OFF -DSPM_ENABLE_TCMALLOC=OFF
cmake --build build --config Release
```

### 4.2 the engine

```bat
set ORT_DIR=C:\deps\onnxruntime-win-x64-1.14.1
set SPM_DIR=C:\deps\sentencepiece
scripts\build_win.bat
```

or directly with CMake:

```bat
cmake -S . -B build-win -G "Visual Studio 16 2019" -A x64 ^
      -DORT_DIR=%ORT_DIR% -DSPM_DIR=%SPM_DIR% -DWIN7_STATIC_CRT=ON
cmake --build build-win --config Release --parallel
```

- **Toolset** — VS2019 (v142) is the recommended Win7 toolset; VS2022 works
  only if you retarget the platform toolset and link against the Win7 SDK.
- **`-DWIN7_STATIC_CRT=ON`** links the static CRT (`/MT`), so the engine needs
  no VC++ redistributable at all. Omit it to use `/MD` (then the redist is
  required). ONNX Runtime's prebuilt DLL is `/MD`; a `/MT` app calling its C API
  is safe (no C++ objects cross the boundary).
- **GUI** uses `/utf-8` (set in CMake) for the Persian string literals, and
  ships the **Sahel** font compiled into the exe (`src/sahel_font.h`, generated
  by `scripts/embed_font.py` — regenerate it if you swap the font:
  `python3 scripts/embed_font.py sahel.ttf src/sahel_font.h`).

#### What was actually built and tested

The binaries shipped in `persian-tts-win7/` come from the **MinGW-w64 cross
build**, `scripts/build_win_mingw.sh`, run on Linux:

```bash
scripts/build_spm_mingw.sh     # static sentencepiece -> deps/spm-mingw/build-mingw/src
scripts/build_win_mingw.sh     # -> build-win/{farsi-tts,farsi-tts-server,farsi-tts-gui}.exe + onnxruntime.dll
```

All three exes report `MajorSubsystemVersion 5 / MinorSubsystemVersion 2` (the
Windows 7 / Vista x64 floor) and import **only** `KERNEL32` and `msvcrt`
(`ws2_32` for the server; `USER32`/`GDI32`/`COMCTL32`/`comdlg32`/`WINMM`/
`ole32`/`SHELL32` additionally for the GUI). `onnxruntime.dll` is loaded at
runtime instead of being linked in, and its own imports are served by
`w7shim.dll` — no exe and no shipped DLL imports a `api-ms-win-*` contract that
Windows 7 lacks, and no Win8+ API is imported (see §2, “The `0xc0000005`
startup crash”).

MinGW needs four workarounds that MSVC does not, all already handled in the
script: `-D_stdcall=__stdcall` (the ORT header hardcodes `_stdcall` under
`_WIN32`), `src/ort_sal_compat.h` (MinGW's `sal.h` lacks the SAL annotations the
ORT header uses), `-municode` (the GUI entry point is `wWinMain`) and
`-D__USE_MINGW_ANSI_STDIO=1` (Windows 7's `msvcrt` printf has no `%zu`/`%lld`;
mingw's own ISO-C99 printf keeps the format strings portable).

### 4.3 Linux / macOS (verification)

```bash
scripts/build_linux.sh          # uses deps/onnxruntime-linux-x64-* and sentencepiece
```

---

## 5. Using it

### CLI

`farsi-tts --help` is the authoritative, always-in-sync reference — it is
reproduced and explained below.

#### What "modes" are

There are exactly two **speech modes**, and they only change *phrasing and
silences*, never the words:

| mode | flag | what it does |
|---|---|---|
| `split` | `--mode split` (default) | every comma / semicolon / colon keeps its pause — the natural "reading aloud" rhythm |
| `pack` | `--mode pack`, `--pack` | comma phrases are merged into longer units so the voice takes one breath: fewer and shorter pauses, the sentence flows on |

Both modes still pack the phonemes into model-sized chunks (the model was
trained on ~11-token utterances). The knob that actually changes how long the
silences are is `--pause-scale`. Mode is one of the parameters the pipeline
inherits from the reference implementation (`split`/`pack` are the two branches
of the plan layer).

#### All options

```
input
  --text STR          text to speak (same as the positional argument)
  --text-file PATH    read UTF-8 text from PATH ("-" = stdin). USE THIS ON
                      WINDOWS: cmd/PowerShell cannot carry Persian on argv
  --phonemes-in       treat the input as phonemes, never run the G2P
  --text-in           the opposite: always run the G2P, even when the input has
                      no Persian characters (numeric/symbol-only input, which
                      auto-detection would read as phonemes)
  --phonemes          print the phoneme string for the text and exit
  --phonemes-out P    with --phonemes: also write the phoneme string to P
  --chunks            print the chunk plan and exit — no audio
  --bulk PATH         one text per line; synthesise every line. The model and
                      the reference voice are loaded/encoded ONCE for the
                      whole batch (fast for many short lines)
  --out-dir DIR       output folder for --bulk (default output/bulk)

reference voice
  --voice PATH        reference voice WAV, ~5 s (default voices/male_hello.wav)
  --preload-voice     encode the voice before doing anything else and print
                      the cost (it is cached for every later synthesis)
  --list-voices       print the reference voices found in <base>/voices and
                      <base>/uploads (one path per line) and exit

output
  --out PATH          output WAV (default output/tts.wav)
  --base DIR          package base dir holding model/, voices/, dicts/
  --play              play the result through the speakers after writing it
                      (Windows; blocking until playback ends)

speech parameters
  --mode MODE         split (default) | pack
  --pack              shorthand for --mode pack
  --pace F            time-stretch factor 0.6 .. 1.5 (1.0 = as-is)
  --seed N            reproducible run (default: random each run)
  --temperature F     flow-LM sampling temperature, 0.05 .. 2.0
                      (default -1 = the model value, 0.3)
  --retries N         quality-gate attempts per chunk, 1 .. 4 (default 3)
  --chunk-tokens N    SentencePiece budget per generated chunk, 8 .. 32
                      (default 18 = model training value)
  --pause-scale F     multiply every pause, 0 .. 3 (1.0 = normal, 0 = tight)
  --keep-ezafe        keep the "1" ezafe marker in the phoneme string
  --zero-noise        deterministic noise (harness only; can silence output)

pronunciation dictionary (<base>/dicts/user.dict)
  --dict PATH         merge an extra dictionary file for this run (repeatable)
  --dict-set W=PH     add/replace an entry in <base>/dicts/user.dict, then exit
  --dict-del WORD     delete an entry, then exit
  --dict-import PATH  merge a dictionary file into <base>/dicts/user.dict
  --dict-list         print the dictionary (word<TAB>phonemes) and exit

engine
  --threads N         ONNX Runtime intra-op threads (default: all cores, max 8)
  --version           print version and exit
  -h, --help          this help
```

What each speech parameter actually changes:

| flag | effect | cost |
|---|---|---|
| `--pace` | linear time-stretch of the finished audio; the only post-processing knob | free |
| `--seed` | makes a run reproducible (same seed ⇒ same waveform) | free |
| `--temperature` | flow-LM noise: low = calmer/more repeatable, high = more variation | free |
| `--retries` | how many times a chunk is generated and rejected by the quality gate (loudness, duration, solid-speech, dead-air checks). Attempt 0 uses `--temperature`; each retry warms it by `+0.25` so the attempts really differ. If none passes, the best one is used and a warning is printed (`quiet_chunks` over HTTP) | `N`× the chunk cost |
| `--chunk-tokens` | how many phoneme tokens go into one generation; bigger = longer breaths, smaller = safer chunks | free (affects the phrasing) |
| `--pause-scale` | multiplies every inter-phrase, inter-chunk and sentence gap | free |
| `--keep-ezafe` | keeps the `1` ezafe markers in the phoneme string (they are stripped by default before generation) | free |
| `--zero-noise` | deterministic noise for the verification harness — **keep it off for normal use**: it feeds zero noise to every flow step, which silences many reference voices (the reference itself always uses random noise) | free |

#### Phonemes: see them, edit them, speak them

Persian text is converted to a romanised phoneme string (G2P) before it is
spoken; anything without Persian characters is taken as phonemes already. So a
three-step round trip works with plain flags:

```bash
# 1. what will actually be spoken?
farsi-tts --phonemes "بردارسازی"
bardArsAzi

# 2. fix it in a text editor, or fix the dictionary (below) so it is fixed for good

# 3. speak your version
farsi-tts --phonemes-in "bordArAzi" --out out.wav
#    (a romanized string is auto-detected too, --phonemes-in just forces it)
```

`--phonemes-out ph.txt` writes the string to a file; `--chunks` prints the
model-sized chunk plan that the synthesis would use (also without generating
audio). `--phonemes` honours the other input flags too — `--keep-ezafe` keeps
the `1` markers, `--phonemes-in` prints the input verbatim.

**Auto-detection only looks for Persian characters.** Input without any (a
number, a symbol, a Latin string) is taken as phonemes — which is right for a
romanized phoneme string and wrong for `123`. Force the G2P with `--text-in`
(or `?input=text`, or the web UI's «متن»):

```bash
farsi-tts --phonemes "123"             # 123            (read as phonemes)
farsi-tts --text-in --phonemes "123"   # sad ?o bist ?o se   (G2P, spelled out)
```

#### Pronunciation dictionary

`<base>/dicts/user.dict` (UTF-8, `word<TAB>phonemes`, `#` comments) overrides
the G2P for the words it knows; everything else still goes through the model:

```bash
farsi-tts --dict-set "بردارسازی=bordArAzi"    # add / replace, writes the file
farsi-tts --dict-list                          # show it
farsi-tts --dict-del "بردارسازی"                # remove one entry
farsi-tts --dict-import extra.dict             # merge a file
farsi-tts --dict extra.dict "…"                # merge for this run only
```

Lookup ignores case, punctuation, ZWNJ and Arabic diacritics, so `بُردارسازی`
and `بردارسازی` are the same key. Every `*.dict`/`*.txt` in `<base>/dicts/` is
loaded at startup; the same file is edited by the Win32 GUI, the web UI and
`POST /api/dict`.

#### Bulk generation

```bash
farsi-tts --bulk lines.txt --out-dir output/bulk --voice voices/male_news.wav
```

Each non-empty line of `lines.txt` becomes `output/bulk/part-0001.wav`, … The
**model is loaded once and the reference voice is encoded once** for the whole
batch (that is the point of `--bulk`: a per-line process would pay ~1 s of model
load + ~1 s of voice encoding on every line). Seeds are derived from `--seed`
(`seed + line`), so a batch is reproducible. `--preload-voice` does the same
single voice encoding without batching.

**Use `--text-file` on Windows.** A cmd.exe / PowerShell code page cannot carry
Persian on `argv`, and the text arrives mangled (`????`). Put the text in a
UTF-8 `.txt` and pass the path:

```bat
farsi-tts.exe --base . --text-file text.txt --out output\tts.wav
```

### Server + web UI

```
farsi-tts-server --base . --port 8000 [--host H] [--scan N] [--voice PATH]
                 [--preload] [--dict FILE] [--seed N] [--threads N]
                 [--idle-unload SEC]
```

`--help` prints the same list; `GET /api/help` returns it over HTTP.

**Port selection.** The requested port is the *preferred* one: if it is busy,
the server walks up (`--port P`, then `P+1 … P+scan`, `--scan` default 32) and
binds the first free port, announcing it:

```
port 8000 busy, looking for a free one...
farsi-tts-server 1.1.0
farsi-tts server: http://127.0.0.1:8001  (port 8000 was busy)
```

`--port 0` asks the OS for any free port, `--no-scan` makes a busy port a hard
error, and `PARSIGO_PORT` / `PARSIGO_HOST` override the defaults. The web UI is
served from `web/`, so the printed URL opens it.

#### Endpoints

| method | path | body / query | returns |
|---|---|---|---|
| GET | `/` | — | the Persian RTL web UI |
| GET | `/api/help` | — | this endpoint list, `text/plain` |
| GET | `/api/health` | — | `{"ok":true,"model_loaded":…,"version":…,"port":…,"base":…}` |
| GET | `/api/config` | — | defaults, limits, modes, dict path — what the web UI renders its controls from |
| GET | `/api/stats` | — | `{"requests":…,"synths":…,"last_ms":…,"total_audio_ms":…}` |
| GET | `/api/voices` | — | `["voices/x.wav","uploads/y.wav",…]` |
| POST | `/api/voice?name=x.wav` | raw **WAV** bytes (RIFF/WAVE — anything else is a 400 with the `ffmpeg` command to convert) | `{"path":"uploads/x.wav"}` |
| POST | `/api/voice/load` | `?voice=` | `{"voice":…,"loaded":true,"cached":false,"ms":N,"rms":0.1148,"gain":1.0,"silent":false}` — encodes the reference voice **once** (`cached:true`, `ms:0` on every later call). `rms` = the prompt's original level, `gain` > 1 = it was quiet and was amplified back to the reference level, `silent` = the file has no speech (the clone will be silent) |
| GET | `/api/voice/state` | — | `{"default":…,"loaded":[…],"model_loaded":bool}` |
| POST | `/api/phonemes` | text (or `?text=`) | `{"phonemes":…,"words":[…],"chunks":[…]}` — no audio |
| POST | `/api/chunks` | text or phonemes | `{"chunks":[…],"count":N}` |
| POST | `/api/synth` | text or phonemes | `audio/wav` (or JSON, below) |
| GET | `/api/dict` | — | `{"path":…,"entries":[{"word":…,"phonemes":…}]}` |
| GET | `/api/dict?word=X` | — | `{"found":bool,"phonemes":…}` |
| POST | `/api/dict` | `{"word":"…","phonemes":"…"}` (or `?word=` + raw phonemes) | adds/updates **and saves** the file |
| DELETE | `/api/dict?word=X` | — | removes and saves |
| POST | `/api/unload` | — | `{"unloaded":true}`, frees the weights |

Request bodies are capped at **32 MB** (headers at 64 KB). Every rejection is
*answered* rather than dropped, because a socket released while the client is
still sending (or while a reply is still queued) is reset by the OS and the
caller sees `WinError 10054 / ECONNRESET` instead of a status code:

| situation | response |
|---|---|
| body > 32 MB, headers > 64 KB | `413 Payload Too Large` |
| garbage request line, malformed `Content-Length` | `400 Bad Request` |
| `Transfer-Encoding: chunked` (no `Content-Length`) | `411 Length Required` — bodies are read by `Content-Length` only |
| client hangs up mid-request | no reply possible; the socket is just released |
| client stalls for 120 s while sending | same as above (a stalled peer must not pin a thread) |

Every response states its length and the connection policy: HTTP/1.1 requests
without `Connection: close` are answered with `Connection: keep-alive`
(`Keep-Alive: timeout=15, max=200`) and the same socket then serves up to 200
more requests — a browser reuses its pooled connections, which removes the
per-request connect/FIN churn. `Connection: close` (and HTTP/1.0 without an
explicit keep-alive) is always honoured, and the socket is always shut down
gracefully — write side flushed, then the peer's remaining bytes swallowed,
then the handle released — so a *finished* reply always arrives as a normal
end-of-stream and never as a reset. Reusing a connection after `Connection:
close` therefore gets a clean `EOF`, not an error. Bytes that arrive past a
request's `Content-Length` (a pipelining client) become the *next* request on
that connection instead of being dropped.

Closing the web UI's tab or window sends `POST /api/unload` (a
`navigator.sendBeacon` on `pagehide`), so the ~500 MB of weights leave the
server process together with the page; the next request simply reloads them
(~1 s), exactly like `--idle-unload`.

#### `/api/synth` parameters

The body is always the text (or the phoneme string); everything else is a query
parameter. `GET` works too if you would rather put the text in `?text=`.

| query | values | default |
|---|---|---|
| `input` | `text` \| `phonemes` \| `auto` | `auto` — Persian script ⇒ G2P, romanized ⇒ used as phonemes. `text` **always** runs the G2P (needed for numeric/symbol-only input), `phonemes` **never** does |
| `voice` | `voices/…`, `uploads/…` | `--voice` (default `voices/male_hello.wav`) |
| `mode` | `split` \| `pack` | `split` |
| `pace` | 0.6 … 1.5 | 1.0 |
| `seed` | integer | random per request |
| `temperature` (or `temp`) | 0.05 … 2.0 | model value (0.3) |
| `retries` | 1 … 4 | 3 |
| `chunk-tokens` (or `chunk_tokens`) | 8 … 32 | 18 |
| `pause-scale` (or `pause_scale`) | 0 … 3 | 1.0 |
| `keep-ezafe` | 0/1 | 0 |
| `zero-noise` (harness only — silence risk) | 0/1 | 0 |
| `dict` | `word=phon;word2=phon2` (URL-encoded) | — per-request dictionary overrides |
| `format` | `json` | — returns `{"phonemes":…,"chunks":[…],"seconds":…,"ms":…,"total_chunks":N,"quiet_chunks":M,"seed":"…","wav":"<base64>"}`; `seed` is a **string** so uint64 values survive JSON parsers. When `quiet_chunks` > 0 a `"warning"` string is added (see the silent-output table below) |

The `audio/wav` response also carries `X-Phonemes` (percent-encoded),
`X-Chunks`, `X-Quiet-Chunks`, `X-Audio-Seconds` and `X-Synth-Ms` headers, so a
client can show what was spoken — and whether it may be silent — without a
second request.

```bash
# phonemes only (what will be said)
curl -s -X POST localhost:8000/api/phonemes --data-binary @text.txt

# audio with every knob set
curl -s -o out.wav -D - \
  "localhost:8000/api/synth?mode=pack&pace=1.1&seed=7&temperature=0.25\
&retries=2&chunk-tokens=16&pause-scale=0.8&voice=voices/male_news.wav" \
  --data-binary @text.txt

# speak a hand-edited phoneme string
curl -s -o out.wav -X POST "localhost:8000/api/synth?input=phonemes" \
  --data-binary 'bardArsAzi'

# metadata + audio in one JSON answer
curl -s -X POST "localhost:8000/api/synth?format=json" --data-binary 'سلام'

# teach it a word (persisted to dicts/user.dict)
curl -s -X POST localhost:8000/api/dict \
  -H 'Content-Type: application/json' \
  -d '{"word":"بردارسازی","phonemes":"bordArAzi"}'
```

**Bulk over HTTP** is a loop — the engine and the encoded voice stay warm
between iterations, so only the per-line generation is paid:

```bash
while IFS= read -r line; do
  curl -s -o "$out" --data-binary "$line" "localhost:8000/api/synth?seed=$n"
done < lines.txt
```

(`POST /api/voice/load` once before the loop if you want to see the encoding
cost separately; otherwise the first synth encodes it and every later one reuses
it.)

#### The web UI

`web/index.html` (single file, no CDN — it works offline) drives all of the
above: text + voice + mode + speed, the phoneme view/edit, the dictionary
table, **every** generation parameter with its live limits from `/api/config`,
and a bulk tab that synthesises many lines sequentially with per-line download
links. It polls `/api/health` for its connection dot and surfaces API errors
inline instead of failing silently.

`PARSIGO_HOST` / `PARSIGO_PORT` override host/port.

### Model lifetime, RAM, and shutting down

**The model is loaded once and reused.** The server builds the engine lazily on
the first `/api/synth` (not at startup), keeps it for every later request, and
the GUI builds it once after the window appears. Loading costs ~1 s and ~500 MB,
so a request never pays it twice. Synthesis is serialised with a mutex, because
the engine owns shared ONNX sessions and a voice cache.

**Freeing the weights without closing the app.** `POST /api/unload` destroys the
engine and trims the working set; the next request transparently reloads (~1 s).
`--idle-unload SEC` does it automatically after `SEC` idle seconds — idle time
is measured from the *end* of the last request, so a long synthesis never looks
idle while it runs. Measured on the 2-core host:

| state | resident memory |
|---|---|
| server running, model not yet loaded | **7 MB** |
| after a synthesis (model resident) | **903 MB** |
| after `POST /api/unload` or `--idle-unload` | **32 MB** |

Destroying the engine only returns memory *to the allocator*, which keeps it —
the working set is trimmed explicitly (`SetProcessWorkingSetSize` on Windows,
`malloc_trim` on glibc), otherwise Task Manager would still show ~900 MB.

**Closing the console shuts the server down and frees the port.** `SIGINT`
(Ctrl+C) and `SIGTERM` set a stop flag; on Windows `SetConsoleCtrlHandler` also
covers Ctrl+Break and closing the console window. The accept loop polls the flag
once a second, then closes the listening socket, waits for in-flight requests to
finish, destroys the engine and exits 0:

```
farsi-tts server: stopping (model released, port closed)
```

Verified: `SIGINT` → process gone, port free, re-bindable immediately; the same
Ctrl+C in the console under the Windows 7 Wine prefix also ended the process
with the port released (see `06-server-shutdown.png`). The GUI frees the engine
and trims on `WM_DESTROY`, so closing the window returns the memory too
(`model released on close` in `farsi-tts-gui.log`).

### GUI (Win32)

`farsi-tts-gui.exe` is a native Win32 window (user32/comctl32/comdlg32/winmm/
wininet, Common Controls v6 — themed via the manifest in `cmake/gui.rc`) — no
browser, no runtime. It has **five tabs**, so every knob the CLI and the HTTP
API expose is reachable from it:

| tab | what is there |
|---|---|
| **متن و ساخت** | text box (RTL/LTR follows the text — `IMF_AUTORTL`; Ctrl+A selects all), reference-voice dropdown, **افزودن صدا…** (add a WAV — this build decodes RIFF/WAVE only), **پیش‌بارگذاری صدا** (encode the reference voice once, shows the ms), speech-mode dropdown (split/pack), speed slider, **ساخت گفتار** / **پخش** / **ذخیره WAV…** |
| **آوانگاشت** | **استخراج آوانگاشت** (run the G2P and show the result), an *editable* phoneme box, **ساخت از روی همین آوانگاشت‌ها** (synthesise exactly that string, skipping the G2P), and the chunk plan |
| **واژه‌نامه** | the `dicts/user.dict` contents as `word⇥phonemes` lines — add/edit/delete entries and **ذخیرهٔ واژه‌نامه** writes the file the CLI and the API read |
| **تنظیمات** | seed (+ random), temperature, retries, chunk-tokens, pause-scale, ONNX threads, zero-noise, keep-ezafe, **همیشه G2P کن** — i.e. `--seed --temperature --retries --chunk-tokens --pause-scale --threads --zero-noise --keep-ezafe --text-in` |
| **دانلود مدل** | the download list from `models.json` (model graphs + weights + sample voices) with size and progress, **دانلود / دانلود همهٔ باقی‌مانده‌ها / لغو**, and **بازخوانی مدل** which releases and reloads the engine so new files take effect without restarting the program |

**Language (فارسی / English).** The dropdown at the bottom right switches every
label, hint, status line and the window title at runtime; the fonts follow
(Sahel for Persian, Segoe UI for English) and the sample text is re-translated
while it is still untouched. `--lang en` starts it in English.

**Reference voice: load once, offload on switch.** The selected voice is
encoded once and reused for every later synthesis (same cache the CLI `--bulk`
uses). Picking a different voice in the dropdown **offloads the previous
one** (its KV prompt is freed) and encodes the new one, so only the voice in
use is ever held in RAM.

**models.json.** A flat list of `{"name","url","dir","kind","size","desc"}`
objects; `dir` is relative to the program folder, `kind` is `model` / `voice` /
`dict`. The file is created next to the exe from the built-in list on first
run (17 entries: the whole `model/onnx` graph set, the tokenizer, and three
sample reference voices from the Hugging Face repos) and is meant to be
edited — add your own hosted voices or a mirror and the tab picks them up.
Downloads use WinInet (`InternetOpenUrl` + `HttpQueryInfo`), land in
`<dir>/<name>` via a `.part` file, and a finished `kind: voice` file appears
in the voice dropdown immediately.

The engine loads on a worker thread so the window paints and stays responsive
during the ~1 s load; synthesis runs on another worker thread, the UI shows the
resulting audio length and phonemes, and playback uses `PlaySound(SND_MEMORY)`
(no temp files). Errors go to `farsi-tts-gui.log`.

**Sahel font.** The Sahel TTF (`/content/sahel.ttf`, (c) Saber Rastikerdar, OFL)
is **embedded in the exe** and registered with `AddFontMemResourceEx`, so every
Persian label renders in Sahel on any machine. Putting a `sahel.ttf` next to the
exe overrides it (it is loaded first); if neither can be registered the GUI
falls back to the first Arabic TrueType face it finds, then Tahoma — common
controls do no complex-script shaping, so a face without Arabic glyphs would
paint the labels as boxes. Regenerate the embedded blob with:

```bash
python3 scripts/embed_font.py /content/sahel.ttf src/sahel_font.h
```

### Custom reference voices

Any 5-second mono clip works; longer clips are truncated and a "hot" opening
syllable is trimmed automatically (the model otherwise replays it over the
first word). Voices are encoded to a speaker embedding once and cached. The
model is trained on ≤5 s prompts — keep uploads short.

**Loading the reference voice once.** Encoding costs ~1 s, so it is worth doing
exactly once when you generate a lot:

| where | how |
|---|---|
| CLI | `--preload-voice` (prints the ms) or just `--bulk` (loads model + voice once for the whole batch) |
| HTTP | `POST /api/voice/load?voice=voices/x.wav` before a loop of `/api/synth`; or let the first synth encode it — every later one reuses the cache |
| GUI | **پیش‌بارگذاری صدا** on the first tab; picking another voice in the dropdown offloads the old encoding and encodes the new one once |
| process lifetime | the cache lives in the engine; only `POST /api/unload` / `--idle-unload` / closing the GUI drops it (and those reload the model anyway). The web UI sends `POST /api/unload` itself when its tab or window is closed |

`GET /api/voice/state` tells you which voices are already encoded.

#### Why is the output silent?

Four separate causes, all reproduced while testing:

| cause | how to recognise it | fix |
|---|---|---|
| the reference clip is quiet — or contains no speech | `POST /api/voice/load` returns `"silent":true` for an empty clip, and `"gain":N` (> 1) for one that was boosted; the web UI says «صدای کم‌بلند N× تقویت شد» | record ~5 s of clear speech. The engine amplifies anything more than ~16 dB below the level of `voices/male_hello.wav` back to that level (never enough to clip), so a quiet-but-real recording is normalised for you; only a genuinely empty file stays empty |
| `zero-noise` is on | soft-to-silent output with most reference voices — reproduced even with the bundled `male_hello.wav` at −6 dB | turn it off («نویز صفر» checkbox, `--zero-noise`, `zero-noise=0`). It exists only for the bit-exact harness, which feeds zero noise at *every* flow step; `ref/tts_onnx.py` itself always uses random noise. For reproducible output use `--seed` — that is deterministic too, and it does not collapse |
| the uploaded file is not a WAV | `400 reference voices must be WAV (RIFF/WAVE)` — anything else is refused at upload time (older builds accepted it and the server died on the first load) | `ffmpeg -i in.mp3 -ar 24000 -ac 1 out.wav` |
| one chunk failed the quality gate | JSON `"quiet_chunks": N` (> 0, plus a `"warning"` string), header `X-Quiet-Chunks: N`, CLI stderr `warning: N of M chunk(s) stayed below the quality gate…`, GUI status «… N چانک … ممکن است بی‌صدا یا ناقص باشد», web status bar in amber | another `--seed` / `seed=` — the sampler is re-drawn at a higher temperature on each retry (attempt 0 keeps your exact `--temperature`), so raising `--retries` to 4 helps too; if it persists, the phoneme string itself is probably wrong |

The quality gate runs once per chunk: **rms ≥ 0.02**, a solid duration of
0.30…1.6× what the token count predicts, ≥ 55 % solid frames and no dead-air
gap longer than 0.35 s. `retries` (default 3) re-draws the sample — attempts
after the first warm the temperature up by `+0.25` each, because at the model
default of 0.3 sampling is nearly greedy and a fresh noise draw used to
reproduce the very same silent chunk. Whatever clears the gate wins; if none
does, the best attempt (loudest first, then closest to the expected duration)
is returned and the run is flagged (`quiet_chunks`) — silent or truncated
audio is never handed back unannounced. A sweep of 16 seeds over the test
phoneme string `bordArAzi` (which used to return silence for seeds 3 and 5)
now gives 16/16 audible output.

Output level follows the reference level on the normal (random-noise) path —
a −10 dB prompt gives roughly −13 dB of speech — so use a healthy recording
when you want a loud clone.

### One parameter, three places

Every generation parameter has exactly one name in each front-end, so a
setting can be copied between them verbatim:

| meaning | CLI | HTTP query | GUI |
|---|---|---|---|
| phrasing mode | `--mode split\|pack` | `mode=` | «حالت گفتار» combo |
| speech rate | `--pace` | `pace=` | «سرعت گفتار» slider |
| reproducibility | `--seed` | `seed=` | «بذر» field |
| flow-LM temperature | `--temperature` | `temperature=` | «دما» slider |
| quality-gate attempts | `--retries` | `retries=` | «تلاش دوباره» combo |
| chunk token budget | `--chunk-tokens` | `chunk-tokens=` | «بودجهٔ چانک» slider |
| pause length | `--pause-scale` | `pause-scale=` | «مقیاس مکث» slider |
| ezafe markers | `--keep-ezafe` | `keep-ezafe=1` | «نشانهٔ اضافه» checkbox |
| deterministic noise | `--zero-noise` | `zero-noise=1` | «نویز صفر» checkbox |
| force phoneme input | `--phonemes-in` | `input=phonemes` | «ساخت از روی همین ونایه‌ها» |
| force the G2P | `--text-in` | `input=text` | «حالت ورود: متن» (web) |
| reference voice | `--voice` | `voice=` | voice dropdown |
| per-run dictionary | `--dict FILE` | `dict=word=phon;…` | dictionary tab |
| ONNX threads | `--threads` | `--threads` (start-up) | «رشتهٔ ONNX» combo (start-up) |

---

## 6. Performance

The pipeline is autoregressive (a 6-layer flow LM stepping token by token, then
a streaming Mimi vocoder stepping frame by frame), so cost scales with output
length and **this model is not faster than real time on CPU.** Budget roughly
**8–18 s of compute per second of audio** on a slow 2-core Xeon; a modern
desktop core is several times quicker. What the port does buy is: no Python, no
PyTorch, no interpreter, a 4.4 MB executable, and one DLL.

Measured on the verification host — **2 vCPU Intel Xeon @ 2.2 GHz, 12 GB RAM,
CPU only, no GPU**:

| run | audio | wall | s of compute per s of audio |
|---|---|---|---|
| Win7 build under Wine (6.1.7601 prefix), 1 short sentence | 1.29 s | 10.0 s | 7.7× |
| Linux native, same sentence, same seed | 1.29 s | 9.97 s | 7.7× |
| Linux native, 2-sentence paragraph | 14.99 s | 266 s | 17.7× |
| Win7 build under Wine, same paragraph | 17.79 s | 136 s | 7.6× |
| `POST /api/synth` over HTTP (server already warm) | 2.91 s | 58.8 s | 20× |

Two caveats on the last two rows: sampling is stochastic without `--seed`, so
the LM stops at a different point and the audio lengths differ; and the HTTP
row includes per-request overhead. Compare *per second of audio*, not per run.

The engine is **~3–5× faster than the reference Python path** on identical
input. ONNX Runtime does the heavy math; all host logic (G2P greedy loop, KV
scatter, state wiring, chunking, quality gates) is plain C++. Build with `-O2`.

**Threads.** `intra_op_num_threads` defaults to *auto* — all cores, capped at 8
— because the FlowLM and Mimi steps are small, latency-bound matmuls that scale
with cores. Measured on the 2-core host: 1 thread 20.3 s vs 2 threads 13.7 s for
the same sentence (1.5×). Override with `--threads N`. Note that ORT's float
reduction order changes with the thread count, so audio is bit-reproducible only
at a **fixed** thread count.

The retry loop is the biggest cost knob: a seed whose first attempt trips a
quality gate pays for up to three generations.

---

## 7. Verification

The port is checked against the reference implementation at each stage
(`scripts/`):

| check | result |
|---|---|
| G2P phonemes (incl. number expansion) | **exact match** |
| phoneme chunk plan (token budget, bound phrases) | **exact match** |
| SentencePiece token ids + LUT gather | **exact match** |
| voice conditioning (Mimi encode + speaker projection) | match to ~5e-5 |
| FlowLM KV cache after each step | match to 6 decimals |
| FlowLM latents given identical conditioning, zero noise | **bit-exact (0.0)** |
| Mimi decoder given identical latents | match to 1e-4 (16-bit) |
| overall audio length | within ~0.1 % |
| whole harness | `python3 scripts/verify_engine.py` → **11/11 passed** |
| Win7 binary vs Linux binary, same seed | 31007/31007 samples **bit-identical** (§8.1) |

Reproduce:

```bash
scripts/build_linux.sh
python3 scripts/verify_engine.py                # -> 11/11 passed
```

The individual stage comparisons are in `scripts/verify_ref.py`,
`scripts/verify_flow.py` and `scripts/debug_flow.py`.

The only residual difference is the speaker-projection matmul accumulating in
double instead of numpy's float32 BLAS: with zero noise that micro-difference
eventually diverges through the autoregressive flow LM, which is why the
flow-equivalence test feeds the reference the C++ conditioning. With normal
(non-zero) noise both paths are stochastic and both produce valid speech.

---

## 8. Testing on a Windows 7 image (VM)

No Win7 machine is required to build, but to validate on the target:

1. **VM:** VirtualBox/VMware, a Windows 7 SP1 x64 image, install
   `vc_redist.x64.exe` and **KB2999226**.
2. Copy `farsi-tts.exe`, `onnxruntime.dll`, **`w7shim.dll`**, and the `model\` folder.
3. Smoke test:
   ```bat
   farsi-tts.exe --phonemes "سلام"        REM engine + tokenizer load
   farsi-tts.exe "سلام دنیا" --out t.wav  REM full pipeline
   farsi-tts-server.exe --port 8000       REM open http://127.0.0.1:8000
   ```4. If `onnxruntime.dll` fails to load (`Windows error 126`), a *dependency* of
it is missing: install `vc_redist.x64.exe` and **KB2999226**, and keep
`onnxruntime.dll` + `w7shim.dll` next to the exe. If you are on ORT ≥ 1.15,
install the 1.14.1 build or compile ORT with `CMAKE_SYSTEM_VERSION=6.1` (§2).
5. If a process dies *before* its first console line, run `w7shim-test.exe` in
the same folder: `RESULT: PASS` means the API-set shim works on that machine;
a `MISSING` line in `w7shim.log` (created when `W7SHIM_LOG=1`) names the
unresolved import.

### 8.1 Results actually obtained (Wine, Windows 7 version reported 6.1.7601)

**These were produced under Wine configured to report itself as Windows 7, on
Linux. Wine is a compatibility layer, not Windows: it proves the binaries use
no API newer than Win7 and that the ORT 1.14 / IR-8 combination loads, but it
is *not* proof of behaviour on real Windows 7 hardware.** Treat §8.1 as a
pre-flight check and still run §8 on a real VM.

Setup used: `WINEPREFIX` with `Version=win7` and `CurrentVersion=6.1`,
`wine cmd /c ver` → `Microsoft Windows 6.1.7601`, Xvfb + openbox for the GUI.

| check | result |
|---|---|
| `wine cmd /c ver` | `Microsoft Windows 6.1.7601` |
| `farsi-tts.exe --help` | exit 0, full usage |
| `--phonemes --text-file` | `salAm hAle SomA Cetor ?ast` — **identical to the Linux build** |
| full synthesis, `--seed 3 --zero-noise` | `1.29s` audio; 31007 samples @ 24 kHz |
| same seed on Linux native | 31007 samples, **31007/31007 samples bit-identical**, max abs diff `0` |
| IR version of every graph | `8` (ORT 1.14 max) — loads, no `Unsupported model IR version` |
| model package size after patching | `469 MB` (unchanged; weights never re-serialised) |
| `farsi-tts-server.exe --port 8210` | `/api/health` → `{"ok":true,"model_loaded":…}` |
| `GET /api/voices` | 3 voices |
| `GET /` | 200, `text/html`, Persian RTL page |
| `POST /api/synth` | 200, `audio/wav`, 69899 samples @ 24 kHz |
| server lazy load / reuse | `model_loaded:false` → `true` after one synth, still `true` after the next |
| `POST /api/unload` | `{"unloaded":true}`, then `model_loaded:false`, next request reloads and returns 200 |
| Ctrl+C in the console | process exited (code 0), **listener on the port gone**, no `farsi-tts-server.exe` left |
| `farsi-tts-gui.exe` | window up, 812×286, Persian labels render |
| GUI **ساخت گفتار** button click | 88339 samples synthesised, played, **no crash**, process alive |
| GUI close (Alt+F4) | log shows `model released on close`, process gone |
| GUI crash log | `farsi-tts-gui.log` reaches `WM_APP_DONE: played` |

Second round of tests, after the `0xc0000005` fix (§2), on the same
Win7-reported prefix — **with `W7SHIM_FORCE_OURS=ALL`, which forces all 206
shim imports onto the Windows-7 code paths even where Wine/Windows 10 would
use their own implementations:**

| check | result |
|---|---|
| exe import tables (pefile) | exes no longer import `onnxruntime.dll`; no `api-ms-win-*` anywhere in the exes |
| patched `onnxruntime.dll` | 33 descriptors → `w7shim.dll`, byte size unchanged (9 271 704), MSVCP140/VCRUNTIME imports untouched |
| `w7shim.dll` exports | 206 = exactly the set `onnxruntime.dll` imports from it |
| `w7shim-test.exe` (ours vs real OS) | **`RESULT: PASS`** — CreateFile2 (7 cases), PathCch (12 cases), clock |
| `farsi-tts.exe --help` | exit 0, full usage, **no ORT load needed** |
| full synthesis, forced Win7 paths | 8.54 s audio, **byte-identical to the Linux build** (md5 `229e5ae22b8fa58f60693c42a2372f32` both) |
| shim resolution log | 206 lines, `4 ours` (the Win8-only functions), `0 MISSING` |
| server start → `GET /api/health` | `{"ok":true,"model_loaded":false}` |
| `POST /api/synth` | 200, 318 830-byte WAV in 48.8 s (forced Win7 paths) |
| health after synth / `POST /api/unload` | `model_loaded:true` → `{"unloaded":true}` → `false` |
| server `SIGINT` | `farsi-tts server: stopping (model released, port closed)`, no process left, port closed |
| `farsi-tts-gui.exe` under Xvfb | alive after 75 s, engine loaded (`farsi-tts-gui.log` has no `engine load failed`), shim log 206/0 missing |
| `scripts/verify_engine.py` (Linux) | **11/11** |

Breadcrumbs a failing machine gives you: the CLI/server print
`farsi-tts: loading onnxruntime.dll ...` *before* the DLL is touched, then
`farsi-tts: onnxruntime.dll loaded, ORT 1.14.1`, or a readable
`cannot load onnxruntime.dll (Windows error N)` with a hint.

Screenshots in `../persian-tts-win7-screenshots/`, regenerated after the
`0xc0000005` fix from the extracted ZIP on the Win7-reported prefix:

| file | shows |
|---|---|
| `01-gui-idle.png` | GUI window, engine loaded (its log has no `engine load failed`) |
| `03-cli-terminal.png` | CLI synthesising from the extracted ZIP; printed MD5 = the Linux reference |
| `04-server-http.png` | `/api/health` → `POST /api/synth` (318 830-byte WAV) → `model_loaded:true` → `/api/unload` → `false` |
| `06-server-shutdown.png` | `SIGINT` → `farsi-tts server: stopping (model released, port closed)` → port refused |

Not regenerated in this round: `02-gui-after-synth.png` (needs an automated
click on the Persian button) and `05-web-ui.png` (needs a browser — the
earlier one was the page **served by the Win7 server** but rasterised by Qt
WebKit on Linux: it showed the page, not a Windows browser).

One caveat on the shutdown row: sending Ctrl+C into the console window with
`xdotool` ended the process and released the port but the app's own
`stopping (...)` line did not appear — a synthetic key press does not become a
real `CTRL_C_EVENT` for the target process. Signalling the process (`SIGINT`,
which Wine maps to a console control event) *does* run the handler: the message
appeared both on Linux and on the Windows 7 prefix. Either way the port is
released; the console-window-close path is code-reviewed but was not exercised
directly, since Wine has no window to click the X on.

---

## 9. Limitations & license

- **Non-commercial only.** The model is **CC-BY-NC-4.0**; this port is a
  derivative and carries the same restriction. The Persian G2P component is MIT.
  Credit to the original authors (see the model card).
- **Not real time.** Budget ~8–18 s of compute per second of audio on a slow
  2-core CPU (§6). This is an autoregressive flow LM + streaming vocoder, not a
  single-pass vocoder like VITS/Kokoro; there is no llama.cpp-style
  speculative-decoding trick that removes the sequential dependency.
- **No ready-made exe exists for this model.** sherpa-onnx ships VITS / Kokoro /
  Matcha / Piper runtimes and controls the text front-end itself; this model is
  a ByT5 G2P + flow LM + Mimi vocoder, so it cannot be loaded by it. The whole
  pipeline is reimplemented here and links the same inference core sherpa-onnx
  bundles (ONNX Runtime) — see §1.
- The published graphs ship at **IR 10**, which the last Win7-capable ONNX
  Runtime (1.14.1) cannot load. `scripts/prepare_win7_onnx.py` lowers the IR
  stamp to 8 **in place**; opsets stay at 18, which ORT 1.14 does support.
- `--pace` uses a simple linear time-stretch (pitch changes slightly); the
  reference used a phase-vocoder time-stretch.
- RNG is the engine's own (xorshift64*/Box-Muller); a fixed `--seed` is
  reproducible *on this engine*, not bit-identical to numpy's generator.
- Audio is bit-reproducible only at a fixed `--threads` value (ORT reduces in a
  thread-count-dependent order).
- The GUI asks the font mapper for a face that covers the Arabic block
  (`EnumFontFamiliesExW` with `ARABIC_CHARSET`); without it the common controls
  paint tofu boxes. Win7 usually needs no extra font — Segoe UI/Tahoma are
  font-linked — but a machine with neither gets `Tahoma`.
- CPU only, x64. ARM64/GPU are out of scope.
- **The MSVC build path (`scripts/build_win.bat`, `CMakeLists.txt`) has never
  been executed** — no MSVC was available in the build environment. Only the
  MinGW-w64 cross build was run.
