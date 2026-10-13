// Persian text normalization — a faithful C++ port of model/v2/normalize_fa.py
// (the subset the ONNX engine path uses: normalize_for_model / is_phonemic).
//
// The reference pipeline normalizes Persian script onto one canonical spelling
// before G2P: fold Arabic letter forms, drop diacritics/invisibles, spell out
// numbers, map punctuation, and strip anything outside the allowed alphabet.
// Phoneme (romanized) input passes through untouched.
#pragma once

#include <string>

namespace tts {

// True when `text` already looks like phonemes rather than Persian script.
bool is_phonemic(const std::string& text);

// Full normalization (numbers spelled out). Returns "" for input that is
// entirely outside the alphabet.
std::string normalize_text(const std::string& text);

// normalize_for_model: phonemes pass through, Persian is normalized, and
// normalization emptying non-empty text is reported instead of returning "".
std::string normalize_for_model(const std::string& text);

// True when `word` carries at least one letter or digit (a lone dash or "..."
// is not a word) — mirrors Python's `any(ch.isalnum())` on a token.
bool fa_has_alnum(const std::string& word);

}  // namespace tts
