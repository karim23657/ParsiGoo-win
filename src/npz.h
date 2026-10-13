// Minimal NumPy .npy / .npz reader, no external dependencies.
//
// The model package ships two archives:
//   weights.npz            host constants (LUT, speaker_proj, BOS, stats)
//   decode_state_init.npz  the 27 initial Mimi-decoder state tensors
//
// Both are written by numpy with ZIP_STORED (compress_type 0), so a plain
// central-directory walk is enough — no inflate, no zlib. Only the dtypes the
// package actually uses are supported: <f4, <f8, <i8, |b1, <f2.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tts {

enum class DType { F32, F64, I64, BOOL, F16, UNKNOWN };

struct Tensor {
    DType dtype = DType::UNKNOWN;
    std::vector<int64_t> shape;
    std::vector<uint8_t> data;  // raw bytes, little-endian on disk

    int64_t numel() const {
        int64_t n = 1;
        for (auto d : shape) n *= d;
        return n;
    }
    const float* f32() const { return reinterpret_cast<const float*>(data.data()); }
    const int64_t* i64() const { return reinterpret_cast<const int64_t*>(data.data()); }
    const uint8_t* b1() const { return data.data(); }
    float* f32m() { return reinterpret_cast<float*>(data.data()); }
};

// Reads one .npy blob (magic + header + payload) into a Tensor.
// Throws std::runtime_error on malformed input.
Tensor parse_npy(const uint8_t* buf, size_t len);

// An .npz is a stored zip of .npy members. Members are returned in central
// directory order, which is exactly the order numpy's `np.load(...).files`
// reports — the Mimi decoder state mapping depends on that order.
struct Npz {
    std::vector<std::string> names;  // entry names without the ".npy" suffix
    std::vector<Tensor> tensors;
    const Tensor& operator[](const std::string& name) const;
    const Tensor& at(size_t i) const { return tensors.at(i); }
    size_t size() const { return tensors.size(); }
};

Npz load_npz(const std::string& path);

}  // namespace tts
