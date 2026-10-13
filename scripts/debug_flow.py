import sys
from pathlib import Path
import numpy as np
sys.path.insert(0, "ref"); sys.path.insert(0, "model/v2")
from tts_onnx import OnnxTts, plan_phrases, split_sentences  # noqa
from g2p_onnx import OnnxG2P  # noqa

text = sys.argv[1] if len(sys.argv) > 1 else "سلام، حال شما چطور است؟"
voice = sys.argv[2] if len(sys.argv) > 2 else "voices/male_hello.wav"

eng = OnnxTts(seed=1)
eng._g2p = OnnxG2P(Path("model/onnx"))

orig = eng._flow_step
step_no = [0]
def dbg(seq, text_emb, offset, noise, cache):
    lat, eos, cache, off = orig(seq, text_emb, offset, noise, cache)
    l = lat.reshape(-1)
    print(f"[dbg] flowcall {step_no[0]} seqlen={seq.shape[1]} textlen={text_emb.shape[1]} off={offset} eos={float(eos.reshape(-1)[0]):.4f} lat0={l[0]:.5f} {l[1]:.5f} {l[2]:.5f}")
    print(f"[dbg] pycache [{cache[0,0,offset,0,0]:.6f}] sum={float(np.abs(cache.astype(np.float64)).sum()):.1f}")
    step_no[0] += 1
    return lat, eos, cache, off

class Z:
    def standard_normal(self, shape):
        return np.zeros(shape, np.float32)

eng._flow_step = dbg
# zero noise path: replace _generate_chunk's rng by a zero rng
import soundfile as sf
from scipy.signal import resample_poly
from tts_onnx import trim_hot_onset
audio, sr = sf.read(voice)
if audio.ndim > 1: audio = audio.mean(axis=1)
audio = audio.astype(np.float32)
if len(audio) > 5 * eng.sample_rate: audio = audio[: 5 * eng.sample_rate]
audio, _ = trim_hot_onset(audio, eng.sample_rate)
lat = eng.s_enc.run(None, {"audio": audio[None, None, :]})[0]
cond = (lat[0] @ eng.spk_proj.T).astype(np.float32)
print("[dbg] cond0=", cond[0, 0], cond[0, 1], "cond_sum=", float(np.abs(cond.astype(np.float64)).sum()))
cache, off = eng.voice_cache(voice)
print("[dbg] voice off=", off, "cache sum=", float(np.abs(cache.astype(np.float64)).sum()))
plan = plan_phrases(split_sentences(text)[0], eng._g2p, eng.sp)
chunks = []
for p, g in plan:
    chunks.extend(eng.chunk_phonemes(p))
print("[dbg] chunks:", chunks)
toks = eng.sp.encode(chunks[0], out_type=int)
print("[dbg] tokens:", toks)
te = eng.lut[np.asarray(toks)][None].astype(np.float32)
print("[dbg] te0=", te[0,0,0], te[0,0,1], te[0,0,2], "cache_sum=", float(np.abs(cache.astype(np.float64)).sum()))
latents, _, _ = eng._generate_chunk(cache.copy(), off, chunks[0], rng=Z())
print("[dbg] n_latents=", len(latents))
