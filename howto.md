# How to publish this project on GitHub

Step-by-step: put this source tree in a repository, ship the **built
executables** and the **model files** as *Releases*, and add the screenshots.
Every block is copy-paste — replace `USER` with your GitHub account name.

## 0. Prerequisites

```bash
git --version
gh --version        # GitHub CLI — if missing: https://cli.github.com/
gh auth login        # once, for gh commands
zip -v | head -1     # or 7z
```

Optional, only where noted below: `huggingface-cli` (model bundle),
Python 3 + `pip` (test gates), MSYS2/MinGW (building the exes).

## 1. What is in this folder — and what is deliberately not

| In the repository | Not in the repository (published some other way) |
|---|---|
| `src/`, `cmake/`, `CMakeLists.txt` — all source code | `build/`, `build-win/` — compiled objects & exes |
| `scripts/` — build, verification, Windows 7 tooling | `deps/` — downloaded toolchains (~415 MB) |
| `web/`, `docs/`, `dicts/`, `voices/`, `ref/` | `model/onnx/*.{onnx,data,npz}` — ~470 MB of weights → **Release** (step 4) |
| `model/v2/` — tokenizer (340 KB) + normalizer | `uploads/`, `output/`, `dist/` — runtime & packaging output |
| `models.json`, `sahel.ttf`, `README.md`, `CHANGELOG.md`, `howto.md` | built `*.exe`, `*.dll` → **Release** (step 4) |
| `screenshots/` — 18 UI screenshots (~1.3 MB) | |

`.gitignore` already enforces this — after adding files, `git status` should
never show weights, exes or zips.

## 2. Upload the repository

### Option A — create a new repository (recommended, nothing is touched)

```bash
cd /path/to/this/folder
git config user.name  "Your Name"            # once per machine, skip if set
git config user.email "you@example.com"
git init
git add .
git commit -m "parsi-go 1.1.0 — source, docs, tests, screenshots"
gh repo create offline-persian-tts-win7 --public --source . --push
```

`gh repo create … --source . --push` creates the repository **and** pushes
`main` in one step. Result: `https://github.com/USER/offline-persian-tts-win7`.

### Option B — reuse the existing repository

Your existing repo (`https://github.com/karim23657/offline-persian-tts-win7`)
currently contains the old binary-only package (“update 5”) at its root.
Pushing this tree **replaces** it on `main`, so back up first:

```bash
# in a clone of the existing repository — keep the old state reachable:
git branch backup-update-5

# in THIS folder — attach the old repo and overwrite main:
cd /path/to/this/folder
git remote add origin https://github.com/karim23657/offline-persian-tts-win7.git
git branch -M main
git push --force origin main
```

The old content stays in the local `backup-update-5` branch (and in GitHub’s
ref cache for a while). If you prefer to keep it forever, tag it as an
“archive” release before forcing:

```bash
git tag archive/update-5 backup-update-5
git push origin archive/update-5
```

> **Option C (least invasive):** leave the old repo untouched and upload this
> folder as `source-v1.1.0.zip` on a release of that repo (step 4 style) —
> the web page then links both.

## 3. Build the Windows executables

```bash
# MSYS2 MinGW64 shell, from the repository root:
./scripts/build_win_mingw.sh
#   -> build-win/farsi-tts.exe          (CLI)
#   -> build-win/farsi-tts-gui.exe      (Win32 GUI)
#   -> build-win/farsi-tts-server.exe   (HTTP server + web UI)
#   -> build-win/w7shim.dll, w7shim-test.exe, onnxruntime.dll

./build-win/farsi-tts.exe --version    # farsi-tts 1.1.0 (pocket-tts-farsi-v2, ONNX)
```

Details, VS2019 route and Win7 notes: [`docs/win7.md`](docs/win7.md).

### Run the release gates before publishing anything

```bash
python3 -m pip install -r scripts/requirements.txt
python3 -m playwright install --with-deps chromium      # for test_web_ui.py

# build the Linux server for the gates (faster than running the exe):
cmake -B build -DORT_DIR=$PWD/deps/onnxruntime-linux-x64-1.20.0 \
      -DSPM_DIR=$PWD/deps/sentencepiece-0.2.0 && cmake --build build -j
./build/farsi-tts-server --base . --port 8123 --no-scan &

python3 scripts/verify_engine.py      # 11/11  byte-exact vs the Python reference
python3 scripts/test_web_ui.py        # 25/25  web UI/API  (rewrites screenshots 01-11)
python3 scripts/test_protocol.py 127.0.0.1 8123   # 12/12 keep-alive/stress
```

