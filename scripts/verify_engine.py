# End-to-end verification of the native C++ engine against the reference
# Python engine in ref/.  Run after scripts/build_linux.sh.
#
#   python3 scripts/verify_engine.py
import subprocess
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

sys.path.insert(0, "ref")
sys.path.insert(0, "model/v2")
from tts_onnx import OnnxTts  # noqa: E402
from g2p_onnx import OnnxG2P  # noqa: E402

BASE = Path(".").resolve()
CLI = BASE / "build" / "farsi-tts"
VOICE = "voices/male_hello.wav"

SENTENCES = [
    "سلام، حال شما چطور است؟",
    "اقتصاد آمریکا را تا سال ۲۰۳۰ تغییر دهد.",
    "دانشگاه تهران در سال ۱۳۱۳ تأسیس شد.",
    "مادر کتاب را روی میز اتاق گذاشت و پنجره را باز کرد.",
    "امروز هوا خیلی خوب است و من می‌خواهم بیرون بروم.",
]
PHONEMES = [
    "salAm hAle SomA Cetor ?ast",
    "?eqtesAde ?AmrikA rA tA sAle do hezAr ?o si taqir dahad",
    "sistemhAye1 ?ettesAlAte1 momken",
    "dAneshgAhe1 tehrAn va1 mA dar1 ruye1 vindoz",
]

results = []


def run(args, env=None):
    import os
    e = dict(os.environ)
    if env:
        e.update(env)
    return subprocess.run([str(CLI), "--base", str(BASE), *args],
                          capture_output=True, text=True, env=e)


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}  {detail}")


def main():
    eng = OnnxTts(seed=1)
    g2p = OnnxG2P(BASE / "model" / "onnx")

    print("1. Persian G2P (phoneme strings must match exactly)")
    for s in SENTENCES:
        out = run(["--phonemes", s]).stdout.strip()
        ref = g2p.phonemise(s)
        check(f"g2p: {s[:22]}…", out == ref, "" if out == ref else f"cpp={out!r} ref={ref!r}")

    print("2. Phrase planning + phoneme chunking")
    for ph in PHONEMES:
        out = run(["--chunks", ph]).stdout.strip().splitlines()
        ref = eng.chunk_phonemes(ph)
        check(f"chunks: {ph[:22]}…", out == ref, "" if out == ref else f"cpp={out} ref={ref}")

    print("3. FlowLM latents (bit-exact given identical conditioning, zero noise)")
    te = Path("/tmp/te.f32")
    raw = Path("/tmp/cpp_verify")
    run(["--voice", VOICE, "--zero-noise", "--seed", "1", "--dump-raw", str(raw), PHONEMES[0]],
        env={"FARSI_DUMP_TEXTEMB": str(te)})
    text_emb = np.fromfile(te, dtype=np.float32).reshape(1, -1, eng.dim)
    cache = np.zeros((eng.L, 2, eng.CAP, eng.H, eng.D), np.float32)
    _, _, cache, off = eng._flow_step(np.zeros((1, 0, eng.ldim), np.float32), text_emb, 0,
                                      np.zeros((1, eng.ldim), np.float32), cache)
    tokens = eng.sp.encode(PHONEMES[0], out_type=int)
    lat, eos, cache, off = eng._flow_step(
        np.full((1, 1, eng.ldim), np.nan, np.float32),
        eng.lut[np.asarray(tokens)][None].astype(np.float32), off,
        np.zeros((1, eng.ldim), np.float32), cache)
    latents = [lat]
    frames_after_eos = 5
    max_len = int(np.ceil((len(tokens) / eng.tps_est + eng.gen_pad) * eng.frame_rate))
    eos_step = None
    for step in range(max_len):
        lat, eos, cache, off = eng._flow_step(latents[-1].reshape(1, 1, eng.ldim),
                                              np.zeros((1, 0, eng.dim), np.float32), off,
                                              np.zeros((1, eng.ldim), np.float32), cache)
        if bool(eos.reshape(-1)[0]) and eos_step is None:
            eos_step = step
        if eos_step is not None and step >= eos_step + frames_after_eos:
            break
        latents.append(lat)
    py = np.concatenate([l.reshape(-1) for l in latents]).astype(np.float32)
    cpp = np.fromfile(str(raw) + ".lat", dtype=np.float32)
    d = np.abs(py[:min(len(py), len(cpp))] - cpp[:min(len(py), len(cpp))])
    check("flow latents bit-exact", float(d.max()) == 0.0, f"max diff={float(d.max()):.2e}")

    print("4. Mimi decoder (given identical latents)")
    aud = eng._decode_all(np.fromfile(str(raw) + ".lat", dtype=np.float32).reshape(-1, 32))
    sf.write("/tmp/py_dec.wav", aud, 24000)
    a, _ = sf.read(str(raw) + ".raw.wav")
    n = min(len(a), len(aud))
    # the C++ side is a 16-bit WAV, which 
    # clips the model's occasional >1.0 peaks
    dd = np.abs(a[:n] - np.clip(aud[:n], -1.0, 1.0))
    check("decoder matches to 16-bit", float(dd.max()) < 2e-3, f"max diff={float(dd.max()):.2e}")

    ok = all(r[1] for r in results)
    print(f"\n{sum(r[1] for r in results)}/{len(results)} checks passed")
    print("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
