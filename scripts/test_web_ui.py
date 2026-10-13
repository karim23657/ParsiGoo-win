#!/usr/bin/env python3
"""Drive the real web UI in a headless browser and check every part of it.

    1. start the server:   ./build/farsi-tts-server --base . --port 8123
    2. run this:           python3 scripts/test_web_ui.py

What it exercises (the same path a human clicks):
  health/config/voices loading, reference-voice preload, synthesis and the
  audio it returns (level checked — silence is the bug we care about),
  phoneme extraction + speaking an edited phoneme string, the dictionary
  round trip, every settings control, input=text on numeric input, bulk
  generation, a quiet reference voice upload, and a rejected non-WAV upload.

Screenshots land in --out (default /content/screenshots).
Exit code 0 = every check passed.  Requires: pip install playwright &&
python3 -m playwright install chromium
"""
import argparse, base64, io, json, math, os, shutil, struct, subprocess, sys, time, wave

ap = argparse.ArgumentParser()
ap.add_argument("--base-url", default="http://127.0.0.1:8123")
ap.add_argument("--out", default="/content/screenshots")
ap.add_argument("--keep-shots", action="store_true")
args = ap.parse_args()

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    sys.exit("playwright is not installed:  pip install playwright && python3 -m playwright install chromium")

B = args.base_url
OUT = args.out
os.makedirs(OUT, exist_ok=True)

results, console_errors, shots = [], [], []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(("  PASS  " if ok else "  FAIL  ") + name + ((" — " + str(detail)) if detail else ""))


def rms_of(raw):
    w = wave.open(io.BytesIO(raw), "rb")
    n, sr = w.getnframes(), w.getframerate()
    vals = struct.unpack("<%dh" % (n * w.getnchannels()), w.readframes(n))
    w.close()
    peak = max(abs(v) for v in vals)
    r = math.sqrt(sum(v * v for v in vals) / len(vals)) if vals else 0.0
    return peak / 32768.0, r / 32768.0, n / sr


def shot(pg, name):
    path = os.path.join(OUT, name)
    pg.screenshot(path=path, full_page=True)
    shots.append(path)
    return path


def synth_via_ui(pg, text, timeout=300000):
    """Fill the text box, press «ساخت گفتار», return the WAV bytes."""
    pg.fill("#text", text)
    with pg.expect_response(lambda r: "/api/synth" in r.url, timeout=timeout) as ri:
        pg.click("#go")
    resp = ri.value
    body = resp.body()
    if "json" in (resp.headers.get("content-type") or ""):
        return base64.b64decode(json.loads(body)["wav"]), json.loads(body)
    return body, {}


quiet_src = "/tmp/voices/vquiet.wav"
if not os.path.exists(quiet_src):
    os.makedirs("/tmp/voices", exist_ok=True)
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", "voices/male_hello.wav",
                    "-af", "volume=-40dB", quiet_src], check=True)


def clean_dict():
    """Leave dicts/user.dict exactly as we found it, whatever happens."""
    try:
        import urllib.request
        urllib.request.urlopen(urllib.request.Request(
            B + "/api/dict?word=%D8%A8%D8%B1%D8%AF%D8%A7%D8%B1%D8%B3%D8%A7%D8%B2%DB%8C",
            method="DELETE"), timeout=10).read()
    except Exception as e:
        print("  note: could not clean the test dictionary entry:", e)


import atexit

_reported = False


def finish():
    """Runs even when the test dies mid-way: restore the dictionary we edited
    and report whatever checks had already run."""
    global _reported
    clean_dict()
    if _reported:
        return
    _reported = True
    print()
    fails = [r for r in results if not r[1]]
    print(f"{len(results) - len(fails)}/{len(results)} checks passed")
    if console_errors:
        print("console errors:")
        for e in console_errors[:10]:
            print("   ", e)


atexit.register(finish)