All three must be green; their expected scores are listed in
[`CHANGELOG.md`](CHANGELOG.md#test-gates-at-this-release).

## 4. Create the releases (executables + model)

### 4.1 Package the two archives

```bash
V=1.1.0
B=build-win
STAGE=dist/stage-win64
rm -rf "$STAGE" && mkdir -p "$STAGE"/model/{onnx,v2} dist

# (a) program bundle — flat layout, no weights (~25 MB)
cp "$B"/farsi-tts.exe "$B"/farsi-tts-gui.exe "$B"/farsi-tts-server.exe \
   "$B"/onnxruntime.dll "$B"/w7shim.dll "$B"/w7shim-test.exe "$STAGE/"
cp -r web docs dicts voices "$STAGE/"
cp sahel.ttf models.json "$STAGE/"
cp model/onnx/manifest.json "$STAGE/model/onnx/"
cp model/v2/normalize_fa.py model/v2/tokenizer_ph.model "$STAGE/model/v2/"
(cd "$STAGE" && zip -qr "../parsi-go-win64-v$V.zip" .) && rm -rf "$STAGE"

# (b) model bundle — weights are not in git, fetch them first (~470 MB)
huggingface-cli download Nimaone/pocket-tts-farsi-v2-onnx --local-dir model/onnx
zip -0 -r "dist/parsi-go-model-onnx-v$V.zip" model/onnx model/v2
#   -0 = store only: raw floats don't compress, and this keeps it fast
```

(If you already have `model/onnx/` populated — e.g. from an earlier install —
skip the download line.)

### 4.2 Publish the release with notes + screenshots

```bash
V=1.1.0
cat > dist/RELEASE_NOTES.md <<EOF
## parsi-go $V — Windows 7 build + model

**Install:** unzip **both** archives into the same folder and start
\`farsi-tts-gui.exe\` (needs Windows 7 SP1 x64, VC++ redist; details in
\`docs/win7.md\`).

- GUI: 5 tabs, Persian/English, RTL input + Ctrl+A, model-download dialog
- Server: HTTP/1.1 keep-alive, \`/api/unload\`, web UI with unload beacon
- CLI: \`--list-voices\`, \`--play\`

![GUI](https://github.com/USER/REPO/raw/main/screenshots/12-gui-fa-main.png)
![Web](https://github.com/USER/REPO/raw/main/screenshots/01-web-tab-sazesh.png)

Full change list: https://github.com/USER/REPO/blob/main/CHANGELOG.md
EOF

gh release create "v$V" \
  "dist/parsi-go-win64-v$V.zip" \
  "dist/parsi-go-model-onnx-v$V.zip" \
  --title "v$V — Windows 7 build + model" \
  --notes-file dist/RELEASE_NOTES.md
```

Remember to replace `USER`/`REPO` inside the notes (the heredoc keeps `$V`
expansion but leaves `USER`/`REPO` as literal text to search-replace, or just
edit the file before running `gh release create`). If the release already
exists, use `gh release upload v1.1.0 <file>` to add/replace one asset.

### 4.3 Optional: host the model files on your own release

The GUI’s download tab reads `models.json` (one entry per file: `name`, `dir`,
`kind`, `size`, `url`, `desc`). By default the `url`s point to HuggingFace.
To let the app download **from your GitHub release** instead:

```bash
# 1) upload every file named in models.json as a release asset
python3 - <<'PY'
import json, pathlib, subprocess
for it in json.load(open("models.json"))["items"]:
    p = pathlib.Path(it["dir"]) / it["name"]
    if p.exists():
        subprocess.run(["gh", "release", "upload", "v1.1.0", str(p)], check=True)
    else:
        print("MISSING (fetch first):", p)
PY

# 2) repoint every url at the release — only if nothing printed MISSING
python3 - <<'PY'
import json
USER, REPO, TAG = "USER", "offline-persian-tts-win7", "v1.1.0"
data = json.load(open("models.json"))
for it in data["items"]:
    it["url"] = f"https://github.com/{USER}/{REPO}/releases/download/{TAG}/{it['name']}"
json.dump(data, open("models.json", "w"), ensure_ascii=False, indent=2)
print("rewrote", len(data["items"]), "urls")
PY

git add models.json && git commit -m "models.json: point at the v1.1.0 release assets"
git push
```

(Replace `USER`/`REPO` in step 2 before running it. Public release assets
download without any login, exactly like the HuggingFace URLs.)

## 5. Screenshots

- The 18 PNGs live in [`screenshots/`](screenshots) and are **already committed
  in step 2** — the README embeds them with relative paths, so they render on
  the repository home page automatically.
- In **release notes**, use absolute URLs (the template in 4.2 does):
  `https://github.com/USER/REPO/raw/main/screenshots/<file>.png`
- Regenerate:
  - **web UI shots (01–11):** `python3 scripts/test_web_ui.py` rewrites them
    every run (server must be up, see step 3);
  - **GUI shots (12–18):** run `farsi-tts-gui.exe`, visit each tab (both
    languages) and take a screenshot — crop to the window for a tidy gallery.
- They are small (~1.3 MB total), so keeping them in git is cheap.

## 6. Verify the publication

```bash
gh repo view USER/REPO --web            # README images render, tree complete
gh release view v1.1.0                  # both assets listed with sane sizes
gh release download v1.1.0 --dir /tmp/check && ls -lh /tmp/check
```

Then on a Windows machine: unzip both archives into one folder, start
`farsi-tts-gui.exe`, press «ساخت گفتار» — speech should come out. On a fresh
clone the build from step 3 must also succeed:

```bash
git clone https://github.com/USER/REPO.git && cd REPO && ./scripts/build_win_mingw.sh
```

## 7. Checklist for the next version

1. Bump the version (`grep -rn "1.1.0" src/`) and rebuild (step 3).
2. Run all three gates (step 3) — must stay green.
3. Add a section to [`CHANGELOG.md`](CHANGELOG.md).
4. Re-package the archives with the new `V=` (step 4.1).
5. `gh release create "vNEXT" … --notes-file …` (step 4.2), including fresh
   screenshots if the UI changed.
6. `git add -A && git commit && git push`.
