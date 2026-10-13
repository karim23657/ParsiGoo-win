// Grapheme-to-phoneme: Persian text -> romanized phoneme string.
//
// A port of scripts/g2p_onnx.py: ByT5 byte tokenizer (id = byte + 3), the ONNX
// T5 encoder/decoder with host-side greedy decoding, Latin transliteration and
// the long-input window split.
#pragma once

#include <string>
#include <vector>

#include "ort_util.h"

namespace tts {

// Latin -> Persian transliteration only (no G2P). Lets callers count the words
// the G2P will actually see.
std::string transliterate_text(const std::string& text);

class G2P {
public:
    G2P(const std::string& enc_onnx, const std::string& dec_onnx);

    // Persian text (or phonemes, passed through) -> phoneme string.
    // keep_ezafe keeps the "1" ezafe marker.
    std::string phonemise(const std::string& text, bool keep_ezafe = false);

private:
    std::string decode_greedy(const std::string& text);
    Session enc_, dec_;
    int max_len_ = 512;
};

}  // namespace tts
