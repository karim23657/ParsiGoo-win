# Verification harness: runs the *reference* ONNX engine with all flow-LM
# noise forced to zero, so the native engine's --zero-noise run can be compared
# against it sample-by-sample. Also prints the phoneme string and chunk plan.
#
#   python3 scripts/verify_ref.py "سلام، حال شما چطور است؟" voices/male_hello.wav out_ref.wav
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

sys.path.insert(0, "ref")
sys.path.insert(0, "model/v2")
from tts_onnx import OnnxTts, plan_phrases, split_sentences, CHUNK_TOKENS  # noqa: E402
from g2p_onnx import OnnxG2P  # noqa: E402


def main():
    text = sys.argv[1] if len(sys.argv) > 1 else "سلام، حال شما چطور است؟"
    voice = sys.argv[2] if len(sys.argv) > 2 else "voices/male_hello.wav"
    out = sys.argv[3] if len(sys.argv) > 3 else "output/ref_zero.wav"

    eng = OnnxTts(seed=1234)
    eng._g2p = OnnxG2P(Path("model/onnx"))

    orig_flow = eng._flow_step

    def zero_flow(seq, text_emb, offset, noise, cache):
        return orig_flow(seq, text_emb, offset, np.zeros_like(noise), cache)

    eng._flow_step = zero_flow

    # phonemes + chunk plan
    pers = any("\u0600" <= c <= "\u06ff" for c in text)
    if pers:
        plan = plan_phrases(split_sentences(text)[0], eng._g2p, eng.sp)
        print("PHONEMES:", " ".join(p for p, _ in plan))
        chunks = []
        for p, g in plan:
            chunks.extend(eng.chunk_phonemes(p))
        print("CHUNKS:", " || ".join(chunks))
    else:
        print("PHONEMES:", text)
        print("CHUNKS:", " || ".join(eng.chunk_phonemes(text)))

    audio = eng.synthesize_text(text, voice, mode="split")
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    sf.write(out, audio, eng.sample_rate)
    print(f"WROTE {out} samples={len(audio)} sr={eng.sample_rate} peak={float(np.abs(audio).max()):.6f}")


if __name__ == "__main__":
    main()
