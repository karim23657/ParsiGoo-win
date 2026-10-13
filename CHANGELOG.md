# Changelog

All notable changes since **“update 5”** (2026-10-08), the last package that
was published on GitHub. Every item below was verified by diffing the old
published files against the current build (or by running both).

## 2026-10-13 — source publication + GUI/server/CLI release

### This repository is new

The previous GitHub package shipped binaries only (executables, model, web
page). This tree adds the **source code** (`src/`, `cmake/`, `CMakeLists.txt`),
the **build scripts** (`scripts/`), the **tests**, the reference engine
(`ref/`), the documentation and the screenshots — everything needed to build
and re-verify the project from scratch.

---

### Desktop GUI — rebuilt, 4 tabs → 5 tabs  (`farsi-tts-gui.exe`)

The old GUI was verified by running the previously published executable: 4 tabs
(«متن و ساخت | تنظیمات | واژه‌نامه | ونایه‌ها»), Persian-only, system font, no
downloader. The new build (`src/gui_win32.cpp`, ~1 830 lines) changes:

- **Five tabs** — «متن و ساخت | آوانگاشت | واژه‌نامه | تنظیمات | **دانلود مدل**»;
  the old «ونایه‌ها» tab was renamed «آوانگاشت», and a new model-download tab
  was added.
- **Persian ⇄ English UI** — a language combo in the window (also `--lang en`);
  every label, status message, dialog and tooltip is translated (table-driven
  `STR[]` + `T()`). Persian text renders in **Sahel** (`sahel.ttf` next to the
  exe), English in Segoe UI.
- **Model download dialog** — reads `models.json` (17 files, name/dir/kind/
  size/url/desc) and downloads through WinInet with a progress bar, writing
  each file as `*.part` and renaming it only when complete (a failed download
  never leaves a half-written model).
- **Voice load-once / offload-on-switch** — choosing a voice loads and encodes
  it a single time; it is reused for every synthesis and released when you
  switch to another voice (status line reports both events).
- **RTL input + Ctrl+A** — the text and dictionary boxes get
  `WS_EX_RTLREADING` (standard Win32 RTL for plain edit controls) and
  `IDM_SELALL`, so the input behaves like a normal Persian editor.
- **Phoneme & dictionary editing** — phonemes per word with copy support, and
  `dicts/user.dict` edited in place with live re-parse and `[x]`/`[ ]` markers
  for active entries.
- **Text fits every button** — controls are auto-sized; no clipped labels.
- **Wide-format fixes** — all wide printfs use `%ls` (ISO C99), eliminating the
  garbage characters MinGW produced in dialogs.

### HTTP server  (`farsi-tts-server.exe`)

- **HTTP/1.1 keep-alive** — one connection now serves up to **200 requests**
  with at most **15 s** idle in between (`Keep-Alive: timeout=15, max=200`);
  browsers reuse their pooled connections instead of reconnecting per request.
  `Connection: close` and HTTP/1.0 are always honoured, and a connection always
  ends shutdown → drain → close (never a reset / WinError 10054).
- **Pipelining** — bytes that arrive past a request’s `Content-Length` are
  carried over to the next request instead of being dropped.
- **`/api/help`** documents the connection policy and the unload endpoint.
- The API surface itself is unchanged: `/api/help`, `/api/health`,
  `/api/stats`, `/api/config`, `/api/voices`, `/api/voice` (+`/load`),
  `/api/phonemes`, `/api/chunks`, `/api/synth`, `/api/dict`, `/api/unload`.

### Web UI  (`web/index.html`)

- **Unload beacon** — the page now listens for `pagehide` and sends
  `navigator.sendBeacon('/api/unload')`, so closing the tab or window hands the
  ~500 MB of weights back immediately; the next request reloads them (~1 s).
  (The modern dark RTL interface itself already shipped in update 5.)

### CLI  (`farsi-tts.exe`)

- **New `--list-voices`** — prints the reference voices found in
  `<base>/voices` and `<base>/uploads` (one path per line) and exits.
- **New `--play`** — plays the written WAV through the speakers on Windows
  (blocking until playback ends).
- **Bug fix** — voice-path printing used `printf("%s", std::string)` (undefined
  behaviour) and produced a garbage prefix on both Linux and Windows; now
  prints clean paths.
- No flags were removed and `--version` is unchanged (`farsi-tts 1.1.0`).

### Documentation  (`docs/win7.md`, 949 lines)

- `--list-voices` and `--play` added to the CLI reference.
- The connection section was rewritten: keep-alive policy, pipelining, idle
  timeout and the tab-close unload beacon (replacing “the server never keeps a
  connection alive”).
- The GUI chapter now describes five tabs, the language switcher, the
  Sahel/Segoe UI fonts, WinInet, the embedded manifest, and the renamed
  phoneme tab.

### Windows 7 tooling

- `w7shim.dll` / `w7shim-test.exe` rebuilt from **unchanged sources** (only 9
  bytes differ — the PE timestamp).

---

## Test gates at this release

| Gate | Result |
|------|--------|
| `scripts/verify_engine.py` — byte-exact vs the Python reference | **11/11** |
| `scripts/test_web_ui.py` — web UI + API (creates its own fixtures with ffmpeg) | **25/25** |
| `scripts/test_protocol.py` — keep-alive, close, HTTP/1.0, pipelining, 4xx, idle-close 15 s, 4 concurrent synths, unload beacon | **12/12** |
| CLI smoke (`--help`, `--list-voices`, synthesis, `--play`) on Linux + Wine | pass |
| GUI smoke on Wine: all 5 tabs, fa/en switch, Ctrl+A, download flow, voice load/offload | pass |
| Screenshots regenerated (web 01–11, GUI 12–18) | 18 PNGs |