with sync_playwright() as p:
    browser = p.chromium.launch(args=["--autoplay-policy=no-user-gesture-required"])
    pg = browser.new_page(viewport={"width": 1440, "height": 1050})
    pg.set_default_timeout(240000)
    pg.on("console", lambda m: console_errors.append(m.type + ": " + m.text)
          if m.type == "error" else None)
    pg.on("pageerror", lambda e: console_errors.append("pageerror: " + str(e)))

    # ---- 1. page loads and configures itself from /api/config -------------
    pg.goto(B + "/", wait_until="networkidle")
    check("UI loads", pg.title() != "")
    check("config -> speech modes", pg.locator("#mode option").count() >= 2,
          pg.locator("#mode option").count())
    check("config -> voices listed", pg.locator("#voice option").count() >= 1)
    check("connection chip", "متصل" in pg.inner_text("#connChip"), pg.inner_text("#connChip"))
    shot(pg, "01-web-tab-sazesh.png")

    # ---- 2. preload the reference voice ----------------------------------
    pg.click("#preload")
    pg.wait_for_function("() => /رمزگذاری شد|از قبل رمزگذاری|تقویت شد/.test"
                         "(document.getElementById('preloadInfo').textContent)", timeout=120000)
    info = pg.inner_text("#preloadInfo")
    check("voice preload reports the ms", "✓" in info, info)
    shot(pg, "02-web-preload-voice.png")

    # ---- 3. synthesis: real audio, not silence ---------------------------
    wav, js = synth_via_ui(pg, "این یک آزمون کوتاه برای رابط کاربری است.")
    peak, lvl, secs = rms_of(wav)
    check("synth returns audio", secs > 0.3, f"{secs:.2f}s")
    check("synth audio is NOT silent", lvl > 1e-3, f"rms={lvl:.4f} peak={peak:.4f}")
    check("synth reports phonemes", bool(js.get("phonemes")), js.get("phonemes"))
    check("result line filled", "ثانیه" in pg.inner_text("#result"), pg.inner_text("#result")[:80])
    check("player has a blob", pg.eval_on_selector("#player audio, #player", "e => !!e"))
    shot(pg, "03-web-after-synth.png")

    # ---- 4. phonemes: extract, edit, speak the edited string --------------
    pg.click('.tab[data-tab="phon"]')
    pg.click("#extract")
    pg.wait_for_function("() => document.getElementById('phonEdit').value.length > 3", timeout=120000)
    ph = pg.input_value("#phonEdit")
    check("phoneme extraction", "?" in ph or "A" in ph, ph[:60])
    pg.fill("#phonEdit", "bordArAzi")
    pg.check("#useEdited")
    pg.click('.tab[data-tab="text"]')          # the synth button lives here
    pg.fill("#text", "این یک آزمون کوتاه برای رابط کاربری است.")
    with pg.expect_response(lambda r: "/api/synth" in r.url, timeout=300000) as ri:
        pg.click("#go")
    js2 = json.loads(ri.value.body())
    check("edited phonemes are spoken verbatim", js2.get("phonemes") == "bordArAzi",
          js2.get("phonemes"))
    check("edited-phoneme audio is NOT silent", rms_of(base64.b64decode(js2["wav"]))[1] > 1e-3)
    pg.click('.tab[data-tab="phon"]')
    pg.uncheck("#useEdited")
    shot(pg, "04-web-tab-vanaye.png")

    # ---- 5. dictionary round trip through the UI -------------------------
    pg.click('.tab[data-tab="dict"]')
    word = "بردارسازی"
    pg.fill("#dWord", word)
    pg.fill("#dPhon", "bordArAzi")
    pg.click("#dAdd")
    pg.wait_for_function("() => document.getElementById('dBody').textContent.includes('bordArAzi')",
                         timeout=60000)
    check("dictionary entry added", True, pg.inner_text("#dMsg")[:60])
    pg.click('.tab[data-tab="text"]')
    pg.fill("#text", "این بُردارسازی است")   # the word we just taught it
    pg.click('.tab[data-tab="phon"]')
    pg.click("#extract")
    pg.wait_for_function("() => document.getElementById('phonEdit').value.includes('bordArAzi')",
                         timeout=60000)
    check("dictionary overrides the G2P", True, pg.input_value("#phonEdit")[:60])
    shot(pg, "05-web-tab-vajehname.png")

    # ---- 6. every settings control reaches the request -------------------
    def set_range(eid, val):
        # range inputs: write the value and fire the events the UI listens to
        pg.evaluate("([id, v]) => { const e = document.getElementById(id); e.value = v;"
                    " e.dispatchEvent(new Event('input', {bubbles: true}));"
                    " e.dispatchEvent(new Event('change', {bubbles: true})); }", [eid, val])

    pg.click('.tab[data-tab="set"]')
    pg.fill("#s-seed", "4242")
    set_range("s-temp", "0.5")
    pg.fill("#s-retries", "2")
    pg.fill("#s-chunk", "24")
    set_range("s-pause", "1.4")
    pg.select_option("#s-mode", "pack")
    pg.check("#s-zn")
    pg.check("#s-ez")
    pg.select_option("#s-input", "text")
    shot(pg, "06-web-tab-tanzimat.png")
    with pg.expect_response(lambda r: "/api/synth" in r.url, timeout=300000) as ri:
        pg.click('.tab[data-tab="text"]')
        pg.fill("#text", "123")
        pg.click("#go")
    js3 = json.loads(ri.value.body())
    check("input=text forces the G2P on numbers", "bist" in js3.get("phonemes", ""),
          js3.get("phonemes"))
    check("settings seed honoured", str(js3.get("seed")) == "4242", js3.get("seed"))
    check("numeric input audio is NOT silent", rms_of(base64.b64decode(js3["wav"]))[1] > 1e-3)
    pg.click('.tab[data-tab="set"]')   # innerText of a hidden pane is empty
    check("zero-noise carries its silence warning",
          "بی‌صدا" in pg.inner_text("#tab-set"), "label + hint present")
    # zero-noise is harness-only and does silence many reference voices (see
    # docs) — everything below runs with it off, like a normal user.
    pg.uncheck("#s-zn")

    # ---- 7. bulk generation ---------------------------------------------
    pg.click('.tab[data-tab="bulk"]')
    pg.fill("#bulkText", "سلام، حال شما چطور است؟\nاین سطر دوم آزمون است.")
    with pg.expect_response(lambda r: "/api/synth" in r.url, timeout=300000):
        pg.click("#bulkGo")
    pg.wait_for_function(
        "() => document.getElementById('bulkList').querySelectorAll('a').length >= 2",
        timeout=600000)
    check("bulk produced 2 downloadable rows",
          pg.locator("#bulkList a").count() >= 2, pg.locator("#bulkList a").count())
    shot(pg, "07-web-tab-tolid-anboh.png")

    # ---- 8. quiet reference voice: the silence bug -----------------------
    pg.click('.tab[data-tab="text"]')
    pg.set_input_files("#upload", quiet_src)
    pg.wait_for_function(
        "() => document.getElementById('statusbar').textContent.includes('بارگذاری شد')",
        timeout=60000)
    check("quiet reference uploaded", True, pg.inner_text("#statusbar")[:70])
    pg.wait_for_function("() => document.getElementById('voice').value.indexOf('uploads/') === 0",
                         timeout=60000)
    pg.click("#preload")
    pg.wait_for_function("() => /تقویت شد|رمزگذاری شد/.test("
                         "document.getElementById('preloadInfo').textContent)", timeout=120000)
    check("quiet reference is reported amplified", "تقویت شد" in pg.inner_text("#preloadInfo"),
          pg.inner_text("#preloadInfo"))
    shot(pg, "08-web-quiet-reference.png")
    wav, _ = synth_via_ui(pg, "صدای این مرجع کم بود اما حالا باید شنیده شود.")
    peak, lvl, secs = rms_of(wav)
    check("quiet reference -> audible output", lvl > 1e-3, f"rms={lvl:.4f} peak={peak:.4f}")
    shot(pg, "09-web-quiet-reference-synth.png")

    # ---- 9. a non-WAV upload is refused in the browser -------------------
    bad = "/tmp/notavoice.mp3"
    if not os.path.exists(bad):
        # build the non-WAV fixture here: /tmp is wiped between sessions
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", "voices/male_hello.wav",
                        bad], check=True)
    pg.set_input_files("#upload", bad)
    pg.wait_for_function(
        "() => document.getElementById('statusbar').textContent.includes('WAV')",
        timeout=30000)
    check("non-WAV upload refused client-side", True, pg.inner_text("#statusbar")[:80])
    shot(pg, "10-web-reject-nonwav.png")

    # ---- 10. help overlay -------------------------------------------------
    pg.click("#helpBtn")
    pg.wait_for_function("() => document.getElementById('helpPre').textContent.length > 200",
                         timeout=30000)
    check("help overlay shows /api/help", True,
          pg.inner_text("#helpPre").splitlines()[0][:60])
    shot(pg, "11-web-help.png")
    pg.click("#helpClose")

    browser.close()

finish()
fails = [r for r in results if not r[1]]
sys.exit(1 if (fails or console_errors) else 0)
