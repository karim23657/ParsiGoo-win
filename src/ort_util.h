// Minimal ONNX Runtime wrapper built on the *C* API only.
//
// The C API is ABI-stable across ONNX Runtime releases, which is what lets the
// same engine source compile against a modern ORT on Linux and an older,
// Windows-7-compatible ORT (<= 1.14) on the target machine. Only functions
// present since the 1.x line are used.
#pragma once

// windows.h (for WINAPI/ORT_API_CALL) + SAL shim for non-MSVC toolchains
#include "ort_sal_compat.h"
#include <onnxruntime_c_api.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tts {

const OrtApi* ort();  // process-wide OrtApi singleton
OrtEnv* ort_env();

// Borrowed-data tensor creators (ORT reads the caller's buffer during Run).
OrtValue* make_tensor(const std::vector<int64_t>& shape, void* data, ONNXTensorElementDataType type);

void ort_release(OrtValue* v);
void* tensor_data(const OrtValue* v);
std::vector<int64_t> tensor_shape(const OrtValue* v);
ONNXTensorElementDataType tensor_type(const OrtValue* v);

class Session {
public:
    Session(const std::string& path, int intra_threads = 2, int inter_threads = 1);
    ~Session();

    const std::vector<std::string>& in_names() const { return in_; }
    const std::vector<std::string>& out_names() const { return out_; }

    // Runs with positional inputs; returns freshly allocated outputs owned by
    // the caller (release with ort_release).
    std::vector<OrtValue*> run(const std::vector<const char*>& names,
                               const std::vector<OrtValue*>& values);
    // Feeds every input in the session's own order (values.size() == in count).
    std::vector<OrtValue*> run_ordered(const std::vector<OrtValue*>& values);

private:
    OrtSession* s_ = nullptr;
    std::vector<std::string> in_, out_;
};

}  // namespace tts
