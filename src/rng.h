// Small deterministic RNG for the flow-LM noise.
//
// Reproducible on every platform/compiler (xorshift64* + Box-Muller), so a
// fixed --seed yields the same audio anywhere. It is NOT numpy's generator:
// the reference engine's noise cannot be reproduced bit-for-bit, which is why
// the verification harness compares against a zero-noise run instead.
#pragma once

#include <cstdint>
#include <vector>

namespace tts {

class Rng {
public:
    Rng();
    explicit Rng(uint64_t seed) : state_(seed ? seed : 0x9E3779B97F4A7C15ULL) {}

    uint64_t next_u64();
    // Standard normal samples (mean 0, std 1).
    std::vector<float> normal(int n);

private:
    uint64_t state_ = 0x9E3779B97F4A7C15ULL;
};

}  // namespace tts
