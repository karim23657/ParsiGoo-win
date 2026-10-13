import sys
from pathlib import Path
import numpy as np
sys.path.insert(0, "ref"); sys.path.insert(0, "model/v2")
from tts_onnx import OnnxTts  # noqa

eng = OnnxTts(seed=1)
chunk = sys.argv[1] if len(sys.argv) > 1 else "salAm hAle SomA Cetor ?ast"

# C++-dumped voice conditioning [1, T+1, 1024]
te = np.fromfile("/tmp/textemb.f32", dtype=np.float32).reshape(1, -1, eng.dim)
cache = np.zeros((eng.L, 2, eng.CAP, eng.H, eng.D), np.float32)
lat, eos, cache, off = eng._flow_step(np.zeros((1, 0, eng.ldim), np.float32), te, 0,
                                      np.zeros((1, eng.ldim), np.float32), cache)
print("voice off", off)

tokens = eng.sp.encode(chunk, out_type=int)
text_emb = eng.lut[np.asarray(tokens)][None].astype(np.float32)
noise = np.zeros((1, eng.ldim), np.float32)
lat, eos, cache, off = eng._flow_step(np.full((1, 1, eng.ldim), np.nan, np.float32), text_emb, off, noise, cache)
latents = [lat]
words = len(chunk.split())
frames_after_eos = (3 if words <= 4 else 1) + 2
max_gen_len = int(np.ceil((len(tokens) / eng.tps_est + eng.gen_pad) * eng.frame_rate))
eos_step = None
for step in range(max_gen_len):
    lat, eos, cache, off = eng._flow_step(latents[-1].reshape(1, 1, eng.ldim),
                                          np.zeros((1, 0, eng.dim), np.float32), off, noise, cache)
    if bool(eos.reshape(-1)[0]) and eos_step is None:
        eos_step = step
    if eos_step is not None and step >= eos_step + frames_after_eos:
        break
    latents.append(lat)
py = np.concatenate([l.reshape(-1) for l in latents]).astype(np.float32)
cpp = np.fromfile("/tmp/cppraw.lat", dtype=np.float32)
print("py latents", py.shape, "cpp latents", cpp.shape)
n = min(len(py), len(cpp))
d = np.abs(py[:n] - cpp[:n])
print("max diff", float(d.max()), "mean", float(d.mean()))
print("PASS" if d.max() < 1e-3 else "DIFFERS")
