// Text/phoneme chunking — port of the plan/chunk layer in scripts/tts_onnx.py.
//
// The model is trained on ~11-token utterances; this layer splits Persian text
// into punctuation-aware phrases, phonemises each, and packs the phoneme words
// into model-sized chunks that never split a bound phrase.
#pragma once

#include <string>
#include <vector>

#include "g2p.h"
#include "spm.h"

namespace tts {

class Dictionary;

struct Phrase {
    std::string phonemes;  // ezafe markers already stripped
    double gap = 0.0;      // pause before this phrase, seconds
};

// Pause lengths used across the pipeline (seconds).
constexpr double STRONG_GAP = 0.26;
constexpr double PHRASE_GAP = 0.16;
constexpr double SENTENCE_GAP = 0.45;
constexpr double CHUNK_GAP = 0.12;

std::vector<std::string> split_sentences(const std::string& text);

// One text sentence -> [(phonemes, gap_before), ...].
//   dict         optional pronunciation dictionary (nullptr = plain G2P)
//   keep_ezafe   keep the "1" ezafe marker in the phoneme strings
//   chunk_tokens SentencePiece budget used by the "does this phrase already
//                fit in one chunk" test (0 = the model default, 18)
std::vector<Phrase> plan_phrases(const std::string& sentence, G2P& g2p, const Spm& sp,
                                 const Dictionary* dict = nullptr, bool keep_ezafe = false,
                                 int chunk_tokens = 0);

// «یکپارچه» mode: merge comma-delimited phrases into longer breathing units.
std::vector<Phrase> pack_phrases(const std::vector<Phrase>& plan);

// Word-boundary packing of a phoneme string into model-sized chunks.
std::vector<std::string> chunk_phonemes(const std::string& phonemes, const Spm& sp, int max_tokens = 18);

}  // namespace tts
