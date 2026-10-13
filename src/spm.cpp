#include "spm.h"

#include <sentencepiece_processor.h>

#include <stdexcept>

namespace tts {

struct Spm::Impl {
    sentencepiece::SentencePieceProcessor sp;
};

Spm::Spm(const std::string& model_path) : impl_(new Impl()) {
    auto st = impl_->sp.Load(model_path);
    if (!st.ok()) throw std::runtime_error("sentencepiece: cannot load " + model_path + ": " + st.ToString());
}

Spm::~Spm() = default;

std::vector<int> Spm::encode(const std::string& text) const {
    std::vector<int> ids;
    auto st = impl_->sp.Encode(text, &ids);
    if (!st.ok()) throw std::runtime_error("sentencepiece: encode failed: " + st.ToString());
    return ids;
}

int Spm::size() const { return impl_->sp.GetPieceSize(); }

}  // namespace tts
