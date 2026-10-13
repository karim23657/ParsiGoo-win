// Thin wrapper around libsentencepiece (the phone tokenizer).
//
// The phoneme string produced by the G2P is tokenized by the model's
// SentencePiece model (model/v2/tokenizer_ph.model); the resulting ids index
// the 4001x1024 LUT that the FlowLM consumes, and their count drives the
// chunking budget.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace tts {

class Spm {
public:
    explicit Spm(const std::string& model_path);
    ~Spm();

    // Token ids for `text` (no BOS/EOS added unless the model does so itself).
    std::vector<int> encode(const std::string& text) const;
    int size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tts
